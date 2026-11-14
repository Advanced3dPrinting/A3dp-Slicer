///|/ Copyright (c) 2026 Advanced3dPrinting LLC
///|/
///|/ A3dp-Slicer is released under the terms of the AGPLv3 or higher
///|/
#include "Snapmaker.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <regex>
#include <sstream>
#include <tuple>
#include <atomic>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <curl/curl.h>
// The websocket API (curl_ws_send/recv) is stable since libcurl 8.11.0. The
// fork's bundled deps curl is 7.75.0 (no websocket support at all), so the
// websocket path compiles out there and the plain upload+print fallback is
// the only code path built by CI. Local builds (system libcurl >= 8.11) get
// the full feature.
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x080B00
#define SNAPMAKER_HAS_CURL_WS 1
#include <curl/websockets.h>
#endif

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include "Http.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

namespace {

// Ranks a candidate that declares no usable colour behind every candidate that does, while
// still leaving it ahead of no match at all. The largest real distance is 3 * 255^2 = 195075.
constexpr unsigned int UNRANKED_COLOR_DISTANCE = 0xfffffffeu;

// filament_vendor of the "Generic ..." filament presets, so vendor-less spools
// still find a neutral preset with sane temperatures.
constexpr const char* GENERIC_FILAMENT_VENDOR = "Generic";

// True when the slot reports a filament. Stock U1 firmware encodes this as 0/1;
// accept a few spellings to be robust against firmware changes.
bool slot_loaded(const std::string &value)
{
    return value == "1" || value == "true" || value == "True";
}

// Parse the RRGGBB part of a colour into 0xRRGGBB, skipping a leading '#' and
// dropping any alpha channel. Returns false rather than letting std::stoul throw
// on an empty or malformed value.
bool parse_rgb(const std::string &value, unsigned int &out)
{
    const size_t hash_pos = value.find('#');
    std::string  digits   = value.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0);
    if (digits.size() > 6)
        digits.resize(6);
    if (digits.size() != 6)
        return false;
    for (char c : digits) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    out = static_cast<unsigned int>(std::stoul(digits, nullptr, 16));
    return true;
}

// Case-insensitive whole-word search. The needle must not be flanked by alphanumeric
// characters, so "CF" matches "PETG-CF" and "PLA CF" but not "Scaffold" or "CF10".
bool contains_word_ci(const std::string &haystack, const std::string &needle)
{
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto equal_ci = [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); };
    const auto is_word  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (auto it = haystack.begin(); it != haystack.end(); ++it) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end(), equal_ci);
        if (it == haystack.end())
            return false;

        const auto after    = it + static_cast<std::string::difference_type>(needle.size());
        const bool left_ok  = it == haystack.begin() || !is_word(*(it - 1));
        const bool right_ok = after == haystack.end() || !is_word(*after);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

