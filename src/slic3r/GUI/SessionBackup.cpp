#include "SessionBackup.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
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

#include <atomic>
#include <cstdio>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include <wx/timer.h>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

bool SessionBackup::s_prev_run_crashed = false;
bool SessionBackup::s_prompt_pending  = false;

namespace {

constexpr const char *FILE_PRESETS_INI = "session_presets.ini";
constexpr const char *FILE_PROJECT_3MF = "session_project.3mf";
constexpr const char *FILE_STATE_JSON  = "session_state.json";

std::atomic<bool> s_backup_running{false};

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

fs::path session_dir()
{
    return fs::path(Slic3r::data_dir()) / "session";
}

fs::path session_file(const char *name)
{
    return session_dir() / name;
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

// Serialize the edited (possibly unsaved) presets of all collections plus the
// names of the active presets, in the standard config bundle format. The
// format is deliberately identical to PresetBundle::export_configbundle() so
// that restore() can reuse PresetBundle::load_configbundle().
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
    c << "  \"app_version\": \""        << json_escape(SLIC3R_VERSION) << "\"," << std::endl;
    c << "  \"printer_technology\": \"" << json_escape(technology)     << "\"," << std::endl;
    c << "  \"project_file\": \""       << json_escape(project_file)   << "\"," << std::endl;
    c << "  \"timestamp\": \""          << json_escape(Slic3r::Utils::iso_utc_timestamp()) << "\"" << std::endl;
    c << "}" << std::endl;
    c.close();
    return true;
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

std::string read_state_project_file(const fs::path &json_path)
{
    // Minimal extraction of the "project_file" value. The file is written by
    // write_state_json() only, so a strict JSON parser is not needed.
    boost::nowide::ifstream c(json_path.string());
    if (! c.is_open())
        return std::string();
    std::string line;
    while (std::getline(c, line)) {
        const size_t key_pos = line.find("\"project_file\"");
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

} // anonymous namespace

bool SessionBackup::enabled()
{
    const AppConfig *cfg = wxGetApp().app_config.get();
    // Missing key = enabled (default on).
    return cfg != nullptr && (! cfg->has("session_backup_enabled") || cfg->get_bool("session_backup_enabled"));
}

void SessionBackup::startup_mark_running()
{
    AppConfig *cfg = wxGetApp().app_config.get();
    if (cfg == nullptr)
        return;
    // Remember whether the PREVIOUS run crashed before arming the flag for
    // this run, otherwise the crash of the previous run would be masked.
    s_prev_run_crashed = cfg->get("session_dirty") == "1";
    cfg->set("session_dirty", "1");
    cfg->save();
}

void SessionBackup::clean_exit()
{
    AppConfig *cfg = wxGetApp().app_config.get();
    if (cfg != nullptr && cfg->get("session_dirty") == "1") {
        cfg->set("session_dirty", "0");
        cfg->save();
    }
    boost::system::error_code ec;
    fs::remove_all(session_dir(), ec);
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
    if (! enabled() || wxGetApp().is_gcode_viewer() || s_prompt_pending)
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
        fs::create_directories(session_dir());
        // 1) Presets, incl. unsaved edits (also covers the no-project case).
        atomic_write(session_file(FILE_PRESETS_INI), write_presets_ini);
        // 2) The loaded project incl. embedded config. The export shares the
        //    code path of a manual project save (store_3mf) and runs on the
        //    main thread; for very large projects this may stall the UI for
        //    the duration of the export (accepted trade-off, see class comment).
        const fs::path project_path = session_file(FILE_PROJECT_3MF);
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
            boost::system::error_code ec;
            fs::remove(project_path, ec);
        }
        // 3) Session metadata.
        atomic_write(session_file(FILE_STATE_JSON), write_state_json);
        BOOST_LOG_TRIVIAL(debug) << "Session backup written to " << session_dir().string();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Session backup failed: " << ex.what();
    }
    s_backup_running = false;
}

bool SessionBackup::backup_exists()
{
    boost::system::error_code ec;
    return fs::exists(session_file(FILE_PRESETS_INI), ec) || fs::exists(session_file(FILE_PROJECT_3MF), ec);
}

void SessionBackup::discard()
{
    boost::system::error_code ec;
    fs::remove_all(session_dir(), ec);
}

bool SessionBackup::restore()
{
    try {
        PresetBundle *bundle = wxGetApp().preset_bundle.get();
        if (bundle == nullptr)
            return false;
        const fs::path presets_path = session_file(FILE_PRESETS_INI);
        const fs::path project_path = session_file(FILE_PROJECT_3MF);
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
                const std::string original_project = read_state_project_file(session_file(FILE_STATE_JSON));
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
        discard();
        BOOST_LOG_TRIVIAL(info) << "Previous session restored from " << session_dir().string();
        return true;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Session restore failed: " << ex.what();
        show_error(wxGetApp().mainframe,
            format_wxstr(_L("Failed to restore the previous session:\n%1%\n\nThe session backup was kept at:\n%2%"),
                wxString::FromUTF8(ex.what()), session_dir().string()));
        return false;
    }
}

} // namespace GUI
} // namespace Slic3r
