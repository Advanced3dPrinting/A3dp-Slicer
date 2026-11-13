///|/ Copyright (c) 2026 Advanced3dPrinting LLC
///|/
///|/ A3dp-Slicer is released under the terms of the AGPLv3 or higher
///|/
#include "Snapmaker.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

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

} // namespace Slic3r
