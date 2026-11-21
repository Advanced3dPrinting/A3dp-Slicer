#include "SessionBackup.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Time.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/process.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/utils.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

bool SessionBackup::s_prompt_pending = false;

namespace {

constexpr const char *FILE_PRESETS_INI = "session_presets.ini";
constexpr const char *FILE_PROJECT_3MF = "session_project.3mf";
constexpr const char *FILE_STATE_JSON  = "session_state.json";
constexpr const char *MARKER_SUFFIX    = ".live";
constexpr const char *LEGACY_PREFIX    = "legacy-";

constexpr int DEFAULT_MAX_AGE_DAYS = 30;

std::atomic<bool> s_backup_running{false};
std::string s_session_id;         // "<pid>-<start epoch>"; empty until startup_mark_running()
std::string s_session_start_iso;  // iso utc of this instance's startup
std::string s_start_token;        // process start-time token for liveness checks
long        s_pid                 = 0;

class BackupTimer : public wxTimer
{
public:
    void Notify() override { SessionBackup::backup_now(); }
};

BackupTimer &backup_timer()
{
    static BackupTimer timer;
    return timer;
}

fs::path session_root()
{
    return fs::path(Slic3r::data_dir()) / "session";
}

fs::path session_dir_of(const std::string &session_id)
{
    return session_root() / session_id;
}

fs::path marker_path(const std::string &session_id)
{
    return session_root() / (session_id + MARKER_SUFFIX);
}

// Write into a temporary file next to the target and rename it into place,
// so a crash in the middle of writing never leaves a truncated backup.
template <typename WriteFn>
bool atomic_write(const fs::path &target, WriteFn write_fn)
{
    fs::path tmp = target;
    tmp += ".tmp";
    boost::system::error_code ec;
    if (! write_fn(tmp)) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // On some platforms rename() may fail if the target exists.
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

std::string iso_from_time_t(std::time_t t)
{
    std::tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

time_t parse_iso_utc(const std::string &iso)
{
    // Slic3r::Utils::iso_utc_timestamp() writes the compact form
    // "YYYYMMDDTHHMMSSZ"; accept the dashed "YYYY-MM-DDTHH:MM:SSZ" too.
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &s) != 6 &&
        sscanf(iso.c_str(), "%4d%2d%2dT%2d%2d%2d", &Y, &M, &D, &h, &m, &s) != 6)
        return 0;
    std::tm tmv = {};
    tmv.tm_year = Y - 1900;
    tmv.tm_mon  = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = s;
#ifdef _WIN32
    return _mkgmtime(&tmv); // MSVC has no timegm()
#else
    return timegm(&tmv);
#endif
}

// Read /proc/<pid>/stat. Returns false when the process does not exist (or
// /proc is unavailable). On success fills state (single char, e.g. "R", "Z")
// and start_token (field 22, starttime in clock ticks - a stable per-process
// identity that survives pid reuse).
bool linux_proc_info(long pid, std::string &state, std::string &start_token)
{
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (! f.is_open())
        return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // comm (field 2) is parenthesized and may contain spaces - parse after the last ')'.
    const size_t close = content.rfind(')');
    if (close == std::string::npos)
        return false;
    std::istringstream ss(content.substr(close + 1));
    std::string tok;
    if (! (ss >> tok)) // field 3: state
        return false;
    state = tok;
    for (int i = 0; i < 19; ++ i) // skip fields 4..22; starttime is the 20th after state
        if (! (ss >> tok))
            return false;
    start_token = tok;
    return true;
}

// A stable identity of THIS process (empty on platforms without support).
// Linux: /proc/self/stat starttime (clock ticks). Windows: process creation
// FILETIME. Used to detect pid reuse when checking a session marker.
std::string process_start_token()
{
#if defined(__linux__)
    std::string state, token;
    if (linux_proc_info((long)wxGetProcessId(), state, token))
        return token;
    return std::string();
#elif defined(_WIN32)
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    if (! GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
        return std::string();
    const unsigned long long ft =
        (unsigned long long(creation.dwHighDateTime) << 32) | unsigned long long(creation.dwLowDateTime);
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", ft);
    return buf;
#else
    return std::string();
#endif
}

// Is the process that owns a session marker still alive? The start token
// guards against pid reuse: a recycled pid with a different start time is
// reported as dead.
bool process_alive(long pid, const std::string &token)
{
    if (pid <= 0)
        return false;
#if defined(__linux__)
    std::string state, proc_token;
    if (linux_proc_info(pid, state, proc_token))
        // A zombie is already dead (unreaped); do not treat it as running.
        return state != "Z" && (token.empty() || proc_token == token);
    return false; // no /proc entry -> dead
#elif defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (h == nullptr)
        return false;
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(h, &creation, &exit_time, &kernel, &user);
    CloseHandle(h);
    if (! ok)
        return true; // could not read the start time - be conservative
    if (token.empty())
        return true;
    const unsigned long long ft =
        (unsigned long long(creation.dwHighDateTime) << 32) | unsigned long long(creation.dwLowDateTime);
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", ft);
    return token == buf;
#else
    return wxProcess::Exists(int(pid));
#endif
}

bool read_marker(const fs::path &path, long &pid, std::string &token)
{
    boost::nowide::ifstream f(path.string());
    if (! f.is_open())
        return false;
    std::string line;
    bool ok = false;
    while (std::getline(f, line)) {
        if (boost::starts_with(line, "pid=")) {
            pid = atol(line.c_str() + 4);
            ok = pid > 0;
        } else if (boost::starts_with(line, "token="))
            token = line.substr(6);
    }
    return ok;
}

bool write_marker(const fs::path &path)
{
    return atomic_write(path, [](const fs::path &tmp) {
        boost::nowide::ofstream f(tmp.string(), std::ios::out | std::ios::trunc);
        if (! f.is_open())
            return false;
        f << "pid=" << s_pid << std::endl;
        f << "token=" << s_start_token << std::endl;
        return true;
    });
}

// Serialize the edited (possibly unsaved) presets of all collections plus the
// names of the active presets, in the standard config bundle format. The
// format is deliberately identical to PresetBundle::export_configbundle() so
// that restore_session() can reuse PresetBundle::load_configbundle().
bool write_presets_ini(const fs::path &path)
{
    PresetBundle *bundle = wxGetApp().preset_bundle.get();
    if (bundle == nullptr)
        return false;
    boost::nowide::ofstream c;
    c.open(path.string(), std::ios::out | std::ios::trunc);
    if (! c.is_open())
        return false;
    c << "# " << Slic3r::header_slic3r_generated() << std::endl;
    c << "# Session backup of the edited presets, incl. unsaved changes." << std::endl;
    c << "# Managed automatically for crash recovery, do not edit." << std::endl;
    for (const PresetCollection *presets : {
             (const PresetCollection *)&bundle->fff_prints, (const PresetCollection *)&bundle->filaments,
             (const PresetCollection *)&bundle->sla_prints, (const PresetCollection *)&bundle->sla_materials,
             (const PresetCollection *)&bundle->printers }) {
        const Preset &edited = presets->get_edited_preset();
        if (edited.name.empty())
            continue;
        c << std::endl << "[" << presets->section_name() << ":" << edited.name << "]" << std::endl;
        for (const std::string &opt_key : edited.config.keys())
            c << opt_key << " = " << edited.config.opt_serialize(opt_key) << std::endl;
    }
    c << std::endl << "[presets]" << std::endl;
    c << "print = "        << bundle->fff_prints.get_selected_preset_name()    << std::endl;
    c << "sla_print = "    << bundle->sla_prints.get_selected_preset_name()    << std::endl;
    c << "sla_material = " << bundle->sla_materials.get_selected_preset_name() << std::endl;
    c << "printer = "      << bundle->printers.get_selected_preset_name()      << std::endl;
    for (size_t i = 0; i < bundle->extruders_filaments.size(); ++ i) {
        char suffix[64];
        if (i > 0)
            sprintf(suffix, "_%d", int(i));
        else
            suffix[0] = 0;
        c << "filament" << suffix << " = " << bundle->extruders_filaments[i].get_selected_preset_name() << std::endl;
    }
    c.close();
    return true;
}

std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)ch < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)ch);
                out += buf;
            } else
                out += ch;
        }
    }
    return out;
}

