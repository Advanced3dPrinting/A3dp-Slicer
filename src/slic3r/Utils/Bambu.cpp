///|/ Copyright (c) Advanced3dPrinting LLC 2026
///|/
///|/ A3dp-Slicer is released under the terms of the AGPLv3 or higher
///|/
#include "Bambu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <curl/curl.h>

#include <wx/string.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

namespace {

// ---------------------------------------------------------------------------
// Minimal MQTT 3.1.1 client tunneled through libcurl's CONNECT_ONLY mode.
//
// CONNECT_ONLY (URL scheme https://) makes libcurl perform TCP + TLS handshake
// and stop before any protocol exchange; curl_easy_send/curl_easy_recv then
// expose the raw TLS stream. This works with every TLS backend libcurl was
// built with (OpenSSL on Linux, Schannel on Windows, SecureTransport on macOS)
// without linking OpenSSL ourselves. Verified against a real Bambu printer.
// ---------------------------------------------------------------------------

constexpr size_t MQTT_MAX_PACKET = 64 * 1024;

// MQTT 3.1.1 CONNACK return codes -> human readable
const char* mqtt_connack_reason(uint8_t rc)
{
    switch (rc) {
        case 1:  return "unacceptable protocol version";
        case 2:  return "identifier rejected";
        case 3:  return "server unavailable";
        case 4:  return "bad user name or password";   // wrong access code
        case 5:  return "not authorized";              // wrong access code
        default: return "unknown connection error";
    }
}

class MqttTunnel
{
public:
    MqttTunnel() = default;
    ~MqttTunnel() { disconnect(); }

    MqttTunnel(const MqttTunnel&) = delete;
    MqttTunnel& operator=(const MqttTunnel&) = delete;

    bool connected() const { return m_curl != nullptr; }

    // TCP + TLS (no cert verification - the printer uses a self-signed
    // certificate) + MQTT CONNECT with username/password.
    bool connect(const std::string &host, const std::string &user, const std::string &password,
        std::string &error, int timeout_s = 10)
    {
        disconnect();

        CURL *curl = ::curl_easy_init();
        if (! curl) {
            error = "curl init failed";
            return false;
        }
        m_curl = curl;
        m_errbuf.assign(CURL_ERROR_SIZE + 1, '\0');

        const std::string url = "https://" + host + ":8883/";
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_CONNECT_ONLY, 1L);           // raw TLS tunnel
        setopt(CURLOPT_SSL_VERIFYPEER, 0L);         // self-signed printer cert
        setopt(CURLOPT_SSL_VERIFYHOST, 0L);
        setopt(CURLOPT_NOSIGNAL, 1L);
        setopt(CURLOPT_CONNECTTIMEOUT, (long)timeout_s);
        setopt(CURLOPT_ERRORBUFFER, m_errbuf.data());

        CURLcode rc = ::curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            error = std::string("TLS connection failed: ") + curl_err(rc);
            disconnect();
            return false;
        }