// A tag reports the fibre loading in its sub type ("CF10", "GF30"), while the material
// types and the profile names only ever say CF or GF. Expects upper case input.
std::string normalize_fibre_sub_type(const std::string &sub)
{
    for (const char* fibre : {"CF", "GF"}) {
        const std::string prefix = fibre;
        if (sub.size() >= prefix.size() && sub.compare(0, prefix.size(), prefix) == 0 &&
            std::all_of(sub.begin() + static_cast<std::string::difference_type>(prefix.size()), sub.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            return prefix;
    }
    return sub;
}

// Like contains_word_ci, but a trailing digit run belongs to the same word, so a CF sub type
// matches a "PA12-CF10" preset as well as "PA12-CF". A trailing letter does not: "CFX" is
// a different product.
bool contains_sub_type_ci(const std::string &haystack, const std::string &needle)
{
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto equal_ci = [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); };
    const auto is_word  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (auto it = haystack.begin(); it != haystack.end(); ++it) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end(), equal_ci);
        if (it == haystack.end())
            return false;

        auto after = it + static_cast<std::string::difference_type>(needle.size());
        while (after != haystack.end() && std::isdigit(static_cast<unsigned char>(*after)) != 0)
            ++after;

        const bool left_ok  = it == haystack.begin() || !is_word(*(it - 1));
        const bool right_ok = after == haystack.end() || !is_word(*after);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

// Tags written by older tools (or as a workaround) carry the full product name in the vendor
// field, e.g. vendor "Anycubic PLA Basic" with an empty sub type, while profiles may use either
// "Anycubic" or "Anycubic PLA Basic". Treat the vendors as matching when they are equal or when
// either contains the other as whole words, ignoring case.
bool vendor_matches(const std::string &tag_vendor, const std::string &profile_vendor)
{
    if (tag_vendor.empty() || profile_vendor.empty())
        return false;
    return contains_word_ci(tag_vendor, profile_vendor) || contains_word_ci(profile_vendor, tag_vendor);
}

// U1 presets are named like "Generic PLA @SnapmakerU1"; vendor template sections are named
// like "*common*". Only the user-selectable presets make sense as slot matches.
bool is_selectable_preset_name(const std::string &name)
{
    return !name.empty() && name[0] != '*';
}

// Does the preset name carry the printer's model? The model id ("U1") may appear glued to
// the vendor name in the preset family suffix ("@SnapmakerU1"), where a whole-word search
// fails, so accept a case-insensitive suffix match as well.
bool name_matches_model(const std::string &name, const std::string &model_hint)
{
    if (model_hint.empty())
        return false;
    return contains_word_ci(name, model_hint) || boost::algorithm::iends_with(name, model_hint);
}

// Rank the filament presets of one filament type by colour distance, preferring a
// preset that carries the spool's sub type. Candidates are limited to vendor_filter,
// which nothing matches when it is empty; tag_vendor is the slot's own vendor field,
// which a legacy tag uses to name the variant. model_hint (printer model with spaces
// removed, e.g. "SnapmakerU1") ranks the printer's own preset family ahead of the
// vendor's legacy presets for other machines -- without a colour to break the tie,
// "Snapmaker PLA @Snapmaker" and "Snapmaker PLA @SnapmakerU1" are otherwise
// indistinguishable. Returns the matched preset name.
// Port of OrcaSlicer SnapmakerPrinterAgent::closest_preset_by_colour(), adapted to
// this fork's PresetCollection (match by preset name, no filament_id).
std::string closest_preset_by_colour(const PresetCollection &filaments, const std::string &vendor_filter,
                                     const std::string &tag_vendor, const std::string &filament_type,
                                     const std::string &sub_wanted, unsigned int target_color_value,
                                     const std::string &model_hint)
{
    std::string  best_match_name      = "";
    unsigned int best_color_distance  = 0xffffffffu;
    bool         best_matches_sub     = false;
    bool         best_preferred       = false;

    for (const Preset &p : filaments.get_presets()) {
        if (p.is_default || !is_selectable_preset_name(p.name) || !p.config.has("filament_type"))
            continue;
        if (p.config.opt_string("filament_type", 0) != filament_type)
            continue;
        const std::string p_vendor = p.config.has("filament_vendor") ? p.config.opt_string("filament_vendor") : std::string();
        if (!vendor_matches(vendor_filter, p_vendor))
            continue;
        // A profile that specifies no colour, or one that cannot be parsed, is unranked: it
        // is kept as a last resort rather than scored as black, which would otherwise let a
        // colourless profile win every dark spool and beat profiles that do declare a colour.
        unsigned int p_color_value = 0;
        unsigned int distance      = UNRANKED_COLOR_DISTANCE;
        if (p.config.has("filament_colour")) {
            const ConfigOptionStrings *color_opt = p.config.option<ConfigOptionStrings>("filament_colour");
            if (color_opt && color_opt->size() > 0)
                if (parse_rgb(color_opt->get_at(0), p_color_value))
                    // Euclidean colour distance in RGB space
                    distance = static_cast<unsigned int>(
                        (int((target_color_value & 0xff))          - int((p_color_value & 0xff)))          * (int((target_color_value & 0xff))          - int((p_color_value & 0xff))) +
                        (int(((target_color_value >> 8) & 0xff))   - int(((p_color_value >> 8) & 0xff)))   * (int(((target_color_value >> 8) & 0xff))   - int(((p_color_value >> 8) & 0xff))) +
                        (int(((target_color_value >> 16) & 0xff))  - int(((p_color_value >> 16) & 0xff)))  * (int(((target_color_value >> 16) & 0xff))  - int(((p_color_value >> 16) & 0xff))));
        }

        // Variants (Matte, Silk, Basic, Eco, ...) are not filament_type values -- the
        // profiles carry them in the preset name. Prefer a candidate whose name mentions
        // the spool's sub type, and rank by colour within that preference. When the tag
        // has no sub type, a legacy tag may still name the variant in its vendor field
        // ("Anycubic PLA Silk"), so look for the whole tag vendor in the preset name too.
        const bool matches_sub = !sub_wanted.empty() ?
                                     contains_sub_type_ci(p.name, sub_wanted) :
                                     (contains_word_ci(tag_vendor, p.name) || contains_word_ci(p.name, tag_vendor));
        const bool preferred = name_matches_model(p.name, model_hint);

        // Rank: printer-model family first, then sub-type match, then colour distance.
        bool better;
        if (best_match_name.empty())
            better = true;
        else if (preferred != best_preferred)
            better = preferred;
        else if (matches_sub != best_matches_sub)
            better = matches_sub;
        else
            better = distance < best_color_distance;
        if (!better)
            continue;

        best_color_distance = distance;
        best_match_name     = p.name;
        best_matches_sub    = matches_sub;
        best_preferred      = preferred;
    }
    return best_match_name;
}

// First selectable preset carrying the exact filament_type, whatever its vendor
// (equivalent of Orca's filament_id_by_type() fallback). model_hint ranks the
// printer's own preset family first.
std::string first_preset_by_type(const PresetCollection &filaments, const std::string &filament_type,
                                 const std::string &model_hint)
{
    const Preset *best = nullptr;
    for (const Preset &p : filaments.get_presets()) {
        if (p.is_default || !is_selectable_preset_name(p.name) || !p.config.has("filament_type"))
            continue;
        if (p.config.opt_string("filament_type", 0) != filament_type)
            continue;
        if (!best)
            best = &p;
        else if (name_matches_model(p.name, model_hint) && !name_matches_model(best->name, model_hint))
            best = &p;
    }
    return best ? best->name : std::string();
}

// Slot material type: tag type + sub type, with fibre loadings normalized
// ("PLA" + "CF10" -> "PLA-CF"). Port of Orca's combine_filament_type().
std::string combine_filament_type(const std::string &type, const std::string &sub_type)
{
    const std::string base = boost::trim_copy(boost::to_upper_copy(type));
    const std::string sub  = boost::trim_copy(boost::to_upper_copy(sub_type));

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    // Only CF and GF are material types, whatever fibre loading the tag reports ("CF10");
    // skip the suffix when the base already carries it.
    const std::string fibre = normalize_fibre_sub_type(sub);
    if (fibre == "CF" || fibre == "GF") {
        const std::string suffix     = "-" + fibre;
        const bool        has_suffix = base.size() >= suffix.size() &&
                                base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0;
        return has_suffix ? base : base + suffix;
    }

    // Every other sub type is a finish or a brand name, not a material type.
    return base;
}

// Match one slot to a filament preset: the slot's own vendor first (with the sub type
// as a preference), then our Generic presets, then any preset of the type. If the type
// carries a fibre suffix ("PLA-CF") and nothing matches, retry with the plain base type.
// model_hint (printer model, spaces removed) ranks the printer's own preset family first.
// Port of Orca's find_closest_color_preset_by_vendor_and_type() + fallback chain.
std::string match_slot_to_preset(PresetBundle *bundle, const FilamentSlot &slot, const std::string &model_hint)
{
    if (!bundle)
        return std::string();

    const PresetCollection &filaments = bundle->filaments;

    unsigned int target_color_value = 0;
    // An unusable colour cannot rank candidates; leave the match to the type-only lookups.
    const bool has_color = parse_rgb(slot.color, target_color_value);
    (void)has_color;

    const std::string sub_wanted = normalize_fibre_sub_type(boost::trim_copy(boost::to_upper_copy(slot.raw_sub_type)));

    // The spool's own vendor first, with the sub type as a preference. A slot that reports
    // no vendor matches no preset's filament_vendor, so such a slot finds nothing in this pass.
    std::string match = closest_preset_by_colour(filaments, slot.vendor, slot.vendor, slot.type, sub_wanted, target_color_value, model_hint);

    // No preset for the spool's vendor: fall back to the Generic presets.
    if (match.empty())
        match = closest_preset_by_colour(filaments, GENERIC_FILAMENT_VENDOR, slot.vendor, slot.type, sub_wanted, target_color_value, model_hint);

    // Type-only over all vendors (first selectable preset of the type).
    if (match.empty())
        match = first_preset_by_type(filaments, slot.type, model_hint);

    // Fibre-suffixed type with no dedicated preset ("PLA-CF"): retry with the plain base type.
    const auto dash = slot.type.rfind('-');
    if (match.empty() && dash != std::string::npos && dash + 1 < slot.type.size()) {
        const std::string base_type = slot.type.substr(0, dash);
        match = closest_preset_by_colour(filaments, slot.vendor, slot.vendor, base_type, sub_wanted, target_color_value, model_hint);
        if (match.empty())
            match = closest_preset_by_colour(filaments, GENERIC_FILAMENT_VENDOR, slot.vendor, base_type, sub_wanted, target_color_value, model_hint);
        if (match.empty())
            match = first_preset_by_type(filaments, base_type, model_hint);
    }

    return match;
}

// Safely read a string element of a parallel slot array.
std::string array_string(const pt::ptree &node, size_t idx)
{
    size_t i = 0;
    for (const auto &item : node) {
        if (i++ == idx)
            return item.second.data();
    }
    return std::string();
}

// Safely read an integer element of a parallel slot array (0 when absent/malformed).
int array_int(const pt::ptree &node, size_t idx)
{
    const std::string value = array_string(node, idx);
    try {
        return std::stoi(value);
    } catch (const std::exception &) {
        return 0;
    }
}

} // anonymous namespace

