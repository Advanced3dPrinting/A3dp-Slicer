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
#include "PrintHost.hpp"

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
// auto-probe 7125->80 from v1), plus on-demand filament slot sync (v2) and
// websocket print start with options + filament->toolhead map_table (v3).
class SnapmakerU1 : public Moonraker
{
public:
    SnapmakerU1(DynamicPrintConfig *config) : Moonraker(config) {}
    ~SnapmakerU1() override = default;

    const char* get_name() const override { return "Snapmaker U1 (Moonraker)"; }
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;

    // v3: "Send to printer" with StartPrint. On firmware >= 1.3.0 the upload is
    // posted without the print trigger and the print is started with a
    // websocket JSON-RPC server.files.start_local_print carrying the print-start
    // options (bed_level / flow_calibrate / time_lapse_camera as 0/1 ints) and
    // the filament->toolhead map_table derived from the current per-extruder
    // filament assignment. Older firmware (or no websocket-capable libcurl)
    // falls back to the plain upload + print=true Moonraker path.
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    // GET /printer/objects/query?print_task_config&filament_detect, then match
    // each loaded slot against the filament presets in the given PresetBundle
    // (nullptr = the running app's bundle). printer_model (e.g. "Snapmaker U1")
    // ranks the printer's own preset family ("@SnapmakerU1") ahead of the
    // vendor's legacy presets. Poll on demand only (connection test / manual
    // refresh), no background thread.
    bool get_filament_slots(std::vector<FilamentSlot> &out, wxString &error, PresetBundle *preset_bundle = nullptr,
                            const std::string &printer_model = std::string()) const;

protected:
    // Captures moonraker_version (the U1 firmware version, read from /etc/VERSION)
    // and computes whether server.files.start_local_print is available.
    void on_server_info(const std::string &moonraker_version) const override;

private:
    // Firmware >= 1.3.0 applies the print options of server.files.start_local_print
    // (Snapmaker's own client uses it from 1.3.0 on).
    static bool firmware_supports_print_options(const std::string &firmware_version);

    // Sends one JSON-RPC request over a short-lived websocket connection to
    // /websocket and waits for its answer (Moonraker does not serve
    // server.files.start_local_print over HTTP). Sets result_json to the
    // answer's result, or error to the reason it failed.
    bool ws_jsonrpc_request(const std::string &method, const std::string &params_json,
                            long timeout_seconds, std::string &result_json, std::string &error) const;

    // Fetches the filament slots and maps each extruder's selected filament
    // preset to the toolhead whose slot matched the same preset. Unmatched
    // extruders are left out (firmware default: extruder i -> toolhead i).
    // bundle must not be null.
    PrintHostPrintStartOptions build_print_start_options(PresetBundle *bundle, wxString &error) const;

    // Bundle whose per-extruder filament assignment feeds the map_table.
    // Virtual so tests can inject a bundle without a running wx app.
    virtual PresetBundle* get_print_preset_bundle() const;

    // mutable: test() and upload() are const, but the captured firmware state
    // is per-instance connection state (single-threaded upload path).
    mutable std::string m_fw_version;
    mutable bool        m_fw_print_options_supported = false;
};

} // namespace Slic3r

#endif // slic3r_Snapmaker_hpp_
