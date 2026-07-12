#ifndef __FF_LAN_PROTOCOL_HPP__
#define __FF_LAN_PROTOCOL_HPP__

#include <chrono>
#include <mutex>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace Slic3r {

// ============================================================================
// FFLanClient - open implementation of the classic FlashForge LAN protocol.
//
// FlashForge printers (Adventurer / Guider / Creator families, and the
// Adventurer 5M / 5M Pro this was verified against, firmware v5.1.8) expose a
// plain-TCP M-code control channel on port 8899. Each request is an ASCII line
//
//     ~<CMD>\r\n
//
// and the printer answers with one or more ASCII lines terminated by a line
// that is exactly "ok". This class speaks that protocol directly over
// boost::asio (already an OrcaSlicer dependency) with NO closed library.
//
// The protocol below was confirmed working on real hardware; see the per-method
// comments for the exact command/response.
//
// Thread-safety: every public method takes an internal recursive mutex, so the
// status-poll thread and the print/command paths can share a single connected
// client. Only one request is ever in flight on the socket at a time.
// ============================================================================
class FFLanClient
{
public:
    // Default control port for the FlashForge LAN M-code channel.
    static constexpr unsigned short DEFAULT_PORT = 8899;

    // Parsed ~M115 machine information.
    struct MachineInfo
    {
        std::string machine_type;   // "Flashforge Adventurer 5M Pro"
        std::string machine_name;   // "FF5M"
        std::string firmware;       // "v5.1.8"
        std::string serial_number;  // "SN..."
        std::string mac_address;
        int         dim_x = 0;      // build volume (mm)
        int         dim_y = 0;
        int         dim_z = 0;
        int         tool_count = 0;
    };

    // Parsed ~M119 status snapshot.
    struct StatusInfo
    {
        std::string machine_status; // READY | BUILDING_FROM_SD | BUSY | PAUSED
        std::string move_mode;      // READY | HOMING | MOVING
        std::string current_file;
        int         led = -1;       // -1 = unknown
    };

    // Parsed ~M105 temperatures (current/target, degrees C).
    struct Temps
    {
        double t0_current = 0.0, t0_target = 0.0; // tool 0
        double t1_current = 0.0, t1_target = 0.0; // tool 1 (if present)
        double bed_current = 0.0, bed_target = 0.0;
    };

    // Parsed ~M27 progress.
    struct Progress
    {
        long long byte_current = 0;
        long long byte_total   = 0;
        int       layer_current = 0;
        int       layer_total   = 0;
        int       percent       = 0; // derived from byte_current/byte_total
    };

    FFLanClient();
    ~FFLanClient();

    FFLanClient(const FFLanClient&)            = delete;
    FFLanClient& operator=(const FFLanClient&) = delete;

    // Open a TCP connection to <ip>:<port>. Returns false on timeout / failure.
    bool connect(const std::string& ip, unsigned short port = DEFAULT_PORT, int timeout_ms = 5000);
    void close();
    bool is_connected() const;

    std::string ip() const { return m_ip; }
    unsigned short port() const { return m_port; }

    // --- session control -----------------------------------------------------
    bool control();  // ~M601 S1  -> "Control Success ..."
    bool release();  // ~M602

    // --- queries -------------------------------------------------------------
    bool get_machine_info(MachineInfo& out); // ~M115
    bool get_status(StatusInfo& out);        // ~M119
    bool get_temps(Temps& out);              // ~M105
    bool get_progress(Progress& out);        // ~M27

    // --- job control ---------------------------------------------------------
    // Upload gcode bytes and store them as 0:/user/<remote_name>:
    //   ~M28 <size> 0:/user/<remote_name>  -> "Writing to file: ..."
    //   <size raw bytes streamed on the same socket>
    //   ~M29                               -> "Done saving file."
    // remote_name should already include the .gcode extension.
    bool upload_gcode(const std::string& remote_name, const std::string& data);

    bool start_print(const std::string& remote_name); // ~M23 0:/user/<remote_name>
    bool stop();                                       // ~M26

    // --- setters (standard M-codes) ------------------------------------------
    bool set_extruder_temp(int celsius); // ~M104 S<t>
    bool set_bed_temp(int celsius);      // ~M140 S<t>
    bool set_fan(bool on);               // ~M106 / ~M107
    bool set_led(bool on);               // ~M146 r.. g.. b..

    // Low-level: write "~<cmd>\r\n" and read until an "ok" line. Fills response
    // with the full raw reply (without the trailing "ok"). Returns false on
    // timeout / socket error.
    bool send_command(const std::string& cmd, std::string& response, int timeout_ms = 5000);

private:
    // Variants that assume m_mutex is already held.
    bool send_command_locked(const std::string& cmd, std::string& response, int timeout_ms);
    bool write_all_locked(const char* data, std::size_t len, int timeout_ms);
    bool read_until_ok_locked(std::string& out, int timeout_ms);
    void close_locked();

    // Pump the io_context until op_ec resolves (handler ran) or the deadline
    // passes. Mirrors the run_for loop used elsewhere in the tree (TCPConsole).
    bool run_until(boost::system::error_code& op_ec, std::chrono::steady_clock::time_point deadline);

    mutable std::recursive_mutex   m_mutex;
    boost::asio::io_context        m_io;
    boost::asio::ip::tcp::socket   m_socket;
    bool                           m_connected = false;
    std::string                    m_ip;
    unsigned short                 m_port = DEFAULT_PORT;

    // Per-request timeouts (ms). Uploads scale with payload size.
    static constexpr int CMD_TIMEOUT_MS      = 5000;
    static constexpr int UPLOAD_TIMEOUT_MS   = 60000;
};

} // namespace Slic3r

#endif // __FF_LAN_PROTOCOL_HPP__