bool write_state_json(const fs::path &path)
{
    Plater *plater = wxGetApp().plater();
    const std::string project_file = (plater != nullptr) ? into_u8(plater->get_project_filename()) : std::string();
    const std::string technology   = (wxGetApp().get_current_printer_technology() == ptSLA) ? "SLA" : "FFF";
    boost::nowide::ofstream c;
    c.open(path.string(), std::ios::out | std::ios::trunc);
    if (! c.is_open())
        return false;
    c << "{" << std::endl;
    c << "  \"app_version\": \""        << json_escape(SLIC3R_VERSION)         << "\"," << std::endl;
    c << "  \"pid\": "                  << s_pid                               << ","   << std::endl;
    c << "  \"session_start\": \""      << json_escape(s_session_start_iso)    << "\"," << std::endl;
    c << "  \"printer_technology\": \"" << json_escape(technology)             << "\"," << std::endl;
    c << "  \"project_file\": \""       << json_escape(project_file)           << "\"," << std::endl;
    c << "  \"timestamp\": \""          << json_escape(Slic3r::Utils::iso_utc_timestamp()) << "\"" << std::endl;
    c << "}" << std::endl;
    c.close();
    return true;
}

// Extract a quoted string value of "key" from the state json. The file is
// written by write_state_json() only, so a strict JSON parser is not needed.
std::string read_state_field(const fs::path &json_path, const char *key)
{
    boost::nowide::ifstream c(json_path.string());
    if (! c.is_open())
        return std::string();
    const std::string needle = std::string("\"") + key + "\"";
    std::string line;
    while (std::getline(c, line)) {
        const size_t key_pos = line.find(needle);
        if (key_pos == std::string::npos)
            continue;
        const size_t colon = line.find(':', key_pos);
        if (colon == std::string::npos)
            continue;
        const size_t value_start = line.find('"', colon);
        if (value_start == std::string::npos)
            continue;
        std::string escaped = line.substr(value_start + 1, line.rfind('"') - value_start - 1);
        // Invert json_escape().
        std::string out;
        for (size_t i = 0; i < escaped.size(); ++ i) {
            if (escaped[i] == '\\' && i + 1 < escaped.size()) {
                switch (escaped[++ i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += escaped[i]; break;
                }
            } else
                out += escaped[i];
        }
        return out;
    }
    return std::string();
}

// Preset sections exported from a system preset are skipped by
// PresetBundle::load_configbundle() ("is a system preset ... will be ignored"),
// yet they carry the user's unsaved edits. Apply those values directly onto the
// stored preset so that the subsequent selection into the edited preset
// (GUI_App::load_current_presets) picks them up.
void apply_system_derived_edits(const fs::path &ini_path, PresetBundle &bundle)
{
    namespace pt = boost::property_tree;
    try {
        boost::nowide::ifstream ifs(ini_path.string());
        pt::ptree tree;
        pt::read_ini(ifs, tree);
        for (const pt::ptree::value_type &section : tree) {
            const std::string &section_name = section.first;
            PresetCollection  *presets     = nullptr;
            std::string        preset_name;
            if (boost::starts_with(section_name, "print:")) {
                presets = &bundle.fff_prints;
                preset_name = section_name.substr(6);
            } else if (boost::starts_with(section_name, "filament:")) {
                presets = &bundle.filaments;
                preset_name = section_name.substr(9);
            } else if (boost::starts_with(section_name, "sla_print:")) {
                presets = &bundle.sla_prints;
                preset_name = section_name.substr(10);
            } else if (boost::starts_with(section_name, "sla_material:")) {
                presets = &bundle.sla_materials;
                preset_name = section_name.substr(13);
            } else if (boost::starts_with(section_name, "printer:")) {
                presets = &bundle.printers;
                preset_name = section_name.substr(8);
            }
            if (presets == nullptr || preset_name.empty())
                continue;
            // respect_active_preset = false: get the STORED preset, not the
            // edited copy, so the values survive the selection into the edited
            // preset performed by GUI_App::load_current_presets() below.
            Preset *existing = presets->find_preset(preset_name, false, false);
            if (existing == nullptr || ! existing->is_system)
                // User / external presets were already overwritten in memory by
                // load_configbundle() above.
                continue;
            ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
            for (const pt::ptree::value_type &kvp : section.second) {
                try {
                    existing->config.set_deserialize(kvp.first, kvp.second.data(), substitutions);
                } catch (const ConfigurationError &ex) {
                    BOOST_LOG_TRIVIAL(error) << "Session restore: skipping key \"" << kvp.first
                                             << "\" of preset \"" << preset_name << "\": " << ex.what();
                }
            }
        }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Session restore: failed to re-apply edited preset values: " << ex.what();
    }
}

// Pre-multi-instance layout: the backup files sat directly under session/.
// Move them into a legacy-<epoch> subdir (no marker => offered as a crashed
// session) so the standard per-session scan finds them.
void migrate_legacy_layout()
{
    const fs::path root = session_root();
    boost::system::error_code ec;
    bool found = false;
    for (const char *name : {FILE_STATE_JSON, FILE_PRESETS_INI, FILE_PROJECT_3MF})
        if (fs::exists(root / name, ec)) {
            found = true;
            break;
        }
    if (! found)
        return;
    const std::string base = std::string(LEGACY_PREFIX) + std::to_string(std::time(nullptr));
    fs::path dst = root / base;
    for (int n = 0; fs::exists(dst, ec) && n < 100; ++ n)
        dst = root / (base + "-" + std::to_string(n));
    fs::create_directories(dst, ec);
    for (const char *name : {FILE_STATE_JSON, FILE_PRESETS_INI, FILE_PROJECT_3MF}) {
        const fs::path src = root / name;
        if (fs::exists(src, ec))
            fs::rename(src, dst / name, ec);
        fs::remove(root / (std::string(name) + ".tmp"), ec);
    }
    BOOST_LOG_TRIVIAL(info) << "Session backup: migrated legacy layout to " << dst.string();
}

// Remove session dirs older than the configured max age (never the ones whose
// owner process is still alive).
void gc_stale_sessions()
{
    const AppConfig *cfg = wxGetApp().app_config.get();
    int max_age_days = DEFAULT_MAX_AGE_DAYS;
    if (cfg != nullptr && cfg->has("session_backup_max_age_days")) {
        max_age_days = atoi(cfg->get("session_backup_max_age_days").c_str());
        if (max_age_days <= 0)
            return; // GC disabled
    }
    const std::time_t cutoff = std::time(nullptr) - max_age_days * 24 * 3600;
    const fs::path root = session_root();
    boost::system::error_code ec;
    if (! fs::is_directory(root, ec))
        return;
    for (fs::directory_iterator it(root, ec), end; it != end && ! ec; it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (fs::last_write_time(p, ec) > cutoff || ec)
            continue;
        if (fs::is_directory(p, ec)) {
            long pid = 0;
            std::string token;
            const fs::path marker = root / (name + MARKER_SUFFIX);
            if (fs::exists(marker, ec) && read_marker(marker, pid, token) && process_alive(pid, token))
                continue; // still owned by a live instance
            fs::remove_all(p, ec);
            fs::remove(marker, ec);
            BOOST_LOG_TRIVIAL(info) << "Session backup: purged stale session " << name;
        } else if (boost::ends_with(name, MARKER_SUFFIX)) {
            long pid = 0;
            std::string token;
            if (read_marker(p, pid, token) && process_alive(pid, token))
                continue; // owned by a live instance (marker mtimes do not refresh)
            fs::remove(p, ec);
        }
    }
}

} // anonymous namespace