        // MQTT CONNECT (3.1.1), clean session, username + password.
        std::vector<uint8_t> pkt;
        const std::string client_id = "a3dp-slicer-" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 1000000);
        pkt.push_back(0x10); // CONNECT
        std::vector<uint8_t> body;
        put_str(body, "MQTT");
        body.push_back(0x04);      // protocol level 4 (3.1.1)
        body.push_back(0xC2);      // clean session | username | password
        put_u16(body, 60);         // keep alive [s]
        put_str(body, client_id);
        put_str(body, user);
        put_str(body, password);
        append_varint(pkt, body.size());
        pkt.insert(pkt.end(), body.begin(), body.end());

        if (! send_all(pkt, error))
            return false;

        std::vector<uint8_t> connack;
        if (! read_packet(connack, timeout_s * 1000, error))
            return false;
        if (connack.size() < 4 || (connack[0] & 0xF0) != 0x20) {
            error = "unexpected reply to MQTT CONNECT";
            return false;
        }
        if (connack[3] != 0) {
            error = std::string("MQTT login rejected (") + mqtt_connack_reason(connack[3]) + ") - check the access code";
            return false;
        }
        return true;
    }

    void disconnect()
    {
        if (m_curl) {
            const uint8_t bye[] = { 0xE0, 0x00 }; // MQTT DISCONNECT
            size_t sent = 0;
            ::curl_easy_send(m_curl, bye, sizeof(bye), &sent);
            ::curl_easy_cleanup(m_curl);
            m_curl = nullptr;
        }
        m_rbuf.clear();
    }

    bool subscribe(const std::string &topic, std::string &error)
    {
        if (! connected()) {
            error = "not connected";
            return false;
        }
        std::vector<uint8_t> pkt;
        pkt.push_back(0x82); // SUBSCRIBE, qos 0
        std::vector<uint8_t> body;
        put_u16(body, m_packet_id++);
        put_str(body, topic);
        body.push_back(0x00); // requested qos
        append_varint(pkt, body.size());
        pkt.insert(pkt.end(), body.begin(), body.end());

        if (! send_all(pkt, error))
            return false;

        std::vector<uint8_t> suback;
        if (! read_packet(suback, 5000, error))
            return false;
        if (suback.size() < 5 || (suback[0] & 0xF0) != 0x90) {
            error = "unexpected reply to MQTT SUBSCRIBE";
            return false;
        }
        if (suback[4] == 0x80) {
            error = "MQTT subscription refused by printer";
            return false;
        }
        return true;
    }

    bool publish(const std::string &topic, const std::string &payload, std::string &error)
    {
        if (! connected()) {
            error = "not connected";
            return false;
        }
        std::vector<uint8_t> pkt;
        pkt.push_back(0x30); // PUBLISH, qos 0 (no packet id, no ack expected at MQTT level)
        std::vector<uint8_t> body;
        put_str(body, topic);
        body.insert(body.end(), payload.begin(), payload.end());
        append_varint(pkt, body.size());
        pkt.insert(pkt.end(), body.begin(), body.end());
        return send_all(pkt, error);
    }

    // Waits for the next PUBLISH packet. Returns false on timeout/transport
    // error (error set); on success topic_out/payload_out hold the message.
    bool wait_publish(std::string &topic_out, std::string &payload_out, int timeout_ms, std::string &error)
    {
        std::vector<uint8_t> pkt;
        if (! read_packet(pkt, timeout_ms, error))
            return false;
        if ((pkt[0] & 0xF0) != 0x30) {
            // CONNACK/SUBACK/PINGRESP etc. should not appear here; skip silently.
            error = "unexpected MQTT packet while waiting for a report";
            return false;
        }
        const uint8_t qos = (pkt[0] >> 1) & 0x03;
        size_t pos = 1;
        uint32_t rem_len = 0;
        size_t varint_len = 0;
        if (! decode_varint(pkt, pos, rem_len, varint_len))
            return false;
        pos += varint_len;
        if (pos + 2 > pkt.size()) {
            error = "malformed MQTT PUBLISH";
            return false;
        }
        const uint16_t topic_len = (uint16_t)((pkt[pos] << 8) | pkt[pos + 1]);
        pos += 2;
        if (pos + topic_len > pkt.size()) {
            error = "malformed MQTT PUBLISH";
            return false;
        }
        topic_out.assign(reinterpret_cast<const char*>(pkt.data() + pos), topic_len);
        pos += topic_len;
        if (qos > 0)
            pos += 2; // packet identifier
        payload_out.assign(reinterpret_cast<const char*>(pkt.data() + std::min(pos, pkt.size())),
            pkt.size() > pos ? pkt.size() - pos : 0);
        return true;
    }

