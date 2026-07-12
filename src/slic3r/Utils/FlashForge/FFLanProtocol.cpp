#include "FFLanProtocol.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

using boost::asio::ip::tcp;

// Returns true once the accumulated reply contains a terminating "ok" line.
// FlashForge closes a response with a line that trims to exactly "ok" (some
// firmwares append a token, e.g. "ok N"); everything before it is the payload.
bool response_has_ok(const std::string& s)
{
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t nl   = s.find('\n', pos);
        std::string       line = (nl == std::string::npos) ? s.substr(pos) : s.substr(pos, nl - pos);
        boost::trim(line);
        boost::to_lower(line);
        if (line == "ok" || (line.rfind("ok", 0) == 0 && line.size() > 2 && line[2] == ' '))
            return true;
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return false;
}

// Extract the text following "<key>:" on the line that contains it, trimmed.
std::string field_after(const std::string& body, const std::string& key)
{
    const std::size_t k = body.find(key);
    if (k == std::string::npos)
        return {};
    std::size_t start = k + key.size();
    std::size_t end   = body.find('\n', start);
    std::string v     = (end == std::string::npos) ? body.substr(start) : body.substr(start, end - start);
    boost::trim(v);
    return v;
}

// Parse "<cur>/<tar>" temperature pair.
void parse_temp_pair(const std::string& token, double& cur, double& tar)
{
    const std::size_t slash = token.find('/');
    if (slash == std::string::npos)
        return;
    try {
        cur = std::stod(token.substr(0, slash));
        tar = std::stod(token.substr(slash + 1));
    } catch (const std::exception&) {
    }
}

} // namespace

FFLanClient::FFLanClient() : m_socket(m_io) {}

FFLanClient::~FFLanClient() { close(); }

bool FFLanClient::is_connected() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_connected && m_socket.is_open();
}

void FFLanClient::close()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    close_locked();
}

void FFLanClient::close_locked()
{
    boost::system::error_code ignore;
    if (m_socket.is_open()) {
        m_socket.shutdown(tcp::socket::shutdown_both, ignore);
        m_socket.close(ignore);
    }
    m_connected = false;
}

bool FFLanClient::run_until(boost::system::error_code& op_ec, std::chrono::steady_clock::time_point deadline)
{
    m_io.restart();
    while (op_ec == boost::asio::error::would_block) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        m_io.run_for(std::min<std::chrono::milliseconds>(slice, std::chrono::milliseconds(100)));
    }
    if (op_ec == boost::asio::error::would_block) {
        // Deadline passed with the operation still pending: cancel it, drain the
        // resulting operation_aborted handler, and report a timeout.
        boost::system::error_code ignore;
        m_socket.cancel(ignore);
        m_io.restart();
        m_io.run();
        if (op_ec == boost::asio::error::would_block)
            op_ec = boost::asio::error::timed_out;
        return false;
    }
    return !op_ec;
}

bool FFLanClient::connect(const std::string& ip, unsigned short port, int timeout_ms)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    close_locked();

    try {
        // dev_ip is a numeric address in every OrcaSlicer LAN flow; if a hostname
        // is ever passed, make_address throws and we report a clean failure.
        tcp::endpoint ep(boost::asio::ip::make_address(ip), port);

        boost::system::error_code op_ec = boost::asio::error::would_block;
        m_socket.async_connect(ep, [&op_ec](const boost::system::error_code& ec) { op_ec = ec; });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        if (!run_until(op_ec, deadline) || !m_socket.is_open()) {
            BOOST_LOG_TRIVIAL(warning) << "FFLanClient: connect to " << ip << ":" << port
                                       << " failed: " << op_ec.message();
            close_locked();
            return false;
        }

        boost::system::error_code no_delay_ec;
        m_socket.set_option(tcp::no_delay(true), no_delay_ec);

        m_connected = true;
        m_ip        = ip;
        m_port      = port;
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "FFLanClient: connect exception: " << e.what();
        close_locked();
        return false;
    }
}