bool SessionBackup::enabled()
{
    const AppConfig *cfg = wxGetApp().app_config.get();
    // Missing key = enabled (default on).
    return cfg != nullptr && (! cfg->has("session_backup_enabled") || cfg->get_bool("session_backup_enabled"));
}

void SessionBackup::startup_mark_running()
{
    if (! enabled())
        return;
    s_pid             = long(wxGetProcessId());
    const std::time_t now = std::time(nullptr);
    s_session_id      = std::to_string(s_pid) + "-" + std::to_string(now);
    s_session_start_iso = iso_from_time_t(now);
    s_start_token     = process_start_token();

    boost::system::error_code ec;
    fs::create_directories(session_root(), ec);
    // The marker must exist before the session dir does: a session dir
    // without a marker is treated as crashed by other instances.
    if (! write_marker(marker_path(s_session_id))) {
        BOOST_LOG_TRIVIAL(error) << "Session backup: failed to write the liveness marker, feature disabled for this run";
        s_session_id.clear();
        return;
    }
    fs::create_directories(session_dir_of(s_session_id), ec);
    migrate_legacy_layout();
    gc_stale_sessions();
    BOOST_LOG_TRIVIAL(debug) << "Session backup: session " << s_session_id << " registered";
}

void SessionBackup::clean_exit()
{
    if (s_session_id.empty())
        return;
    boost::system::error_code ec;
    fs::remove_all(session_dir_of(s_session_id), ec);
    fs::remove(marker_path(s_session_id), ec);
    BOOST_LOG_TRIVIAL(debug) << "Session backup: session " << s_session_id << " removed (clean exit)";
    s_session_id.clear();
}

