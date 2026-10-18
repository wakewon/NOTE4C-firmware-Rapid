/**
 * @file ap_transfer_server.h
 * @brief WiFi AP + HTTP Server for image transfer
 *
 * Provides:
 * - WiFi AP (SSID: InkScreen-AP, Password: 12345678)
 * - HTTP Server at 192.168.4.1
 * - HTML page for image upload
 * - Browser-side measured-profile 09k colour conversion
 * - Save to SPIFFS
 */

#ifndef AP_TRANSFER_SERVER_H
#define AP_TRANSFER_SERVER_H

#include <atomic>
#include <mutex>
#include <string>
#include <functional>
#include <esp_timer.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace rawdraw {

/**
 * @brief AP Transfer Server for WiFi image upload
 */
class ApTransferServer {
public:
    ApTransferServer();
    ~ApTransferServer();

    // Start AP + HTTP server
    void Start();

    // Start HTTP server on the current Wi-Fi station interface
    bool StartLan(const std::string& ip_address);
    
    // Stop AP + HTTP server
    void Stop();

    // Check if server is running
    bool IsRunning() const { return server_ != nullptr; }
    // esp_timer time of the last HTTP connection (or of the start), 0 if
    // never started. Read by the app task for the local-service idle timeout.
    int64_t LastActivityUs() const {
        return last_activity_us_.load(std::memory_order_acquire);
    }
    bool IsApMode() const { return mode_ == TransferMode::kAp; }
    bool IsLanMode() const { return mode_ == TransferMode::kLan; }

    // State callback
    enum ServerState {
        kStopped,
        kApStarted,
        kClientConnected,
        kReceivingImage,
        kProcessingImage,
        kImageSaved,
        kError,
    };
    
    void SetStateCallback(std::function<void(ServerState, const std::string&)> callback);

    // Image received callback (called when image saved to SPIFFS)
    void SetImageReceivedCallback(std::function<void(const char* photo_id)> callback);

    void SetSettingsChangedCallback(std::function<void(int slideshow_interval_minutes)> callback);
    void SetPhotosChangedCallback(std::function<void()> callback);
    void SetShowPhotoCallback(std::function<bool(const std::string& photo_id)> callback);

private:
    enum class TransferMode {
        kNone,
        kAp,
        kLan,
    };

    // Start/Stop run on the app task, the ap_web_control task and the AP
    // start task; this serializes them. Recursive because Stop() may run from
    // a path that already holds it (a failed start).
    std::recursive_mutex lifecycle_mutex_;
    std::atomic<int64_t> last_activity_us_{0};
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    // The AP has taken over the Wi-Fi driver and must hand it back (or stop
    // it) on release.
    bool ap_owns_radio_ = false;
    volatile bool running_ = false;
    volatile bool starting_ = false;
    TaskHandle_t start_task_ = nullptr;
    std::string ap_ip_ = "192.168.4.1";
    TransferMode mode_ = TransferMode::kNone;

    std::function<void(ServerState, const std::string&)> state_callback_;
    std::function<void(const char* photo_id)> image_received_callback_;
    std::function<void(int slideshow_interval_minutes)> settings_changed_callback_;
    std::function<void()> photos_changed_callback_;
    std::function<bool(const std::string& photo_id)> show_photo_callback_;

    bool StartAccessPoint();
    void ReleaseAccessPoint();
    const std::string& GetApIp() const { return ap_ip_; }
    bool StartHttpServer();
    bool RegisterHandlers(httpd_handle_t server);
    void MarkActivity() {
        last_activity_us_.store(esp_timer_get_time(), std::memory_order_release);
    }
    static void StartTask(void* arg);
    void RunStartSequence();

    // HTTP handlers
    static esp_err_t IndexHandler(httpd_req_t* req);
    static esp_err_t UploadHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t SettingsHandler(httpd_req_t* req);
    static esp_err_t PhotosHandler(httpd_req_t* req);
    static esp_err_t PhotoHandler(httpd_req_t* req);
    static esp_err_t PhotoMetaHandler(httpd_req_t* req);
    static esp_err_t PhotoMoveHandler(httpd_req_t* req);
    static esp_err_t PhotoShowHandler(httpd_req_t* req);
    
    // Notify state change
    void NotifyState(ServerState state, const std::string& message);
    
};

}  // namespace rawdraw

#endif  // AP_TRANSFER_SERVER_H