bool FFLanClient::write_all_locked(const char* data, std::size_t len, int timeout_ms)
{
    if (!m_connected)
        return false;
    boost::system::error_code op_ec = boost::asio::error::would_block;
    boost::asio::async_write(m_socket, boost::asio::buffer(data, len),
                             [&op_ec](const boost::system::error_code& ec, std::size_t) { op_ec = ec; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (!run_until(op_ec, deadline)) {
        if (op_ec != boost::asio::error::timed_out)
            close_locked(); // hard socket error: drop the connection
        return false;
    }
    return true;
}

bool FFLanClient::read_until_ok_locked(std::string& out, int timeout_ms)
{
    out.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<char, 4096> buf;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        boost::system::error_code op_ec = boost::asio::error::would_block;
        std::size_t               nread = 0;
        m_socket.async_read_some(boost::asio::buffer(buf),
                                 [&op_ec, &nread](const boost::system::error_code& ec, std::size_t n) {
                                     op_ec = ec;
                                     nread = n;
                                 });
        if (!run_until(op_ec, deadline)) {
            // eof / connection_reset means the printer dropped us: forget the
            // socket so the next call reconnects. A plain timeout keeps it.
            if (op_ec != boost::asio::error::timed_out && op_ec != boost::asio::error::operation_aborted)
                close_locked();
            return false;
        }
        if (nread > 0) {
            out.append(buf.data(), nread);
            if (response_has_ok(out))
                return true;
        }
    }
}

bool FFLanClient::send_command_locked(const std::string& cmd, std::string& response, int timeout_ms)
{
    if (!m_connected)
        return false;
    const std::string wire = "~" + cmd + "\r\n";
    if (!write_all_locked(wire.data(), wire.size(), timeout_ms))
        return false;
    return read_until_ok_locked(response, timeout_ms);
}

bool FFLanClient::send_command(const std::string& cmd, std::string& response, int timeout_ms)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return send_command_locked(cmd, response, timeout_ms);
}

// ---------------------------------------------------------------------------
// Session control
// ---------------------------------------------------------------------------

bool FFLanClient::control()
{
    std::string resp;
    return send_command("M601 S1", resp, CMD_TIMEOUT_MS);
}