wxString SnapmakerU1::get_test_ok_msg() const
{
    return _(L("Connection to Snapmaker U1 works correctly."));
}

wxString SnapmakerU1::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s"
        , _L("Could not connect to Snapmaker U1")
        , msg);
}

bool SnapmakerU1::get_filament_slots(std::vector<FilamentSlot> &out, wxString &error, PresetBundle *preset_bundle,
                                     const std::string &printer_model) const
{
    const char* name = get_name();

    // GET /printer/objects/query?print_task_config&filament_detect
    // (port auto-probe already ran if test() was called before this; make_url()
    // then serves the winning host:port)
    const auto url = make_url("printer/objects/query?print_task_config&filament_detect");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Fetching filament slots at: %2%") % name % url;

    std::string body;
    bool res = true;
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string, std::string err, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error fetching filament slots: %2%, HTTP %3%") % name % err % status;
        error = format_error(std::string(), err, status);
        res = false;
    })
    .on_complete([&](std::string response, unsigned) {
        body = std::move(response);
    })
    .perform_sync();

    if (!res)
        return false;

    pt::ptree root;
    try {
        std::stringstream ss(body);
        pt::read_json(ss, root);
    } catch (const std::exception &) {
        error = _L("Could not parse server response");
        return false;
    }

    auto status_opt = root.get_child_optional("result.status");
    if (!status_opt) {
        error = _L("Could not parse server response");
        return false;
    }
    const pt::ptree &status = *status_opt;
    auto ptc_opt = status.get_child_optional("print_task_config");
    if (!ptc_opt) {
        error = _L("No print_task_config in response: is this a Snapmaker U1?");
        return false;
    }
    const pt::ptree &ptc = *ptc_opt;

    const auto exist    = ptc.get_child_optional("filament_exist");
    const auto types    = ptc.get_child_optional("filament_type");
    const auto subtypes = ptc.get_child_optional("filament_sub_type");
    const auto colors   = ptc.get_child_optional("filament_color_rgba");
    const auto vendors  = ptc.get_child_optional("filament_vendor");
    const auto skus     = ptc.get_child_optional("filament_sku");

    const size_t slot_count = exist ? exist->size() : 0;
    if (slot_count == 0) {
        error = _L("No filament slots reported by the printer");
        return false;
    }

    // NFC tag data (optional): bed / max hotend temperatures per slot.
    const auto nfc_info = status.get_child_optional("filament_detect.info");
    auto nfc_slot = [&](size_t idx) -> const pt::ptree * {
        if (!nfc_info)
            return nullptr;
        size_t i = 0;
        for (const auto &item : *nfc_info) {
            if (i++ == idx)
                return &item.second;
        }
        return nullptr;
    };

    out.clear();
    out.reserve(slot_count);

    PresetBundle *bundle = preset_bundle ? preset_bundle : GUI::wxGetApp().preset_bundle.get();
    // Preset families are suffixed "@<ModelWithoutSpaces>" ("Snapmaker U1" -> "@SnapmakerU1").
    const std::string model_hint = boost::erase_all_copy(printer_model, " ");

    for (size_t i = 0; i < slot_count; ++i) {
        FilamentSlot slot;
        slot.tool   = static_cast<int>(i);
        slot.loaded = slot_loaded(array_string(*exist, i));

        if (slot.loaded) {
            slot.raw_sub_type = subtypes ? array_string(*subtypes, i) : std::string();
            slot.type         = combine_filament_type(types ? array_string(*types, i) : std::string(), slot.raw_sub_type);
            slot.color        = colors ? array_string(*colors, i) : std::string();
            slot.vendor       = vendors ? array_string(*vendors, i) : std::string();
            slot.sku          = skus ? array_string(*skus, i) : std::string();

            if (const pt::ptree *nfc = nfc_slot(i)) {
                slot.bed_temp        = nfc->get<int>("BED_TEMP", 0);
                slot.nozzle_temp_max = nfc->get<int>("HOTEND_MAX_TEMP", 0);
            }

            // Map the slot to one of our filament presets (vendor + type + sub-type +
            // colour distance, then Generic / type-only fallbacks).
            slot.matched_preset = match_slot_to_preset(bundle, slot, model_hint);
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Slot %2%: type %3%, vendor %4%, color %5% -> preset %6%")
                % name % i % slot.type % slot.vendor % slot.color % (slot.matched_preset.empty() ? "<none>" : slot.matched_preset);
        }

        out.emplace_back(std::move(slot));
    }

    return true;
}

