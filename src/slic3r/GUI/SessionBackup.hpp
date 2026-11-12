#ifndef slic3r_SessionBackup_hpp_
#define slic3r_SessionBackup_hpp_

#include <string>

namespace Slic3r {
namespace GUI {

// Crash session backup & restore.
//
// Periodically snapshots the current session into <data_dir>/session/ so it can
// be restored after an unclean shutdown:
//   session_presets.ini  - the edited presets of all collections (incl. unsaved
//                          changes) + the names of the active presets, in the
//                          standard config bundle format.
//   session_project.3mf  - the loaded project incl. embedded config (only if
//                          the plater is not empty).
//   session_state.json   - app version, printer technology, original project
//                          file name, timestamp.
//
// A crash is detected through the AppConfig flag "session_dirty", which is set
// at startup and cleared only on a clean exit (wxApp::OnExit). All files are
// written atomically (.tmp + rename). The whole backup runs on the main thread
// and is debounced: schedule_backup() arms a single-shot wxTimer which is
// restarted on every change, so the backup fires once, 10 s after the LAST
// change. Timer-triggered exports are skipped while another backup is running.
//
// Main thread only - every entry point is called from wx event handlers.
class SessionBackup
{
public:
    // Feature toggle. AppConfig key "session_backup_enabled", default on.
    static bool enabled();

    // True if the previous run was not shut down cleanly.
    static bool previous_run_crashed() { return s_prev_run_crashed; }

    // Call once at startup (before any dialog is shown): remembers the
    // previous run's crash flag and arms the flag for this run.
    static void startup_mark_running();

    // Call from wxApp::OnExit: clears the crash flag and removes the backup.
    static void clean_exit();

    // Schedule a backup that fires delay_ms after the LAST call (debounce,
    // single-shot). delay_ms == 0 fires as soon as the event loop allows.
    static void schedule_backup(int delay_ms = 10000);

    // Synchronous backup on the calling (main) thread.
    static void backup_now();

    static bool backup_exists();

    // Remove the session backup without restoring.
    static void discard();

    // Apply the backed-up presets and load the backed-up project.
    // Returns false on failure (caller falls back to a normal startup).
    // The backup is deleted on success, kept on failure.
    static bool restore();

    // While the "restore previous session?" prompt is unanswered, all backups
    // are suppressed: the new run's own startup changes must never overwrite
    // the crashed session's backup before the user has decided.
    static void set_prompt_pending(bool pending) { s_prompt_pending = pending; }

private:
    static bool s_prev_run_crashed;
    static bool s_prompt_pending;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SessionBackup_hpp_
