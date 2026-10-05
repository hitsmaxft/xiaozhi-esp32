#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <string>
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
    struct CallRecord {
        std::string id;
        std::string parent_id;
        std::string tool;
        std::string scope;
        uint32_t argument_crc = 0;
        uint32_t operation_id = 0;
        uint16_t generation = 0;
        uint16_t invocation_generation = 0;
        const char* state = "completed";
    };
    CallRecord* FindCall(const char* id);
    CallRecord* ReserveCall(const char* id, const char* parent_id, const char* tool,
                            const char* scope, uint16_t generation, uint32_t argument_crc,
                            uint32_t operation_id);
    cJSON* CallSnapshot(const CallRecord& call) const;
    void FinishCall(uint32_t operation_id, bool success);
    bool CompleteCall(CallRecord& call);
    bool CancelCall(CallRecord& call);

    int socket_ = -1;
    bool authorized_ = false;
    bool subscribed_ = false;
    bool media_queued_ = false;
    uint32_t boot_id_ = 0;
    uint32_t revision_ = 0;
    uint32_t event_seq_ = 0;
    uint32_t host_session_id_ = 0;
    uint32_t connection_epoch_ = 0;
    uint32_t scene_id_ = 4;
    uint32_t bitmap_operation_id_ = 0;
    uint32_t bitmap_expires_at_ms_ = 0;
    uint32_t bitmap_duration_ms_ = 0;
    uint16_t bitmap_width_ = 0;
    uint16_t bitmap_height_ = 0;
    bool bitmap_was_active_ = false;
    uint32_t last_heartbeat_ms_ = 0;
    uint32_t last_state_ms_ = 0;
    uint32_t next_operation_id_ = 0;
    std::array<CallRecord, 16> calls_{};
    size_t call_next_ = 0;
    uint32_t media_operation_id_ = 0;
    std::atomic<uint32_t> screen_cancelled_{0};
    std::vector<uint8_t> receive_;
    std::atomic<uint32_t> screen_pending_{0};
    std::atomic<uint32_t> screen_done_{0};
    std::atomic<uint32_t> screen_done_scene_{0};
};