// ---------------------------------------------------------------------------
// v3: websocket print start with options + filament->toolhead map_table
// ---------------------------------------------------------------------------

bool SnapmakerU1::firmware_supports_print_options(const std::string &firmware_version)
{
    // The U1 reports its firmware version as moonraker_version (read from /etc/VERSION).
    static const std::regex version_regex(R"(^\s*[vV]?(\d+)\.(\d+)(?:\.(\d+))?)");
    std::smatch match;
    if (!std::regex_search(firmware_version, match, version_regex))
        return false;
    try {
        const int major = std::stoi(match[1].str());
        const int minor = std::stoi(match[2].str());
        const int patch = match[3].matched ? std::stoi(match[3].str()) : 0;
        return std::make_tuple(major, minor, patch) >= std::make_tuple(1, 3, 0);
    } catch (const std::exception &) {
        return false;
    }
}

void SnapmakerU1::on_server_info(const std::string &moonraker_version) const
{
    m_fw_version                = moonraker_version;
    m_fw_print_options_supported = firmware_supports_print_options(moonraker_version);
    BOOST_LOG_TRIVIAL(info) << get_name() << ": firmware " << (moonraker_version.empty() ? "<unknown>" : moonraker_version)
                            << (m_fw_print_options_supported ? " applies print options"
                                                             : " predates print options, prints start without them");
}