void SessionBackup::schedule_backup(int delay_ms)
{
    if (! enabled() || wxGetApp().is_gcode_viewer() || s_prompt_pending)
        return;
    // wxTimer::Start() restarts a running timer, so repeated calls coalesce
    // into a single backup delay_ms after the last change.
    backup_timer().Start(delay_ms, true /* one shot */);
}

void SessionBackup::backup_now()
{
    backup_timer().Stop();
    if (! enabled() || wxGetApp().is_gcode_viewer() || s_prompt_pending || s_session_id.empty())
        // While the crash-recovery prompt is unanswered, never write: the new
        // run's startup changes would overwrite the crashed session's backup.
        return;
    // Skip if another backup is already running (the export runs on the main
    // thread, so this only guards against re-entry).
    if (s_backup_running.exchange(true))
        return;
    try {
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr) {
            s_backup_running = false;
            return;
        }
        const fs::path dir = session_dir_of(s_session_id);
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        // 1) Presets, incl. unsaved edits (also covers the no-project case).
        atomic_write(dir / FILE_PRESETS_INI, write_presets_ini);
        // 2) The loaded project incl. embedded config. The export shares the
        //    code path of a manual project save (store_3mf) and runs on the
        //    main thread; for very large projects this may stall the UI for
        //    the duration of the export (accepted trade-off, see class comment).
        const fs::path project_path = dir / FILE_PROJECT_3MF;
        Model &model = plater->model();
        if (! model.objects.empty()) {
            DynamicPrintConfig config = wxGetApp().preset_bundle->full_config_secure();
            atomic_write(project_path, [&model, &config](const fs::path &tmp) {
                return Slic3r::store_3mf(tmp.string().c_str(), &model, &config,
                    Slic3r::OptionStore3mf{}
                        .set_fullpath_sources(false)
                        .set_thumbnail_data(nullptr)
                        .set_bake_transformation_in_mesh(model.baked_transformation));
            });
        } else {
            // Plater went empty since the last backup: drop the stale project
            // so a restore does not resurrect removed objects.
            fs::remove(project_path, ec);
        }
        // 3) Session metadata.
        atomic_write(dir / FILE_STATE_JSON, write_state_json);
        BOOST_LOG_TRIVIAL(debug) << "Session backup written to " << dir.string();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Session backup failed: " << ex.what();
    }
    s_backup_running = false;
}