private:
    static void put_u16(std::vector<uint8_t> &v, uint16_t x)
    {
        v.push_back((uint8_t)(x >> 8));
        v.push_back((uint8_t)(x & 0xFF));
    }

    static void put_str(std::vector<uint8_t> &v, const std::string &s)
    {
        put_u16(v, (uint16_t)s.size());
        v.insert(v.end(), s.begin(), s.end());
    }

    static void append_varint(std::vector<uint8_t> &v, size_t len)
    {
        do {
            uint8_t d = (uint8_t)(len % 128);
            len /= 128;
            if (len > 0)
                d |= 0x80;
            v.push_back(d);
        } while (len > 0);
    }

    // Decodes an MQTT remaining-length varint starting at pos (which points at
    // the length field); varint_len receives the number of bytes consumed.
    static bool decode_varint(const std::vector<uint8_t> &pkt, size_t pos, uint32_t &len_out, size_t &varint_len)
    {
        uint32_t multiplier = 1;
        uint32_t value = 0;
        varint_len = 0;
        for (size_t i = pos; i < pkt.size(); ++i, ++varint_len) {
            const uint8_t d = pkt[i];
            value += (uint32_t)(d & 0x7F) * multiplier;
            if ((d & 0x80) == 0) {
                len_out = value;
                return true;
            }
            multiplier *= 128;
            if (varint_len >= 4)
                return false;
        }
        return false; // incomplete (caller must read more bytes)
    }

    void setopt(CURLoption opt, const std::string &value)
    {
        ::curl_easy_setopt(m_curl, opt, value.c_str());
    }
    void setopt(CURLoption opt, long value)
    {
        ::curl_easy_setopt(m_curl, opt, value);
    }
    void setopt(CURLoption opt, void *value)
    {
        ::curl_easy_setopt(m_curl, opt, value);
    }

    const char* curl_err(CURLcode rc) const
    {
        const char *eb = m_errbuf.data();
        if (eb[0] != '\0')
            return eb;
        return ::curl_easy_strerror(rc);
    }

    bool send_all(const std::vector<uint8_t> &pkt, std::string &error)
    {
        size_t off = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (off < pkt.size()) {
            size_t sent = 0;
            CURLcode rc = ::curl_easy_send(m_curl, pkt.data() + off, pkt.size() - off, &sent);
            if (rc == CURLE_OK) {
                off += sent;
                continue;
            }
            if (rc == CURLE_AGAIN && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            error = std::string("MQTT send failed: ") + (rc == CURLE_AGAIN ? "timed out" : curl_err(rc));
            return false;
        }
        return true;
    }

    // Reads one complete MQTT packet (may pull several TLS records).
    bool read_packet(std::vector<uint8_t> &pkt, int timeout_ms, std::string &error)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::vector<uint8_t> tmp(4096);
        for (;;) {
            // Try to frame a packet out of what we already have.
            if (m_rbuf.size() >= 2) {
                uint32_t rem_len = 0;
                size_t varint_len = 0;
                if (decode_varint(m_rbuf, 1, rem_len, varint_len)) {
                    const size_t total = 1 + varint_len + rem_len;
                    if (total > MQTT_MAX_PACKET) {
                        error = "oversized MQTT packet from printer";
                        return false;
                    }
                    if (m_rbuf.size() >= total) {
                        pkt.assign(m_rbuf.begin(), m_rbuf.begin() + total);
                        m_rbuf.erase(m_rbuf.begin(), m_rbuf.begin() + total);
                        return true;
                    }
                }
                // else: incomplete varint, need more bytes
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "timed out waiting for MQTT data";
                return false;
            }
            size_t recvd = 0;
            CURLcode rc = ::curl_easy_recv(m_curl, tmp.data(), tmp.size(), &recvd);
            if (rc == CURLE_OK && recvd > 0) {
                m_rbuf.insert(m_rbuf.end(), tmp.begin(), tmp.begin() + recvd);
                continue;
            }
            if (rc == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            error = std::string("MQTT receive failed: ") + curl_err(rc);
            return false;
        }
    }

    CURL *m_curl = nullptr;
    std::vector<uint8_t> m_rbuf;
    std::vector<char>    m_errbuf;
    uint16_t             m_packet_id = 1;
};

// ---------------------------------------------------------------------------
// FTPS (implicit TLS, port 990) file transfer through libcurl.
// ---------------------------------------------------------------------------

struct FtpsCtx
{
    FILE      *file = nullptr;
    curl_off_t size = 0;
    PrintHost::ProgressFn progress_fn;
};

size_t ftps_read_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    FtpsCtx *ctx = static_cast<FtpsCtx*>(userdata);
    if (! ctx->file || ctx->size <= 0)
        return 0;
    return fread(buffer, 1, size * nitems, ctx->file);
}

size_t ftps_discard_cb(char*, size_t size, size_t nitems, void*)
{
    return size * nitems;
}

int ftps_progress_cb(void *userdata, curl_off_t, curl_off_t, curl_off_t ultotal, curl_off_t ulnow)
{
    FtpsCtx *ctx = static_cast<FtpsCtx*>(userdata);
    if (ctx->progress_fn) {
        Http::Progress progress;
        progress.ultotal = ultotal;
        progress.ulnow   = ulnow;
        bool cancel = false;
        ctx->progress_fn(progress, cancel);
        if (cancel)
            return 1; // abort
    }
    return 0;
}

// Common connection setup. Returns a ready easy handle or nullptr (error filled).
CURL* ftps_handle(const std::string &host, const std::string &user, const std::string &password,
    const std::string &url, std::vector<char> &errbuf, long timeout_s = 20)
{
    CURL *curl = ::curl_easy_init();
    if (! curl)
        return nullptr;
    errbuf.assign(CURL_ERROR_SIZE + 1, '\0');
    const std::string userpwd = user + ":" + password;
    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    ::curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // self-signed printer cert
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
    ::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    ::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf.data());
    return curl;
}