bool SnapmakerU1::ws_jsonrpc_request(const std::string &method, const std::string &params_json,
                                     long timeout_seconds, std::string &result_json, std::string &error) const
{
#ifndef SNAPMAKER_HAS_CURL_WS
    (void)method; (void)params_json; (void)timeout_seconds; (void)result_json;
    error = "libcurl without websocket support";
    return false;
#else
    static std::atomic<int> next_jsonrpc_id{1};
    const int id = next_jsonrpc_id.fetch_add(1);

    // ws(s)://<effective host>/websocket (Moonraker's JSON-RPC endpoint)
    std::string ws_url;
    {
        const std::string &host = effective_host();
        std::string scheme = "ws://";
        std::string rest   = host;
        if (boost::algorithm::istarts_with(host, "https://")) {
            scheme = "wss://";
            rest   = host.substr(8);
        } else if (boost::algorithm::istarts_with(host, "http://")) {
            rest = host.substr(7);
        }
        while (!rest.empty() && rest.back() == '/')
            rest.pop_back();
        ws_url = scheme + rest + "/websocket";
    }

    nlohmann::json request = {{"jsonrpc", "2.0"}, {"method", method}, {"params", nlohmann::json::parse(params_json)}, {"id", id}};
    // params_json carries only the params object; the full JSON-RPC envelope is built here.
    const std::string request_text = request.dump();

    CURL *curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }

    // One deadline covers connect, handshake, request and the wait for the answer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool ok = false;

    curl_easy_setopt(curl, CURLOPT_URL, ws_url.c_str());
    // 2 = websocket ("connect only" + upgrade)
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    if (!m_apikey.empty()) {
        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, ("X-Api-Key: " + m_apikey).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (!m_cafile.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, m_cafile.c_str());
    } else {
        // LAN printers typically carry self-signed certificates; same policy as
        // the print-agent implementations this port comes from.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        error = std::string("websocket connect: ") + curl_easy_strerror(rc);
    } else {
        // Bound blocking recv() so the loop below can enforce the deadline itself.
        curl_socket_t sockfd = CURL_SOCKET_BAD;
        curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
        if (sockfd != CURL_SOCKET_BAD) {
            struct timeval tv;
            tv.tv_sec  = 2;
            tv.tv_usec = 0;
#ifdef _WIN32
            setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
            setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        }

        size_t sent = 0;
        rc = curl_ws_send(curl, request_text.data(), request_text.size(), &sent, 0, CURLWS_TEXT);
        if (rc != CURLE_OK || sent != request_text.size()) {
            error = std::string("websocket send: ") + curl_easy_strerror(rc);
        } else {
            // Moonraker pushes notifications to every connected client, so messages are
            // skipped until the one answering this request id.
            std::string acc;
            bool answered = false;
            while (!answered) {
                if (std::chrono::steady_clock::now() > deadline) {
                    error = "websocket answer timed out (the print may already be starting)";
                    break;
                }
                char buf[16384];
                size_t rlen = 0;
                const struct curl_ws_frame *meta = nullptr;
                rc = curl_ws_recv(curl, buf, sizeof(buf), &rlen, &meta);
                if (rc == CURLE_AGAIN || rc == CURLE_OPERATION_TIMEDOUT)
                    continue; // SO_RCVTIMEO tick; re-check the deadline
                if (rc != CURLE_OK) {
                    error = std::string("websocket recv: ") + curl_easy_strerror(rc);
                    break;
                }
                if (meta && (meta->flags & CURLWS_CLOSE)) {
                    error = "websocket closed by the printer before the answer";
                    break;
                }
                acc.append(buf, rlen);
                // bytesleft > 0: the message continues in further frames.
                if (meta && meta->bytesleft != 0)
                    continue;

                const nlohmann::json msg = nlohmann::json::parse(acc, nullptr, false);
                acc.clear();
                if (!msg.is_object() || !msg.contains("id") || !msg["id"].is_number_integer() ||
                    msg["id"].get<int>() != id)
                    continue; // notification or another request's answer

                answered = true;
                if (msg.contains("error")) {
                    const auto &err = msg["error"];
                    error = err.is_object() && err.contains("message") && err["message"].is_string()
                                ? err["message"].get<std::string>()
                                : err.dump();
                } else {
                    nlohmann::json result = msg.contains("result") ? msg["result"] : nlohmann::json();
                    result_json = result.dump();
                    ok = true;
                }
            }
        }
    }

    // Best-effort close; a failure here changes nothing about the answer.
    size_t sent = 0;
    curl_ws_send(curl, nullptr, 0, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(curl);
    return ok;
#endif
}