std::vector<SessionBackup::CrashedSession> SessionBackup::crashed_sessions()
{
    std::vector<CrashedSession> out;
    if (! enabled() || s_session_id.empty())
        return out;
    const fs::path root = session_root();
    boost::system::error_code ec;
    if (! fs::is_directory(root, ec))
        return out;
    for (fs::directory_iterator it(root, ec), end; it != end && ! ec; it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (! fs::is_directory(p, ec)) {
            if (boost::ends_with(name, MARKER_SUFFIX)) {
                // Orphan marker - only when its session dir is already gone.
                // A marker of a LIVE instance must never be removed here.
                const std::string suffix(MARKER_SUFFIX);
                const std::string sid = name.substr(0, name.size() - suffix.size());
                if (! fs::exists(session_root() / sid, ec))
                    fs::remove(p, ec);
            }
            continue;
        }
        if (name == s_session_id)
            continue; // this instance
        long pid = 0;
        std::string token;
        const fs::path marker = root / (name + MARKER_SUFFIX);
        if (fs::exists(marker, ec) && read_marker(marker, pid, token) && process_alive(pid, token))
            continue; // owned by a live instance
        const bool has_content = fs::exists(p / FILE_STATE_JSON, ec) || fs::exists(p / FILE_PRESETS_INI, ec) ||
                                 fs::exists(p / FILE_PROJECT_3MF, ec);
        if (! has_content) {
            // Crashed before the first backup completed - nothing to restore.
            fs::remove_all(p, ec);
            fs::remove(marker, ec);
            continue;
        }
        CrashedSession cs;
        cs.session_id  = name;
        cs.app_version = read_state_field(p / FILE_STATE_JSON, "app_version");
        cs.timestamp   = read_state_field(p / FILE_STATE_JSON, "timestamp");
        cs.project_file = read_state_field(p / FILE_STATE_JSON, "project_file");
        if (cs.timestamp.empty())
            cs.timestamp = iso_from_time_t(fs::last_write_time(p, ec));
        out.push_back(cs);
    }
    // Newest first (iso utc sorts lexicographically).
    std::sort(out.begin(), out.end(), [](const CrashedSession &a, const CrashedSession &b) {
        return a.timestamp > b.timestamp;
    });
    return out;
}