std::string ftps_error(CURLcode rc, const std::vector<char> &errbuf)
{
    const char *eb = errbuf.data();
    return (eb[0] != '\0') ? std::string(eb) : std::string(::curl_easy_strerror(rc));
}

// Verifies reachability + login by listing the FTP root.
bool ftps_check(const std::string &host, const std::string &user, const std::string &password, std::string &error)
{
    std::vector<char> errbuf;
    CURL *curl = ftps_handle(host, user, password, "ftps://" + host + ":990/", errbuf);
    if (! curl) {
        error = "curl init failed";
        return false;
    }
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ftps_discard_cb);
    ::curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L);
    CURLcode rc = ::curl_easy_perform(curl);
    ::curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        error = ftps_error(rc, errbuf);
        return false;
    }
    return true;
}

// Uploads source_path to "cache/<filename>" on the printer.
bool ftps_upload_file(const std::string &host, const std::string &user, const std::string &password,
    const fs::path &source_path, const std::string &remote_name,
    const PrintHost::ProgressFn &progress_fn, std::string &error)
{
    FILE *f = fopen(source_path.string().c_str(), "rb");
    if (! f) {
        error = "Cannot open local file for upload: " + source_path.string();
        return false;
    }
    fseek(f, 0, SEEK_END);
    const curl_off_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    CURL *curl = nullptr;
    // curl_easy_escape needs its own handle; a temporary one is fine.
    CURL *esc = ::curl_easy_init();
    std::string url;
    if (esc) {
        char *escaped = ::curl_easy_escape(esc, remote_name.c_str(), (int)remote_name.size());
        url = std::string("ftps://") + host + ":990/cache/" + (escaped ? escaped : remote_name);
        if (escaped)
            ::curl_free(escaped);
        ::curl_easy_cleanup(esc);
    } else
        url = std::string("ftps://") + host + ":990/cache/" + remote_name;

    std::vector<char> errbuf;
    curl = ftps_handle(host, user, password, url);
    if (! curl) {
        fclose(f);
        error = "curl init failed";
        return false;
    }

    FtpsCtx ctx;
    ctx.file = f;
    ctx.size = size;
    ctx.progress_fn = progress_fn;

    ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    ::curl_easy_setopt(curl, CURLOPT_READFUNCTION, &ftps_read_cb);
    ::curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    ::curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, size);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ftps_discard_cb);
    ::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ftps_progress_cb);
    ::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode rc = ::curl_easy_perform(curl);
    ::curl_easy_cleanup(curl);
    fclose(f);
    if (rc != CURLE_OK) {
        error = (rc == CURLE_ABORTED_BY_CALLBACK) ? std::string("Upload cancelled") : ftps_error(rc, errbuf);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else
                    out += c;
        }
    }
    return out;
}

std::string json_str(const std::string &s) { return "\"" + json_escape(s) + "\""; }

// Hand-built request JSON (not boost::property_tree::write_json, which quotes
// every scalar - the printer's parser expects real JSON booleans).

