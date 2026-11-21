#ifndef slic3r_SessionBackup_hpp_
#define slic3r_SessionBackup_hpp_

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// Crash session backup & restore, multi-instance safe.
//
// Every editor instance owns a PRIVATE session directory
//   <data_dir>/session/<session-id>/
//     session_presets.ini  - the edited presets of all collections (incl.
//                            unsaved changes) + the names of the active
//                            presets, in the standard config bundle format.
//     session_project.3mf  - the loaded project incl. embedded config (only
//                            if the plater is not empty).
//     session_state.json   - app version, pid, session start timestamp,
//                            printer technology, original project file name,
//                            last-save timestamp.
// where <session-id> is "<pid>-<unix start time>", unique per launch, so two
// simultaneous instances never write to the same files and cannot clobber
// each other's backups.
//
// Liveness is tracked with per-instance marker files (no shared registry
// file, therefore no cross-process read-modify-write locking):
//   <data_dir>/session/<session-id>.live
// containing the owner's pid and a process start-time token. The marker is
// written (atomically) BEFORE the session directory is created, so a session
// dir is never visible to another instance without its marker. The marker is
// removed on a clean exit. On startup, a session dir whose marker is missing
// or whose owner process is dead (pid existence + start-token verified, so a
// recycled pid cannot masquerade as a live instance) is a crashed candidate.
//
// Restore prompt: exactly one crashed session gets the plain yes/no prompt;
// several crashed sessions are listed with per-session checkboxes (timestamp
// + project name) offering restore/discard of the selected ones.
//
// Lifecycle: restore deletes the session dir after a successful load; discard
// deletes it; a clean exit deletes the instance's own dir + marker. On
// startup, session dirs older than N days (AppConfig key
// "session_backup_max_age_days", default 30, <= 0 disables) are purged.
//
// All files are written atomically (.tmp + rename). The whole backup runs on
// the main thread and is debounced: schedule_backup() arms a single-shot
// wxTimer which is restarted on every change, so the backup fires once, 10 s
// after the LAST change. Timer-triggered exports are skipped while another
// backup is running.
//
// Main thread only - every entry point is called from wx event handlers.
class SessionBackup
{
public:
    struct CrashedSession
    {
        std::string session_id;    // directory name under <data_dir>/session/
        std::string project_file;  // original project path, may be empty
        std::string timestamp;     // iso utc of the last backup
        std::string app_version;
    };

    // Feature toggle. AppConfig key "session_backup_enabled", default on.
    static bool enabled();

    // Call once at startup (before any dialog is shown): registers this
    // instance's session id + liveness marker, migrates the pre-multi-
    // instance flat backup layout, and purges stale session dirs.
    static void startup_mark_running();

    // Call from wxApp::OnExit: removes this instance's session dir + marker.
    static void clean_exit();

    // Schedule a backup that fires delay_ms after the LAST call (debounce,
    // single-shot). delay_ms == 0 fires as soon as the event loop allows.
    static void schedule_backup(int delay_ms = 10000);

    // Synchronous backup on the calling (main) thread.
    static void backup_now();

    // While the "restore previous session?" prompt is unanswered, all backups
    // are suppressed: the new run's own startup changes must never overwrite
    // a crashed session's backup before the user has decided.
    static void set_prompt_pending(bool pending) { s_prompt_pending = pending; }

    // Sessions left behind by instances that did not shut down cleanly
    // (newest first).
    static std::vector<CrashedSession> crashed_sessions();

    // Remove one crashed session's backup without restoring.
    static void discard_session(const std::string &session_id);

    // Apply one session's backed-up presets and load its backed-up project.
    // Returns false on failure (the backup is kept); the session dir is
    // deleted on success.
    static bool restore_session(const std::string &session_id);

    // Show the restore prompt(s): a single crashed session gets the plain
    // yes/no dialog, several get a checkbox list. Chosen restores/discards
    // are scheduled via CallAfter. No-op when there is nothing to restore.
    static void prompt_restore();

private:
    // Restore the given sessions one after another (chained via CallAfter).
    static void schedule_restores(std::vector<std::string> session_ids);

    static bool s_prompt_pending;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SessionBackup_hpp_