bool FFLanClient::release()
{
    std::string resp;
    return send_command("M602", resp, CMD_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool FFLanClient::get_machine_info(MachineInfo& out)
{
    std::string body;
    if (!send_command("M115", body, CMD_TIMEOUT_MS))
        return false;

    out.machine_type  = field_after(body, "Machine Type:");
    out.machine_name  = field_after(body, "Machine Name:");
    out.firmware      = field_after(body, "Firmware:");
    out.serial_number = field_after(body, "SN:");
    out.mac_address   = field_after(body, "Mac Address:");

    // "X: 220 Y: 220 Z: 220" appears on a single line.
    try {
        const std::string x = field_after(body, "X:");
        const std::string y = field_after(body, "Y:");
        const std::string z = field_after(body, "Z:");
        if (!x.empty()) out.dim_x = std::atoi(x.c_str());
        if (!y.empty()) out.dim_y = std::atoi(y.c_str());
        if (!z.empty()) out.dim_z = std::atoi(z.c_str());
    } catch (const std::exception&) {
    }
    const std::string tc = field_after(body, "Tool Count:");
    if (!tc.empty())
        out.tool_count = std::atoi(tc.c_str());
    return true;
}

bool FFLanClient::get_status(StatusInfo& out)
{
    std::string body;
    if (!send_command("M119", body, CMD_TIMEOUT_MS))
        return false;

    out.machine_status = field_after(body, "MachineStatus:");
    out.move_mode      = field_after(body, "MoveMode:");
    out.current_file   = field_after(body, "CurrentFile:");
    const std::string led = field_after(body, "LED:");
    if (!led.empty())
        out.led = std::atoi(led.c_str());
    return true;
}

bool FFLanClient::get_temps(Temps& out)
{
    std::string body;
    if (!send_command("M105", body, CMD_TIMEOUT_MS))
        return false;

    // "T0:29.9/0.0 T1:0.0/0.0 B:27.6/0.0"
    const std::string t0 = field_after(body, "T0:");
    const std::string t1 = field_after(body, "T1:");
    const std::string b  = field_after(body, "B:");
    // field_after returns the rest of the line; keep only the first token.
    auto first_token = [](std::string s) {
        const std::size_t sp = s.find(' ');
        return sp == std::string::npos ? s : s.substr(0, sp);
    };
    if (!t0.empty()) parse_temp_pair(first_token(t0), out.t0_current, out.t0_target);
    if (!t1.empty()) parse_temp_pair(first_token(t1), out.t1_current, out.t1_target);
    if (!b.empty())  parse_temp_pair(first_token(b),  out.bed_current, out.bed_target);
    return true;
}

bool FFLanClient::get_progress(Progress& out)
{
    std::string body;
    if (!send_command("M27", body, CMD_TIMEOUT_MS))
        return false;

    // "SD printing byte 0/100"
    const std::size_t byte_kw = body.find("byte");
    if (byte_kw != std::string::npos) {
        std::string rest = body.substr(byte_kw + 4);
        boost::trim(rest);
        const std::size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            try {
                out.byte_current = std::atoll(rest.substr(0, slash).c_str());
                std::string total = rest.substr(slash + 1);
                out.byte_total    = std::atoll(total.c_str());
            } catch (const std::exception&) {
            }
        }
    }
    // "Layer: 0/0"
    const std::string layer = field_after(body, "Layer:");
    if (!layer.empty()) {
        const std::size_t slash = layer.find('/');
        if (slash != std::string::npos) {
            out.layer_current = std::atoi(layer.substr(0, slash).c_str());
            out.layer_total   = std::atoi(layer.substr(slash + 1).c_str());
        }
    }
    if (out.byte_total > 0)
        out.percent = static_cast<int>((out.byte_current * 100) / out.byte_total);
    return true;
}

// ---------------------------------------------------------------------------
// Job control
// ---------------------------------------------------------------------------

bool FFLanClient::upload_gcode(const std::string& remote_name, const std::string& data)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_connected)
        return false;

    const std::string path = "0:/user/" + remote_name;

    // 1) Announce the write and the exact byte count.
    std::string resp;
    const std::string cmd = "M28 " + std::to_string(data.size()) + " " + path;
    if (!send_command_locked(cmd, resp, CMD_TIMEOUT_MS)) {
        BOOST_LOG_TRIVIAL(error) << "FFLanClient: M28 failed for " << path;
        return false;
    }

    // 2) Stream exactly <size> raw gcode bytes on the same socket. Scale the
    //    timeout with the payload size (min 60s) so large files can complete.
    const int upload_timeout = std::max<int>(UPLOAD_TIMEOUT_MS,
                                             static_cast<int>(data.size() / 1024)); // ~1ms/KB floor
    if (!write_all_locked(data.data(), data.size(), upload_timeout)) {
        BOOST_LOG_TRIVIAL(error) << "FFLanClient: streaming gcode body failed for " << path;
        return false;
    }

    // 3) Close the file.
    if (!send_command_locked("M29", resp, CMD_TIMEOUT_MS)) {
        BOOST_LOG_TRIVIAL(error) << "FFLanClient: M29 (save) failed for " << path;
        return false;
    }
    return true;
}

bool FFLanClient::start_print(const std::string& remote_name)
{
    std::string resp;
    const std::string cmd = "M23 0:/user/" + remote_name;
    return send_command(cmd, resp, CMD_TIMEOUT_MS);
}

bool FFLanClient::stop()
{
    // NOTE: on AD5M Pro firmware v5.1.8, M26 did not reliably abort a malformed
    // job. It is still the documented stop command and is issued best-effort.
    std::string resp;
    return send_command("M26", resp, CMD_TIMEOUT_MS);
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

bool FFLanClient::set_extruder_temp(int celsius)
{
    std::string resp;
    return send_command("M104 S" + std::to_string(celsius), resp, CMD_TIMEOUT_MS);
}

bool FFLanClient::set_bed_temp(int celsius)
{
    std::string resp;
    return send_command("M140 S" + std::to_string(celsius), resp, CMD_TIMEOUT_MS);
}

bool FFLanClient::set_fan(bool on)
{
    std::string resp;
    return send_command(on ? "M106" : "M107", resp, CMD_TIMEOUT_MS);
}

bool FFLanClient::set_led(bool on)
{
    std::string resp;
    return send_command(on ? "M146 r255 g255 b255" : "M146 r0 g0 b0", resp, CMD_TIMEOUT_MS);
}

} // namespace Slic3r