// Extracts "print"/"info" sub-object fields from a report payload.
bool parse_report(const std::string &payload, const std::string &section, pt::ptree &out)
{
    try {
        std::istringstream iss(payload);
        pt::ptree root;
        pt::read_json(iss, root);
        const auto child = root.get_child_optional(section);
        if (! child)
            return false;
        out = *child;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string to_lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

// Serial numbers are uppercase alphanumeric; sanitize user input defensively.
std::string sanitize_serial(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (std::isalnum((unsigned char)c))
            out.push_back((char)std::toupper((unsigned char)c));
    return out;
}

// Extracts the device serial from a "device/<serial>/report" topic.
std::string serial_from_topic(const std::string &topic)
{
    if (! boost::starts_with(topic, "device/"))
        return {};
    const size_t end = topic.find('/', 7);
    if (end == std::string::npos)
        return {};
    return topic.substr(7, end - 7);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Bambu
// ---------------------------------------------------------------------------

Bambu::Bambu(DynamicPrintConfig *config) :
    m_host        (config->opt_string("print_host")),
    m_access_code (config->opt_string("printhost_apikey")),
    m_serial_cfg  (sanitize_serial(config->opt_string("printhost_serial")))
{}

constexpr char BambuName[] = "Bambu (LAN)";
const char* Bambu::get_name() const { return BambuName; }

wxString Bambu::get_test_ok_msg() const
{
    wxString msg = _L("Connection to %s works correctly.");
    if (! m_serial.empty())
        msg += "\n" + _L("Serial number: %s (control channel via MQTT: OK)");
    else
        msg += "\n" + _L("Control channel (MQTT) unreachable: upload works, start the print from the printer screen.");
    return wxString::Format(msg, get_name(), m_serial);
}

wxString Bambu::get_test_failed_msg(wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s")
        % (boost::format(_u8L("Could not connect to %s")) % get_name())
        % std::string(msg.ToUTF8())).str());
}

bool Bambu::test(wxString &curl_msg) const
{
    curl_msg.Clear();
    if (m_host.empty()) {
        curl_msg = _L("Printer IP is not set.");
        return false;
    }
    if (m_access_code.empty()) {
        curl_msg = _L("Access code is not set (printer screen: Settings -> Network).");
        return false;
    }

    // 1) FTPS reachability + login (upload path)
    {
        std::string err;
        if (! ftps_check(m_host, "bblp", m_access_code, err)) {
            curl_msg = _L("FTP upload channel (port 990): ") + wxString::FromUTF8(err);
            return false;
        }
    }

    // 2) MQTT control channel: connect, subscribe to the wildcard report topic,
    //    discover the serial, then verify the request topic with get_status.
    MqttTunnel mqtt;
    std::string err;
    if (! mqtt.connect(m_host, "bblp", m_access_code, err)) {
        curl_msg = _L("MQTT control channel (port 8883): ") + wxString::FromUTF8(err);
        return false;
    }

    std::string serial = m_serial_cfg;
    {
        if (! mqtt.subscribe("device/+/report", err)) {
            curl_msg = _L("MQTT control channel (port 8883): ") + wxString::FromUTF8(err);
            return false;
        }
        std::string topic, payload;
        if (! mqtt.wait_publish(topic, payload, 5000, err)) {
            curl_msg = _L("MQTT connected, but the printer sent no status report: ") + wxString::FromUTF8(err);
            return false;
        }
        const std::string discovered = sanitize_serial(serial_from_topic(topic));
        if (serial.empty())
            serial = discovered;
        else if (! discovered.empty() && discovered != serial)
            BOOST_LOG_TRIVIAL(warning) << "Bambu: configured serial " << serial << " differs from discovered " << discovered;
    }

    {
        // Verify the request topic with a status query.
        const std::string req = "{\"printing\": false, \"command\": \"get_status\", \"sequence_id\": \"1\"}";
        if (! mqtt.publish("device/" + serial + "/request", req, err)) {
            curl_msg = _L("MQTT request failed: ") + wxString::FromUTF8(err);
            return false;
        }
        bool status_ok = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            std::string topic, payload;
            std::string err2;
            if (! mqtt.wait_publish(topic, payload, 2000, err2))
                break;
            pt::ptree report;
            if (parse_report(payload, "print", report) && ! report.empty()) {
                status_ok = true;
                break;
            }
        }
        if (! status_ok) {
            curl_msg = _L("MQTT connected, but the printer did not answer a status request (serial %s).");
            curl_msg = wxString::Format(curl_msg, wxString::FromUTF8(serial));
            return false;
        }
    }

    m_serial = serial;
    return true;
}