void SessionBackup::discard_session(const std::string &session_id)
{
    boost::system::error_code ec;
    fs::remove_all(session_dir_of(session_id), ec);
    fs::remove(marker_path(session_id), ec);
}

bool SessionBackup::restore_session(const std::string &session_id)
{
    const fs::path dir = session_dir_of(session_id);
    try {
        PresetBundle *bundle = wxGetApp().preset_bundle.get();
        if (bundle == nullptr)
            return false;
        const fs::path presets_path = dir / FILE_PRESETS_INI;
        const fs::path project_path = dir / FILE_PROJECT_3MF;
        boost::system::error_code ec;
        if (fs::exists(presets_path, ec)) {
            // Import the backed-up presets into the collections and select the
            // active ones. No SaveImported: nothing is written to the user's
            // preset files, the import stays in memory.
            PresetsConfigSubstitutions substitutions;
            size_t presets_imported = 0;
            std::tie(substitutions, presets_imported) = bundle->load_configbundle(
                presets_path.string(), PresetBundle::LoadConfigBundleAttributes{},
                ForwardCompatibilitySubstitutionRule::Enable);
            if (! substitutions.empty())
                BOOST_LOG_TRIVIAL(warning) << "Session restore: " << substitutions.size()
                                           << " configuration values were substituted while importing the session presets.";
            apply_system_derived_edits(presets_path, *bundle);
            // Load the currently selected presets into the GUI tabs.
            wxGetApp().load_current_presets();
        }
        if (fs::exists(project_path, ec)) {
            Plater *plater = wxGetApp().plater();
            if (plater != nullptr) {
                const std::string original_project = read_state_field(dir / FILE_STATE_JSON, "project_file");
                // Load through the normal project path so everything (config,
                // objects, undo/redo) initializes as with a manual open.
                plater->load_project(from_path(project_path), false);
                // Present the restored session as the user's original project,
                // not as the internal session file.
                if (! original_project.empty())
                    plater->set_project_filename(from_u8(original_project));
            }
        }
        // The backup served its purpose.
        discard_session(session_id);
        BOOST_LOG_TRIVIAL(info) << "Session " << session_id << " restored from " << dir.string();
        return true;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Session restore failed: " << ex.what();
        show_error(wxGetApp().mainframe,
            format_wxstr(_L("Failed to restore the previous session:\n%1%\n\nThe session backup was kept at:\n%2%"),
                wxString::FromUTF8(ex.what()), dir.string()));
        return false;
    }
}

void SessionBackup::schedule_restores(std::vector<std::string> session_ids)
{
    if (session_ids.empty())
        return;
    const std::string next = session_ids.front();
    session_ids.erase(session_ids.begin());
    wxGetApp().CallAfter([next, session_ids]() {
        restore_session(next);
        schedule_restores(session_ids);
    });
}

