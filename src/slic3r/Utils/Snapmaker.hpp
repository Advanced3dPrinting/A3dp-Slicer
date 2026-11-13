///|/ Copyright (c) 2026 Advanced3dPrinting LLC
///|/
///|/ A3dp-Slicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Snapmaker_hpp_
#define slic3r_Snapmaker_hpp_

#include <string>
#include <vector>
#include <wx/string.h>

#include "Moonraker.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class PresetBundle;

// One toolhead/spool slot as reported by the Snapmaker U1 firmware
// (Klipper object print_task_config + NFC data from filament_detect).
struct FilamentSlot
{
    int         tool           = -1;    // 0-based toolhead index
    bool        loaded         = false; // filament_exist
    std::string type;                   // normalized material type ("PLA", "PLA-CF", ...)
    std::string raw_sub_type;           // as reported ("CF10", "Silk", ...) before normalization
    std::string color;                  // "#RRGGBBAA" as reported, may be empty
    std::string vendor;
    std::string sku;
    int         bed_temp        = 0;   // NFC tag BED_TEMP, 0 = unknown
    int         nozzle_temp_max = 0;   // NFC tag HOTEND_MAX_TEMP, 0 = unknown
    // Filament preset matched to this slot by vendor + type + sub-type +
    // color distance (port of OrcaSlicer SnapmakerPrinterAgent matching).
    // Empty = no matching preset.
    std::string matched_preset;
};

// Snapmaker U1 print host: stock Moonraker underneath (upload/print, port
// auto-probe 7125->80 from v1), plus on-demand filament slot sync.
class SnapmakerU1 : public Moonraker
{
public:
    SnapmakerU1(DynamicPrintConfig *config) : Moonraker(config) {}
    ~SnapmakerU1() override = default;

    const char* get_name() const override { return "Snapmaker U1 (Moonraker)"; }
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;

    // GET /printer/objects/query?print_task_config&filament_detect, then match
    // each loaded slot against the filament presets in the given PresetBundle
    // (nullptr = the running app's bundle). printer_model (e.g. "Snapmaker U1")
    // ranks the printer's own preset family ("@SnapmakerU1") ahead of the
    // vendor's legacy presets. Poll on demand only (connection test / manual
    // refresh), no background thread.
    bool get_filament_slots(std::vector<FilamentSlot> &out, wxString &error, PresetBundle *preset_bundle = nullptr,
                            const std::string &printer_model = std::string()) const;
};

} // namespace Slic3r

#endif // slic3r_Snapmaker_hpp_
