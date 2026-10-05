#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

#include <cJSON.h>

class DeviceUseWifi {
public:
    static DeviceUseWifi& GetInstance();
    void Start();

private:
    DeviceUseWifi() = default;
    static void Task(void* arg);
    void Run();
    bool Connect();
    void Disconnect();
    bool SendJson(cJSON* message);
    bool SendCompact(uint8_t kind, const uint8_t* payload, size_t length);
    bool SendAll(const uint8_t* data, size_t length);
    void ProcessBytes(const uint8_t* data, size_t length);
    void HandleMessage(cJSON* message);
    void Reply(int id, cJSON* result, const char* error = nullptr);
    void Event(uint8_t kind, uint32_t operation_id, int32_t value);
    void Heartbeat();
    void StateNotice();
    cJSON* StateSnapshot() const;
    bool WasSeen(uint32_t operation_id);

    int socket_ = -1;
    bool authorized_ = false;
    bool subscribed_ = false;
    bool media_queued_ = false;
    uint32_t boot_id_ = 0;
    uint32_t revision_ = 0;
    uint32_t event_seq_ = 0;
    uint32_t host_session_id_ = 0;
    uint32_t scene_id_ = 4;
    uint32_t bitmap_operation_id_ = 0;
    uint32_t bitmap_expires_at_ms_ = 0;
    uint32_t bitmap_duration_ms_ = 0;
    uint16_t bitmap_width_ = 0;
    uint16_t bitmap_height_ = 0;
    bool bitmap_was_active_ = false;
    uint32_t last_heartbeat_ms_ = 0;
    uint32_t last_state_ms_ = 0;
    std::array<uint32_t, 16> recent_ids_{};
    size_t recent_next_ = 0;
    std::vector<uint8_t> receive_;
    std::atomic<uint32_t> screen_pending_{0};
    std::atomic<uint32_t> screen_done_{0};
    std::atomic<uint32_t> screen_done_scene_{0};
};