PrintHostPrintStartOptions SnapmakerU1::build_print_start_options(PresetBundle *bundle, wxString &error) const
{
    PrintHostPrintStartOptions options;

    std::vector<FilamentSlot> slots;
    wxString slot_error;
    if (!bundle) {
        error = _L("No presets loaded");
        return options;
    }
    if (!get_filament_slots(slots, slot_error, bundle, std::string())) {
        error = _L("Could not fetch filament slots for the toolhead mapping") + ": " + slot_error;
        return options;
    }

    // Same model hint as the slot chips in the Physical Printer dialog, so the
    // matching here reproduces exactly what the user saw.
    std::string printer_model;
    if (bundle) {
        const Preset &printer_preset = bundle->printers.get_edited_preset();
        if (printer_preset.config.has("printer_model"))
            printer_model = printer_preset.config.opt_string("printer_model");
    }

    // For every extruder, the toolhead whose slot matched the same filament preset.
    // Unmatched extruders are left out (firmware default: extruder i -> toolhead i).
    for (size_t i = 0; i < bundle->extruders_filaments.size(); ++i) {
        const std::string preset_name = bundle->extruders_filaments[i].get_selected_preset_name();
        if (preset_name.empty())
            continue;
        const FilamentSlot *match = nullptr;
        for (const FilamentSlot &slot : slots) {
            if (!slot.loaded || slot.matched_preset != preset_name)
                continue;
            if (!match || slot.tool == static_cast<int>(i))
                match = &slot; // prefer the identity mapping when several slots match
        }
        if (match) {
            if (match->tool != static_cast<int>(i))
                BOOST_LOG_TRIVIAL(info) << get_name() << ": extruder " << i << " (" << preset_name
                                        << ") -> toolhead " << match->tool;
            options.map_table.emplace_back(static_cast<int>(i), match->tool);
        }
    }

    // One slot feeding two extruders is not printable on a toolchanger; the firmware
    // would silently print both filaments from the same toolhead.
    for (size_t a = 0; a < options.map_table.size(); ++a)
        for (size_t b = a + 1; b < options.map_table.size(); ++b)
            if (options.map_table[a].second == options.map_table[b].second)
                BOOST_LOG_TRIVIAL(warning) << get_name() << ": extruders " << options.map_table[a].first << " and "
                                           << options.map_table[b].first << " both map to toolhead "
                                           << options.map_table[b].second;
    return options;
}

