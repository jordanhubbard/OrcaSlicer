#ifndef __FLASHFORGE_PRINTER_AGENT_HPP__
#define __FLASHFORGE_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

class FFLanClient;

// Stable identifier written into FlashForge printer profiles as
// printer_agent="flashforge". NetworkAgentFactory keys the agent by this id.
#define FLASHFORGE_PRINTER_AGENT_ID "flashforge"

/**
 * FlashForgePrinterAgent - OPEN implementation of IPrinterAgent for FlashForge.
 *
 * This talks the classic FlashForge "M-code over TCP" LAN protocol directly
 * (see FFLanClient / FFLanProtocol) on port 8899. There is NO closed library:
 * everything runs over boost::asio TCP sockets, so the whole path is auditable
 * and shippable in an upstream open-source build.
 *
 * Verified against a real Adventurer 5M Pro (firmware v5.1.8):
 *   connect  -> ~M601 S1 (take control)
 *   status   -> ~M105 (temps) + ~M119 (state) + ~M27 (progress), polled
 *   print    -> ~M28 (+ raw stream) + ~M29 (save) then ~M23 (start)
 *   stop     -> ~M26
 *   release  -> ~M602
 *
 * LAN-only by design. Cloud/bind/cert/subscribe methods have no LAN equivalent
 * and are documented stubs (see the // TODO(cloud) notes). The agent is a
 * Utils-layer component and is UI-free (no wxWidgets); discovered devices and
 * live status cross the callback boundary as plain JSON strings.
 */
class FlashForgePrinterAgent : public IPrinterAgent
{
public:
    FlashForgePrinterAgent();
    ~FlashForgePrinterAgent() override;

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Certificates
    int  check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    // Agent Information
    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Print Job Operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

private:
    // Shared LAN upload+(optionally)start path used by the start_* entry points.
    int lan_send_gcode(const PrintParams& params, bool print_now, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

    // Background status-poll thread lifecycle.
    void start_status_poll(const std::string& dev_id);
    void stop_status_poll();
    void status_poll_loop(std::string dev_id, uint64_t generation);

    // Callback marshalling helpers (mirror MoonrakerPrinterAgent).
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);
    void dispatch_message(const std::string& dev_id, const std::string& payload);

private:
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;

    // Active LAN connection.
    mutable std::mutex             m_conn_mutex;
    std::shared_ptr<FFLanClient>   m_client;
    std::string                    m_dev_id;
    std::string                    m_dev_ip;
    std::string                    m_selected_machine;

    // Status-poll thread.
    std::thread           m_poll_thread;
    std::atomic<bool>     m_poll_stop{false};
    std::atomic<uint64_t> m_poll_generation{0};

    // Stored std::function callbacks (mirrors the other agents' registration).
    mutable std::mutex    m_cb_mutex;
    OnMsgArrivedFn        m_on_ssdp_msg_fn;
    OnPrinterConnectedFn  m_on_printer_connected_fn;
    GetSubscribeFailureFn m_on_subscribe_failure_fn;
    OnMessageFn           m_on_message_fn;
    OnMessageFn           m_on_user_message_fn;
    OnLocalConnectedFn    m_on_local_connect_fn;
    OnMessageFn           m_on_local_message_fn;
    QueueOnMainFn         m_queue_on_main_fn;
    OnServerErrFn         m_on_server_err_fn;
};

} // namespace Slic3r

#endif // __FLASHFORGE_PRINTER_AGENT_HPP__