void SessionBackup::prompt_restore()
{
    const std::vector<CrashedSession> crashed = crashed_sessions();
    if (crashed.empty())
        return;
    set_prompt_pending(true);
    std::vector<std::string> to_restore;
    std::vector<std::string> to_discard;
    if (crashed.size() == 1) {
        MessageDialog dialog(nullptr,
            format_wxstr(_L("%1% did not shut down cleanly last time.\n\n"
                "Restore the previous session (project and unsaved preset changes)?"), SLIC3R_APP_NAME),
            _L("Crash recovery"), wxYES_NO | wxICON_QUESTION);
        dialog.SetButtonLabel(wxID_YES, _L("Restore session"));
        dialog.SetButtonLabel(wxID_NO,  _L("Discard backup"));
        if (dialog.ShowModal() == wxID_YES)
            to_restore.push_back(crashed.front().session_id);
        else
            to_discard.push_back(crashed.front().session_id);
    } else {
        wxDialog dialog(nullptr, wxID_ANY, _L("Crash recovery"), wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
        top->Add(new wxStaticText(&dialog, wxID_ANY,
            format_wxstr(_L("%1% did not shut down cleanly last time.\nSelect the sessions to restore:"),
                SLIC3R_APP_NAME)), 0, wxALL, 10);
        std::vector<wxCheckBox *> boxes;
        for (const CrashedSession &cs : crashed) {
            const std::time_t t = parse_iso_utc(cs.timestamp);
            wxString when = (t > 0) ? wxDateTime(t).Format(_L("%Y-%m-%d %H:%M")) : from_u8(cs.timestamp);
            std::string project = cs.project_file;
            if (! project.empty())
                project = fs::path(project).filename().string();
            wxCheckBox *cb = new wxCheckBox(&dialog, wxID_ANY,
                format_wxstr(_L("%1% \u2014 %2%"), when,
                    project.empty() ? _L("(no project)") : from_u8(project)));
            top->Add(cb, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
            boxes.push_back(cb);
        }
        top->Add(new wxStaticText(&dialog, wxID_ANY,
            _L("Sessions are restored one after another; the plater keeps the last restored project.\n"
               "Unchecked sessions are kept until they are restored, discarded, or expire.")), 0, wxALL, 10);
        wxBoxSizer *btns = new wxBoxSizer(wxHORIZONTAL);
        wxButton *select_all = new wxButton(&dialog, wxID_ANY, _L("Select all"));
        btns->Add(select_all, 0, wxRIGHT, 10);
        btns->AddStretchSpacer();
        wxButton *discard_btn = new wxButton(&dialog, wxID_ANY, _L("Discard selected"));
        wxButton *restore_btn = new wxButton(&dialog, wxID_OK, _L("Restore selected"));
        restore_btn->SetDefault();
        btns->Add(discard_btn, 0, wxRIGHT, 10);
        btns->Add(restore_btn, 0);
        top->Add(btns, 0, wxEXPAND | wxALL, 10);
        dialog.SetSizer(top);
        top->SetSizeHints(&dialog);
        select_all->Bind(wxEVT_BUTTON, [&boxes](wxCommandEvent &) {
            for (wxCheckBox *cb : boxes)
                cb->SetValue(true);
        });
        restore_btn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
            for (size_t i = 0; i < boxes.size(); ++ i)
                if (boxes[i]->GetValue())
                    to_restore.push_back(crashed[i].session_id);
            dialog.EndModal(wxID_OK);
        });
        discard_btn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
            for (size_t i = 0; i < boxes.size(); ++ i)
                if (boxes[i]->GetValue())
                    to_discard.push_back(crashed[i].session_id);
            dialog.EndModal(wxID_NO);
        });
        // Closing the dialog (Esc / window close) keeps all backups.
        dialog.ShowModal();
    }
    set_prompt_pending(false);
    for (const std::string &sid : to_discard)
        discard_session(sid);
    schedule_restores(std::move(to_restore));
}

} // namespace GUI
} // namespace Slic3r
