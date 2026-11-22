///|/ Copyright (c) Advanced3dPrinting LLC 2026
///|/
///|/ A3dp-Slicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Bambu_hpp_
#define slic3r_Bambu_hpp_

#include <string>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Bambu Lab printers over the LAN, no cloud account required:
//  - G-code upload over implicit FTPS on port 990 (user "bblp", password = LAN
//    access code). Files go to the printer's "cache" directory.
//  - Print control over MQTT on port 8883 with TLS and "bblp" authentication.
//    Requests are published to device/{serial}/request, responses arrive on
//    device/{serial}/report. The serial number is discovered by subscribing to
//    the wildcard topic device/+/report (or taken from printhost_serial).
//
// Protocol references (ported payloads):
//  - Doridian/OpenBambuAPI, mqtt.md: local MQTT broker auth (bblp + LAN access
//    code), device/{id}/report + device/{id}/request topics, print.project_file
//    request structure, report structure with sequence_id/result/reason.
//  - FTPS: implicit TLS on port 990; the server (vsftpd) requires TLS session
//    reuse on the data channel, which libcurl negotiates on its own.
class Bambu : public PrintHost
{
public:
    explicit Bambu(DynamicPrintConfig *config);
    ~Bambu() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }

    // Serial number seen during the last test()/upload() (discovery or echo).
    // Empty if the control channel was never reached.
    const std::string& discovered_serial() const { return m_serial; }

private:
    std::string m_host;        // printer IP or hostname
    std::string m_access_code; // LAN access code (also the FTPS password)
    std::string m_serial_cfg;  // configured serial (printhost_serial), may be empty
    mutable std::string m_serial; // discovered / confirmed serial (set by const test()/upload())
};

} // namespace Slic3r

#endif // slic3r_Bambu_hpp_