PresetBundle *SnapmakerU1::get_print_preset_bundle() const
{
    return GUI::wxGetApp().preset_bundle.get();
}

bool SnapmakerU1::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return Moonraker::upload(std::move(upload_data), prorgess_fn, error_fn, info_fn);

    // Connection test first: port auto-probe + firmware version capture
    // (Moonraker::upload runs it again for its own upload; harmless).
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

#if defined(SNAPMAKER_HAS_CURL_WS)
    const bool ws_available = true;
#else
    const bool ws_available = false;
#endif

    if (!m_fw_print_options_supported || !ws_available) {
        const wxString reason = !m_fw_print_options_supported
            ? _L("Printer firmware predates print start options (1.3.0+ required); starting the print without them.")
            : _L("This build cannot reach the printer's websocket; starting the print without the options.");
        BOOST_LOG_TRIVIAL(info) << name << ": " << GUI::into_u8(reason);
        info_fn(_L("Print start options"), reason);
        return Moonraker::upload(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    }

    // Filament -> toolhead mapping from the current per-extruder filament assignment.
    wxString options_error;
    PrintHostPrintStartOptions start_options = build_print_start_options(get_print_preset_bundle(), options_error);
    if (!options_error.empty()) {
        error_fn(options_error);
        return false;
    }
    // The three switches come from the send dialog.
    start_options.bed_level      = upload_data.print_start_options.bed_level;
    start_options.flow_calibrate = upload_data.print_start_options.flow_calibrate;
    start_options.time_lapse     = upload_data.print_start_options.time_lapse;

    // Upload without the print trigger, then start the print with the options
    // over the websocket (server.files.start_local_print is websocket-only).
    PrintHostUpload upload_only = upload_data;
    upload_only.post_action = PrintHostPostUploadAction::None;
    if (!Moonraker::upload(std::move(upload_only), prorgess_fn, error_fn, info_fn))
        return false;

    nlohmann::json options = {
        {"bed_level",        start_options.bed_level      ? 1 : 0},
        {"flow_calibrate",   start_options.flow_calibrate ? 1 : 0},
        {"time_lapse_camera", start_options.time_lapse    ? 1 : 0},
    };
    // The firmware turns each option into a parameter of SDCARD_PRINT_FILE_WITH_PARAMETERS
    // with Python's str(), so a JSON true would arrive as "True" and be rejected by its
    // get_int(); the switches are sent as 0/1 ints. Without a map_table the firmware
    // prints each filament with the toolhead of the same number.
    if (!start_options.map_table.empty()) {
        nlohmann::json map_table = nlohmann::json::array();
        for (const auto &pair : start_options.map_table)
            map_table.push_back({pair.first, pair.second});
        options["map_table"] = std::move(map_table);
    }
    // path is relative to the gcodes root, exactly what the upload posted.
    nlohmann::json params = {
        {"path",    upload_data.upload_path.generic_string()},
        {"options", std::move(options)},
    };
    const std::string request_body = params.dump();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: server.files.start_local_print params: %2%") % name % request_body;

    info_fn(_L("Print start options"), _L("Starting print..."));
    std::string result_json, ws_error;
    // Moonraker answers only after the firmware ran its pre-print checks, hence 60 s.
    if (!ws_jsonrpc_request("server.files.start_local_print", request_body, 60, result_json, ws_error)) {
        // Not retried with the plain print trigger: after a timeout the print may
        // already be starting (same trade-off as the reference implementation).
        BOOST_LOG_TRIVIAL(error) << name << ": start_local_print failed: " << ws_error;
        error_fn(_L("Print start failed") + ": " + GUI::from_u8(ws_error));
        return false;
    }

    // The firmware reports its own failures (e.g. nozzle mismatch) in the result,
    // not as a JSON-RPC error.
    const nlohmann::json result = nlohmann::json::parse(result_json, nullptr, false);
    const std::string state = result.is_object() && result.contains("state") && result["state"].is_string()
                                  ? result["state"].get<std::string>() : std::string();
    if (state != "success") {
        const std::string message = result.is_object() && result.contains("message") && result["message"].is_string()
                                        ? result["message"].get<std::string>() : result_json;
        BOOST_LOG_TRIVIAL(error) << name << ": the printer refused the print: " << message;
        error_fn(_L("The printer refused the print") + ": " + GUI::from_u8(message));
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << name << ": print started with options";
    return true;
}

} // namespace Slic3r
