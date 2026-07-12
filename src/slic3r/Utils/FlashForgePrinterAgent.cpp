#include "FlashForgePrinterAgent.hpp"
#include "FlashForge/FFLanProtocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

constexpr const char* FLASHFORGE_AGENT_VERSION = "1.0.0 (LAN/8899)";

// Poll cadence for the background status thread.
constexpr int  POLL_INTERVAL_MS = 2000;
constexpr int  POLL_SLICE_MS    = 100; // wake granularity so stop() is responsive

// FlashForge LAN discovery: broadcast a fixed 20-byte probe to UDP port 48899;
// responders reply with a fixed-layout datagram carrying the printer name (at
// offset 0) and serial number (at offset 0x92). This mirrors the verified probe
// already used by Slic3r::Flashforge. Discovery is best-effort; manual-IP add
// (dev_ip supplied by the PhysicalPrinter flow) remains the primary path.
constexpr unsigned short FF_DISCOVERY_PORT = 48899;

const std::array<unsigned char, 20> FF_DISCOVERY_MESSAGE = {
    0x77, 0x77, 0x77, 0x2e, 0x75, 0x73, 0x72, 0x22,
    0x65, 0x36, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Extract a NUL-terminated ASCII field of at most len bytes at offset off.
std::string ff_field(const unsigned char* data, std::size_t size, std::size_t off, std::size_t len)
{
    if (off >= size)
        return {};
    len = std::min(len, size - off);
    std::string s(reinterpret_cast<const char*>(data + off), len);
    const auto nul = s.find('\0');
    if (nul != std::string::npos)
        s.resize(nul);
    boost::trim(s);
    return s;
}

// Read a whole file into memory. Returns false if it cannot be opened.
bool read_file_bytes(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    f.seekg(0, std::ios::beg);
    if (len > 0)
        out.resize(static_cast<std::size_t>(len));
    if (len > 0)
        f.read(&out[0], len);
    return static_cast<bool>(f) || f.eof();
}

// Map a FlashForge MachineStatus string to a Bambu-style gcode_state, so the
// Device tab (which parses the Bambu "print" JSON shape) can render it.
std::string map_gcode_state(const std::string& ff_status)
{
    if (ff_status == "READY")            return "IDLE";
    if (ff_status == "BUILDING_FROM_SD") return "RUNNING";
    if (ff_status == "PAUSED")           return "PAUSE";
    if (ff_status == "BUSY")             return "PREPARE";
    return "IDLE";
}

int map_print_stage(const std::string& ff_status)
{
    if (ff_status == "BUILDING_FROM_SD") return 1; // printing
    if (ff_status == "PAUSED")           return 2; // paused
    return 0;
}

// Ensure a destination name that FlashForge accepts (basename + .gcode).
std::string remote_gcode_name(const PrintParams& params)
{
    namespace fs = boost::filesystem;
    std::string name;
    if (!params.dst_file.empty())
        name = fs::path(params.dst_file).filename().string();
    else if (!params.task_name.empty())
        name = params.task_name;
    else if (!params.project_name.empty())
        name = params.project_name;
    else if (!params.filename.empty())
        name = fs::path(params.filename).filename().string();
    if (name.empty())
        name = "orca_print";
    if (!boost::iends_with(name, ".gcode") && !boost::iends_with(name, ".gx"))
        name += ".gcode";
    return name;
}

// Local gcode file to send: prefer the explicit sliced output (dst_file), else
// the source filename.
std::string local_gcode_path(const PrintParams& params)
{
    if (!params.dst_file.empty())
        return params.dst_file;
    return params.filename;
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

FlashForgePrinterAgent::FlashForgePrinterAgent() = default;

FlashForgePrinterAgent::~FlashForgePrinterAgent()
{
    stop_status_poll();
    std::shared_ptr<FFLanClient> client;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        client = std::move(m_client);
    }
    if (client)
        client->close();
}

void FlashForgePrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    m_cloud_agent = std::move(cloud);
    // LAN-only agent: the cloud handle is retained for interface parity but not
    // used. TODO(cloud): FlashForge WAN would need its own clientId/accessToken.
}

