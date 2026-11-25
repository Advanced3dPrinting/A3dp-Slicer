///|/ Copyright (c) Prusa Research 2020 - 2023 David Kocík @kocikdav, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef GUI_PROCESS_HPP
#define GUI_PROCESS_HPP

#include <string>
#include <vector>


class wxWindow;
class wxString;

namespace Slic3r {
namespace GUI {

// Start a new slicer instance, optionally with a file to open.
void start_new_slicer(const wxString *path_to_open = nullptr, bool single_instance = false, bool delete_after_load = false);
void start_new_slicer(const std::vector<wxString>& files, bool single_instance = false, bool delete_after_load = false);

// Start a new G-code viewer instance, optionally with a file to open.
void start_new_gcodeviewer(const wxString *path_to_open = nullptr);
// Open a file dialog, ask the user to select a new G-code to open, start a new G-code viewer.
void start_new_gcodeviewer_open_file(wxWindow *parent = nullptr);

// Start a new instance of THIS executable (slicer mode) with extra command
// line arguments and the same data directory. Unlike start_new_slicer(), the
// running executable itself is started, not a SLIC3R_APP_CMD sibling, so this
// also works for renamed / side-by-side installs. The caller is responsible
// for adding "--no-single-instance" when the new instance must not forward
// its command line to a running one. Returns false when the spawn failed.
bool start_new_slicer_with_extra_args(const std::vector<std::string> &extra_args);

} // namespace GUI
} // namespace Slic3r

#endif // GUI_PROCESS_HPP