bool Bambu::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    if (m_host.empty()) {
        error_fn(_L("Printer IP is not set."));
        return false;
    }
    if (m_access_code.empty()) {
        error_fn(_L("Access code is not set (printer screen: Settings -> Network)."));
        return false;
    }

    fs::path filename = upload_data.upload_path.filename();
    if (filename.empty())
        filename = upload_data.source_path.filename();
    if (filename.empty()) {
        error_fn(_L("No file name for upload."));
        return false;
    }

    info_fn(wxString::FromUTF8(name), _L("Uploading gcode file to the printer (FTPS)..."));

    std::string err;
    if (! ftps_upload_file(m_host, "bblp", m_access_code, upload_data.source_path, filename.string(), progress_fn, err)) {
        error_fn(_L("FTP upload failed: ") + wxString::FromUTF8(err));
        return false;
    }

    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint) {
        info_fn(wxString::FromUTF8(name), _L("Upload finished. The file is in the printer's cache."));
        return true;
    }

    // Start the print via MQTT: device/{serial}/request <- print.project_file
    info_fn(wxString::FromUTF8(name), _L("Starting print via MQTT..."));

    MqttTunnel mqtt;
    if (! mqtt.connect(m_host, "bblp", m_access_code, err)) {
        error_fn(_L("The file was uploaded, but the MQTT control channel (port 8883) is unreachable: ")
            + wxString::FromUTF8(err) + "\n" + _L("Start the print from the printer screen."));
        return false;
    }

    std::string serial = m_serial_cfg;
    if (serial.empty()) {
        // Discover via the wildcard report topic (the printer announces itself).
        if (! mqtt.subscribe("device/+/report", err)) {
            error_fn(_L("The file was uploaded, but MQTT subscription failed: ")
                + wxString::FromUTF8(err) + "\n" + _L("Start the print from the printer screen."));
            return false;
        }
        std::string topic, payload;
        if (! mqtt.wait_publish(topic, payload, 5000, err)) {
            error_fn(_L("The file was uploaded, but the printer serial could not be discovered: ")
                + wxString::FromUTF8(err) + "\n" + _L("Set the serial in the printer connection dialog, or start the print from the printer screen."));
            return false;
        }
        serial = sanitize_serial(serial_from_topic(topic));
        if (serial.empty()) {
            error_fn(_L("The file was uploaded, but the printer serial could not be discovered.")
                + "\n" + _L("Set the serial in the printer connection dialog, or start the print from the printer screen."));
            return false;
        }
        m_serial = serial;
    }

    {
        // Subscribe to this printer's report topic to catch the command ack.
        if (! mqtt.subscribe("device/" + serial + "/report", err)) {
            error_fn(_L("The file was uploaded, but MQTT subscription failed: ")
                + wxString::FromUTF8(err) + "\n" + _L("Start the print from the printer screen."));
            return false;
        }

        // OpenBambuAPI print.project_file: local prints always use 0 ids and
        // url "ftp:///..." pointing at the uploaded file (cache/ for uploads).
        const std::string cache_name = "cache/" + filename.string();
        std::ostringstream req;
        req << "{\"print\": {"
            << "\"sequence_id\": \"1\", "
            << "\"command\": \"project_file\", "
            << "\"param\": " << json_str(cache_name) << ", "
            << "\"project_id\": \"0\", "
            << "\"profile_id\": \"0\", "
            << "\"task_id\": \"0\", "
            << "\"subtask_id\": \"0\", "
            << "\"subtask_name\": " << json_str(filename.stem().string()) << ", "
            << "\"url\": " << json_str("ftp:///" + cache_name) << ", "
            << "\"md5\": \"\", "
            << "\"timelapse\": false, "
            << "\"bed_type\": \"auto\", "
            << "\"use_ams\": false, "
            << "\"flow_cali\": false, "
            << "\"vibration_cali\": false, "
            << "\"layer_inspect\": false, "
            << "\"auto_recovery\": true, "
            << "\"allow_skip_parts\": false"
            << "}}";

        if (! mqtt.publish("device/" + serial + "/request", req.str(), err)) {
            error_fn(_L("The file was uploaded, but the print start command could not be sent: ")
                + wxString::FromUTF8(err) + "\n" + _L("Start the print from the printer screen."));
            return false;
        }
    }

    // Wait for the command acknowledgement on device/{serial}/report.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool acked = false;
    bool ok = false;
    std::string reason;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string topic, payload;
        std::string err2;
        if (! mqtt.wait_publish(topic, payload, 2000, err2))
            break; // timeout below
        pt::ptree report;
        if (! parse_report(payload, "print", report))
            continue;
        const auto cmd = report.get_optional<std::string>("command");
        if (! cmd || *cmd != "project_file")
            continue; // unrelated push_status etc.
        acked = true;
        const auto result = report.get_optional<std::string>("result");
        ok = result && to_lower_ascii(*result) == "success";
        const auto why = report.get_optional<std::string>("reason");
        if (why)
            reason = *why;
        break;
    }

    if (ok) {
        info_fn(wxString::FromUTF8(name), _L("Print started by the printer."));
        return true;
    }
    if (acked) {
        error_fn(_L("The printer rejected the print start (reason: %s). The file is in the printer's cache; start it from the printer screen.")
            % wxString::FromUTF8(reason));
        return false;
    }
    error_fn(_L("No acknowledgement from the printer after the print start command. The file is in the printer's cache; start it from the printer screen."));
    return false;
}

} // namespace Slic3r