// ============================================================================
// Callback marshalling (mirror MoonrakerPrinterAgent)
// ============================================================================

void FlashForgePrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn fn;
    QueueOnMainFn      queue;
    {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        fn    = m_on_local_connect_fn;
        queue = m_queue_on_main_fn;
    }
    if (!fn)
        return;
    auto call = [state, dev_id, msg, fn]() { fn(state, dev_id, msg); };
    if (queue) queue(call); else call();
}

void FlashForgePrinterAgent::dispatch_printer_connected(const std::string& dev_id)
{
    OnPrinterConnectedFn fn;
    QueueOnMainFn        queue;
    {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        fn    = m_on_printer_connected_fn;
        queue = m_queue_on_main_fn;
    }
    if (!fn)
        return;
    auto call = [dev_id, fn]() { fn(dev_id); };
    if (queue) queue(call); else call();
}

void FlashForgePrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn   local_fn;
    OnMessageFn   cloud_fn;
    QueueOnMainFn queue;
    {
        std::lock_guard<std::mutex> lock(m_cb_mutex);
        local_fn = m_on_local_message_fn;
        cloud_fn = m_on_message_fn;
        queue    = m_queue_on_main_fn;
    }
    if (!local_fn && !cloud_fn)
        return;
    auto call = [dev_id, payload, local_fn, cloud_fn]() {
        if (local_fn) { local_fn(dev_id, payload); return; }
        if (cloud_fn) cloud_fn(dev_id, payload);
    };
    if (queue) queue(call); else call();
}

// ============================================================================
// Communication
// ============================================================================

int FlashForgePrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) dev_id; (void) json_str; (void) qos; (void) flag;
    // Cloud relay: no LAN equivalent.
    // TODO(cloud): implement over FlashForge WAN once a session exists.
    return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

int FlashForgePrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username,
                                            std::string password, bool use_ssl)
{
    (void) username; (void) password; (void) use_ssl; // FlashForge LAN has no auth/TLS.
    if (dev_ip.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    // Tear down any previous connection first.
    stop_status_poll();

    auto client = std::make_shared<FFLanClient>();
    if (!client->connect(dev_ip, FFLanClient::DEFAULT_PORT)) {
        BOOST_LOG_TRIVIAL(error) << "FlashForgePrinterAgent: TCP connect to " << dev_ip << " failed";
        dispatch_local_connect(ConnectStatusFailed, dev_id, "connect_failed");
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
    // Take control of the printer (~M601 S1). Non-fatal if it fails on some
    // firmwares, but log it.
    if (!client->control())
        BOOST_LOG_TRIVIAL(warning) << "FlashForgePrinterAgent: ~M601 control handshake did not confirm";

    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        m_client = client;
        m_dev_id = dev_id;
        m_dev_ip = dev_ip;
        if (m_selected_machine.empty())
            m_selected_machine = dev_id;
    }

    dispatch_local_connect(ConnectStatusOk, dev_id, "0");
    dispatch_printer_connected(dev_id);
    start_status_poll(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::disconnect_printer()
{
    stop_status_poll();

    std::shared_ptr<FFLanClient> client;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        client = std::move(m_client);
        m_dev_id.clear();
        m_dev_ip.clear();
    }
    if (client) {
        client->release(); // ~M602 relinquish control
        client->close();
    }
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos; (void) flag; (void) dev_id;
    // Best-effort bridge: accept a {"cmd":"<raw M-code>"} JSON and forward it.
    std::shared_ptr<FFLanClient> client;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        client = m_client;
    }
    if (!client || !client->is_connected())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    try {
        auto j = nlohmann::json::parse(json_str, nullptr, false);
        if (!j.is_discarded() && j.contains("cmd") && j["cmd"].is_string()) {
            std::string resp;
            return client->send_command(j["cmd"].get<std::string>(), resp)
                       ? BAMBU_NETWORK_SUCCESS
                       : BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
        }
    } catch (const std::exception&) {
    }
    // TODO(cloud): full JSON->M-code command dispatcher for the Device tab.
    return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

// ============================================================================
// Certificates (no concept in the FlashForge LAN protocol)
// ============================================================================

int  FlashForgePrinterAgent::check_cert() { return BAMBU_NETWORK_SUCCESS; }
void FlashForgePrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    (void) dev_id; (void) lan_only;
}

// ============================================================================
// Discovery (best-effort UDP probe; manual-IP add is the primary path)
// ============================================================================

bool FlashForgePrinterAgent::start_discovery(bool start, bool sending)
{
    (void) sending;
    if (!start)
        return true;

    // 1) Re-announce an already-connected device so the Device tab keeps it.
    OnMsgArrivedFn ssdp_fn;
    std::string    dev_id, dev_ip;
    {
        std::lock_guard<std::mutex> cb(m_cb_mutex);
        ssdp_fn = m_on_ssdp_msg_fn;
    }
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        dev_id = m_dev_id;
        dev_ip = m_dev_ip;
    }
    if (ssdp_fn && !dev_id.empty() && !dev_ip.empty()) {
        nlohmann::json j;
        j["dev_id"]        = dev_id;
        j["dev_name"]      = dev_id;
        j["dev_ip"]        = dev_ip;
        j["connect_type"]  = "lan";
        j["bind_state"]    = "free";
        j["sec_link"]      = "secure";
        j["ssdp_version"]  = "v1";
        j["printer_agent"] = FLASHFORGE_PRINTER_AGENT_ID;
        ssdp_fn(j.dump());
    }

    // 2) Best-effort local-subnet UDP broadcast (port 48899, verified probe).
    //    Each responder's datagram carries the printer name and serial directly.
    //    Runs detached; never blocks the caller.
    std::thread([ssdp_fn]() {
        if (!ssdp_fn)
            return;
        try {
            boost::asio::io_context      io;
            boost::asio::ip::udp::socket sock(io, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
            sock.set_option(boost::asio::socket_base::broadcast(true));

            boost::asio::ip::udp::endpoint bcast(boost::asio::ip::address_v4::broadcast(), FF_DISCOVERY_PORT);
            boost::system::error_code      ec;
            sock.send_to(boost::asio::buffer(FF_DISCOVERY_MESSAGE), bcast, 0, ec);

            // Collect responses for ~800ms, emitting each parsed printer once.
            std::set<std::string> seen;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            while (std::chrono::steady_clock::now() < deadline) {
                boost::system::error_code      recv_ec = boost::asio::error::would_block;
                std::array<unsigned char, 512> buf{};
                boost::asio::ip::udp::endpoint from;
                std::size_t                    n = 0;
                sock.async_receive_from(boost::asio::buffer(buf), from,
                                        [&recv_ec, &n](const boost::system::error_code& e, std::size_t got) {
                                            recv_ec = e;
                                            n       = got;
                                        });
                io.restart();
                io.run_for(std::chrono::milliseconds(150));
                if (recv_ec == boost::asio::error::would_block) {
                    sock.cancel(recv_ec);
                    io.restart();
                    io.run();
                    continue;
                }
                if (recv_ec || n < 0xC4)
                    continue;

                const std::string ip     = from.address().to_string();
                const std::string name   = ff_field(buf.data(), n, 0x00, 32);
                const std::string serial = ff_field(buf.data(), n, 0x92, 32);
                if (name.empty() && serial.empty())
                    continue;
                const std::string dev_id = serial.empty() ? ip : serial;
                if (!seen.insert(dev_id).second)
                    continue;

                nlohmann::json j;
                j["dev_id"]        = dev_id;
                j["dev_name"]      = name.empty() ? dev_id : name;
                j["dev_ip"]        = ip;
                j["connect_type"]  = "lan";
                j["bind_state"]    = "free";
                j["sec_link"]      = "secure";
                j["ssdp_version"]  = "v1";
                j["printer_agent"] = FLASHFORGE_PRINTER_AGENT_ID;
                ssdp_fn(j.dump());
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(debug) << "FlashForgePrinterAgent: UDP discovery probe skipped: " << e.what();
        }
    }).detach();

    return true;
}

// ============================================================================
// Binding (LAN has no account binding; report benign success)
// ============================================================================

int FlashForgePrinterAgent::ping_bind(std::string ping_code)
{
    (void) ping_code;
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) sec_link;
    // Confirm the device by querying it directly over the LAN protocol.
    FFLanClient client;
    if (!client.connect(dev_ip, FFLanClient::DEFAULT_PORT, 3000))
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    FFLanClient::MachineInfo mi;
    const bool ok = client.get_machine_info(mi);
    client.close();
    if (!ok)
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;

    detect.dev_id       = mi.serial_number.empty() ? dev_ip : mi.serial_number;
    detect.dev_name     = mi.machine_name.empty() ? mi.machine_type : mi.machine_name;
    detect.model_id     = mi.machine_type;
    detect.version      = mi.firmware;
    detect.bind_state   = "free";
    detect.connect_type = "lan";
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link,
                                 std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    (void) dev_ip; (void) dev_id; (void) sec_link; (void) timezone; (void) improved;
    // LAN-only: nothing to bind against a cloud account.
    // TODO(cloud): FlashForge WAN binding.
    if (update_fn)
        update_fn(BAMBU_NETWORK_SUCCESS, 0, "");
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::unbind(std::string dev_id)
{
    (void) dev_id;
    return BAMBU_NETWORK_SUCCESS; // TODO(cloud): FlashForge WAN unbind.
}

int FlashForgePrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket)
        ticket->clear();
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_server_err_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string FlashForgePrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    return m_selected_machine;
}

int FlashForgePrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_conn_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Agent Information
// ============================================================================

AgentInfo FlashForgePrinterAgent::get_agent_info_static()
{
    return AgentInfo{FLASHFORGE_PRINTER_AGENT_ID, "FlashForge", FLASHFORGE_AGENT_VERSION,
                     "FlashForge printer agent (open LAN/8899 M-code protocol)"};
}

// ============================================================================
// Print Job Operations
// ============================================================================

int FlashForgePrinterAgent::lan_send_gcode(const PrintParams& params, bool print_now,
                                           OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    std::shared_ptr<FFLanClient> client;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        client = m_client;
    }
    if (!client || !client->is_connected())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    const std::string src = local_gcode_path(params);
    if (src.empty() || !boost::filesystem::exists(src))
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;

    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Reading G-code...");

    std::string bytes;
    if (!read_file_bytes(src, bytes))
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;

    const std::string remote_name = remote_gcode_name(params);

    if (update_fn)
        update_fn(PrintingStageUpload, 0, "Uploading G-code...");
    if (!client->upload_gcode(remote_name, bytes)) {
        if (update_fn)
            update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, "Upload failed");
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    if (print_now) {
        if (cancel_fn && cancel_fn())
            return BAMBU_NETWORK_ERR_CANCELED;
        if (update_fn)
            update_fn(PrintingStageSending, 0, "Starting print...");
        if (!client->start_print(remote_name)) {
            if (update_fn)
                update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, "Start failed");
            return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
        }
    }

    if (update_fn)
        update_fn(PrintingStageFinished, 100, print_now ? "Print started" : "File uploaded");
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn,
                                        WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    (void) wait_fn;
    // No FlashForge cloud path; treat a "managed" print as a LAN upload+start.
    // TODO(cloud): route through FlashForge WAN when a session exists.
    return lan_send_gcode(params, /*print_now=*/true, update_fn, cancel_fn);
}

int FlashForgePrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn,
                                                          WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    (void) wait_fn;
    // The cloud "record" (history) half has no LAN equivalent; do the real
    // LAN upload+start. TODO(cloud): register job history via FlashForge WAN.
    return lan_send_gcode(params, /*print_now=*/true, update_fn, cancel_fn);
}

int FlashForgePrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn,
                                                       WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    (void) wait_fn;
    // Upload only (no ~M23 start).
    return lan_send_gcode(params, /*print_now=*/false, update_fn, cancel_fn);
}

int FlashForgePrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return lan_send_gcode(params, /*print_now=*/true, update_fn, cancel_fn);
}

int FlashForgePrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    (void) cancel_fn;
    // Start a file already resident on the printer -> ~M23.
    std::shared_ptr<FFLanClient> client;
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        client = m_client;
    }
    if (!client || !client->is_connected())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    const std::string remote_name = remote_gcode_name(params);
    const bool        ok          = client->start_print(remote_name);
    const int         rc          = ok ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    if (update_fn)
        update_fn(ok ? PrintingStageFinished : PrintingStageERROR, rc, ok ? "Print started" : "Start failed");
    return rc;
}

// ============================================================================
// Status poll thread
// ============================================================================

void FlashForgePrinterAgent::start_status_poll(const std::string& dev_id)
{
    m_poll_stop.store(false);
    const uint64_t gen = ++m_poll_generation;
    m_poll_thread = std::thread([this, dev_id, gen]() { status_poll_loop(dev_id, gen); });
}

void FlashForgePrinterAgent::stop_status_poll()
{
    m_poll_stop.store(true);
    ++m_poll_generation;
    if (m_poll_thread.joinable())
        m_poll_thread.join();
}

void FlashForgePrinterAgent::status_poll_loop(std::string dev_id, uint64_t generation)
{
    while (!m_poll_stop.load() && generation == m_poll_generation.load()) {
        std::shared_ptr<FFLanClient> client;
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            client = m_client;
        }
        if (!client || !client->is_connected())
            break;

        FFLanClient::Temps      temps;
        FFLanClient::StatusInfo status;
        FFLanClient::Progress   progress;
        const bool ok_t = client->get_temps(temps);
        const bool ok_s = client->get_status(status);
        const bool ok_p = client->get_progress(progress);

        if (ok_t || ok_s || ok_p) {
            // Status JSON modelled on the Bambu "print"/push_status shape the
            // Device tab already parses (see MoonrakerPrinterAgent). Fields not
            // provided by FlashForge are simply omitted.
            // TODO(devtab): confirm exact key set DevManager expects for FF.
            nlohmann::json payload;
            auto& print = payload["print"];
            print["command"]            = "push_status";
            print["msg"]                = 0;
            print["support_mqtt_alive"] = true;
            print["dev_id"]             = dev_id;

            if (ok_s) {
                print["gcode_state"]    = map_gcode_state(status.machine_status);
                print["mc_print_stage"] = map_print_stage(status.machine_status);
                if (!status.current_file.empty()) {
                    print["gcode_file"]   = status.current_file;
                    print["subtask_name"] = status.current_file;
                }
                if (status.led >= 0)
                    print["lights_report"] = status.led ? "on" : "off";
            }
            if (ok_t) {
                print["nozzle_temper"]        = temps.t0_current;
                print["nozzle_target_temper"] = temps.t0_target;
                print["bed_temper"]           = temps.bed_current;
                print["bed_target_temper"]    = temps.bed_target;
            }
            if (ok_p) {
                print["mc_percent"]     = progress.percent;
                print["layer_num"]      = progress.layer_current;
                print["total_layer_num"] = progress.layer_total;
            }
            const auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            print["t_utc"] = now_ms;

            dispatch_message(dev_id, payload.dump());
        }

        // Sleep in small slices so stop() is responsive.
        for (int waited = 0; waited < POLL_INTERVAL_MS && !m_poll_stop.load() &&
                             generation == m_poll_generation.load();
             waited += POLL_SLICE_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLICE_MS));
        }
    }
}

// ============================================================================
// Callback registration
// ============================================================================

int FlashForgePrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_ssdp_msg_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_printer_connected_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_subscribe_failure_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_user_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_local_connect_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_on_local_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashForgePrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(m_cb_mutex);
    m_queue_on_main_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
