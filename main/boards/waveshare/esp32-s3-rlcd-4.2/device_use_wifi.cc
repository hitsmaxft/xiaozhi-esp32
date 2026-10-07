#include "device_use_wifi.h"

#include <cerrno>
#include <cstring>
#include <climits>
#include <string>
#include <utility>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <wifi_manager.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "custom_lcd_display.h"

#if __has_include("local_config.h")
#include "local_config.h"
#endif

namespace {
constexpr const char* kTag = "device_use_wifi";
constexpr uint8_t kMagic = 0xd5;
constexpr uint8_t kJsonFrame = 0xa7;
constexpr uint8_t kCompactFrame = 0xa8;
constexpr size_t kMaxRequest = 4096;
constexpr uint16_t kPort = 8766;

void Write16(uint8_t* p, uint16_t value) {
    p[0] = value & 0xff;
    p[1] = value >> 8;
}

void Write32(uint8_t* p, uint32_t value) {
    for (int index = 0; index < 4; ++index) p[index] = (value >> (index * 8)) & 0xff;
}

uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint32_t Crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xffffffff;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return crc ^ 0xffffffff;
}

uint16_t Crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xffff;
    for (size_t index = 0; index < length; ++index) {
        crc ^= uint16_t(data[index]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
        }
    }
    return crc;
}

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

const char* Expression(uint32_t scene_id) {
    switch (scene_id) {
        case 1: return "happy";
        case 2: return "sad";
        case 3: return "surprised";
        default: return "neutral";
    }
}

bool Integer(const cJSON* value, int min, int max) {
    return cJSON_IsNumber(value) && value->valuedouble >= min &&
           value->valuedouble <= max && value->valuedouble == value->valueint;
}

bool CallId(const char* id) {
    if (!id) return false;
    const size_t length = strlen(id);
    if (length == 0 || length > 48) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':')) return false;
    }
    return true;
}

uint32_t ArgumentsCrc(const char* tool, const cJSON* args) {
    char* encoded = cJSON_PrintUnformatted(args);
    if (!encoded) return 0;
    std::string canonical(tool);
    canonical.push_back('\n');
    canonical += encoded;
    cJSON_free(encoded);
    return Crc32(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
}

bool DecodeBitmap(const char* encoded, size_t expected, std::vector<uint8_t>& output) {
    if (!encoded) return false;
    const size_t length = strlen(encoded);
    if (length == 0 || length % 4 != 0 || length > 3200) return false;
    output.clear();
    output.reserve(expected);
    for (size_t i = 0; i < length; i += 4) {
        uint32_t value = 0;
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = encoded[i + j];
            int digit = -1;
            if (c >= 'A' && c <= 'Z') digit = c - 'A';
            else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
            else if (c >= '0' && c <= '9') digit = c - '0' + 52;
            else if (c == '+') digit = 62;
            else if (c == '/') digit = 63;
            else if (c == '=' && i + 4 == length && j >= 2) {
                digit = 0;
                ++padding;
            }
            if (digit < 0 || (padding && c != '=')) return false;
            value = (value << 6) | digit;
        }
        if (padding == 1 && (value & 0xff) != 0) return false;
        if (padding == 2 && (value & 0xffff) != 0) return false;
        for (int j = 0; j < 3 - padding; ++j) {
            if (output.size() == expected) return false;
            output.push_back((value >> (16 - j * 8)) & 0xff);
        }
    }
    return output.size() == expected;
}

CustomLcdDisplay* RlcdDisplay() {
    return static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
}

cJSON* ParseLiteral(const char* text) { return cJSON_Parse(text); }
}  // namespace

DeviceUseWifi& DeviceUseWifi::GetInstance() {
    static DeviceUseWifi instance;
    return instance;
}

void DeviceUseWifi::Start() {
#if defined(RLCD_DEVICE_USE_HOST) && defined(RLCD_DEVICE_USE_TOKEN)
    boot_id_ = esp_random() & 0x7fffffff;
    if (boot_id_ == 0) boot_id_ = 1;
    xTaskCreate(&Task, "device_use_wifi", 10240, this, 4, nullptr);
#else
    ESP_LOGI(kTag, "Device Use Wi-Fi disabled: no private host/token build configuration");
#endif
}

void DeviceUseWifi::Task(void* arg) { static_cast<DeviceUseWifi*>(arg)->Run(); }

bool DeviceUseWifi::Connect() {
#if defined(RLCD_DEVICE_USE_HOST) && defined(RLCD_DEVICE_USE_TOKEN)
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    if (inet_pton(AF_INET, RLCD_DEVICE_USE_HOST, &address.sin_addr) != 1) return false;
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ < 0) return false;
    timeval send_timeout{.tv_sec = 2, .tv_usec = 0};
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    if (connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        Disconnect();
        return false;
    }
    timeval receive_timeout{.tv_sec = 0, .tv_usec = 200000};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    receive_.clear();
    authorized_ = false;
    subscribed_ = false;
    ++connection_epoch_;
    if (connection_epoch_ == 0) ++connection_epoch_;
    cJSON* hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(hello, "id", 1);
    cJSON_AddStringToObject(hello, "method", "device/hello");
    cJSON* params = cJSON_AddObjectToObject(hello, "params");
    cJSON_AddStringToObject(params, "token", RLCD_DEVICE_USE_TOKEN);
    cJSON_AddStringToObject(params, "board", "waveshare-esp32-s3-rlcd-4.2");
    cJSON_AddNumberToObject(params, "bootId", boot_id_);
    cJSON_AddNumberToObject(params, "connectionEpoch", connection_epoch_);
    cJSON_AddNumberToObject(params, "lastEventSeq", event_seq_);
    bool sent = SendJson(hello);
    cJSON_Delete(hello);
    if (!sent) Disconnect();
    return sent;
#else
    return false;
#endif
}

void DeviceUseWifi::Disconnect() {
    if (socket_ >= 0) {
        shutdown(socket_, SHUT_RDWR);
        close(socket_);
        socket_ = -1;
    }
    authorized_ = false;
    receive_.clear();
}

bool DeviceUseWifi::SendAll(const uint8_t* data, size_t length) {
    while (length > 0) {
        int written = send(socket_, data, length, 0);
        if (written <= 0) return false;
        data += written;
        length -= written;
    }
    return true;
}

bool DeviceUseWifi::SendJson(cJSON* message) {
    char* json = cJSON_PrintUnformatted(message);
    if (!json) return false;
    size_t length = strlen(json);
    if (length == 0 || length > kMaxRequest) {
        cJSON_free(json);
        return false;
    }
    uint8_t header[10] = {kMagic, kJsonFrame};
    Write32(header + 2, length);
    Write32(header + 6, Crc32(reinterpret_cast<const uint8_t*>(json), length));
    bool sent = SendAll(header, sizeof(header)) &&
                SendAll(reinterpret_cast<const uint8_t*>(json), length);
    cJSON_free(json);
    return sent;
}

bool DeviceUseWifi::SendCompact(uint8_t kind, const uint8_t* payload, size_t length) {
    if (length > 31) return false;
    uint8_t packet[64] = {kMagic, kCompactFrame, kind, static_cast<uint8_t>(length)};
    memcpy(packet + 6, payload, length);
    uint8_t checksum[2 + 31] = {kind, static_cast<uint8_t>(length)};
    memcpy(checksum + 2, payload, length);
    Write16(packet + 4, Crc16(checksum, length + 2));
    return SendAll(packet, length + 6);
}

void DeviceUseWifi::Event(uint8_t kind, uint32_t operation_id, int32_t value) {
    uint8_t payload[21]{};
    Write32(payload, ++event_seq_);
    Write32(payload + 4, revision_);
    Write32(payload + 8, operation_id);
    Write32(payload + 12, static_cast<uint32_t>(value));
    Write32(payload + 16, boot_id_);
    payload[20] = kind;
    if (!SendCompact(2, payload, sizeof(payload))) Disconnect();
}

void DeviceUseWifi::Heartbeat() {
    uint8_t payload[13]{};
    Write32(payload, boot_id_);
    Write32(payload + 4, NowMs());
    Write32(payload + 8, revision_);
    payload[12] = 1;  // TCP connected; no motor or local speech lane.
    if (!SendCompact(1, payload, sizeof(payload))) Disconnect();
}

void DeviceUseWifi::StateNotice() {
    if (!subscribed_) return;
    uint8_t payload[25]{};
    Write32(payload, boot_id_);
    Write32(payload + 4, revision_);
    Write32(payload + 8, NowMs());
    Write16(payload + 12, scene_id_);
    Write16(payload + 14, 0);
    Write32(payload + 16, 0);
    Write32(payload + 20, 0);
    payload[24] = 1 | (RlcdDisplay()->TemporaryBitmapActive() ? 16 : 0);
    if (!SendCompact(3, payload, sizeof(payload))) Disconnect();
    last_state_ms_ = NowMs();
}

cJSON* DeviceUseWifi::StateSnapshot() const {
    cJSON* state = cJSON_CreateObject();
    cJSON_AddNumberToObject(state, "bootId", boot_id_);
    cJSON_AddNumberToObject(state, "hostSessionId", host_session_id_);
    cJSON_AddNumberToObject(state, "nowMs", NowMs());
    cJSON_AddNumberToObject(state, "revision", revision_);
    cJSON_AddNumberToObject(state, "seq", event_seq_);
    cJSON_AddBoolToObject(state, "online", true);
    cJSON* screen = cJSON_AddObjectToObject(state, "screen");
    cJSON_AddNumberToObject(screen, "sceneId", scene_id_);
    cJSON_AddNumberToObject(screen, "width", 400);
    cJSON_AddNumberToObject(screen, "height", 300);
    cJSON_AddBoolToObject(screen, "bitmapActive", RlcdDisplay()->TemporaryBitmapActive());
    if (RlcdDisplay()->TemporaryBitmapActive()) {
        cJSON_AddNumberToObject(screen, "bitmapWidth", bitmap_width_);
        cJSON_AddNumberToObject(screen, "bitmapHeight", bitmap_height_);
        cJSON_AddNumberToObject(screen, "bitmapExpiresAtMs", bitmap_expires_at_ms_);
    }
    cJSON_AddBoolToObject(state, "mediaRequested", media_queued_);
    cJSON_AddNumberToObject(state, "droppedEvents", 0);
    return state;
}

void DeviceUseWifi::Reply(int id, cJSON* result, const char* error) {
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(response, "id", id);
    if (error) {
        cJSON* error_object = cJSON_AddObjectToObject(response, "error");
        cJSON_AddNumberToObject(error_object, "code", -32602);
        cJSON_AddStringToObject(error_object, "message", error);
        cJSON_Delete(result);
    } else {
        cJSON_AddItemToObject(response, "result", result ? result : cJSON_CreateObject());
    }
    if (!SendJson(response)) Disconnect();
    cJSON_Delete(response);
}

DeviceUseWifi::CallRecord* DeviceUseWifi::FindCall(const char* id) {
    if (!id) return nullptr;
    for (auto& call : calls_) if (call.id == id) return &call;
    return nullptr;
}

DeviceUseWifi::CallRecord* DeviceUseWifi::ReserveCall(const char* id, const char* parent_id,
        const char* tool, const char* scope, uint16_t generation, uint32_t argument_crc,
        uint32_t operation_id) {
    if (!id) return nullptr;
    for (size_t offset = 0; offset < calls_.size(); ++offset) {
        const size_t slot = (call_next_ + offset) % calls_.size();
        auto& call = calls_[slot];
        if (!call.id.empty() && strcmp(call.state, "accepted") == 0) continue;
        call.id = id;
        call.parent_id = parent_id ? parent_id : "";
        call.tool = tool;
        call.scope = scope;
        call.argument_crc = argument_crc;
        call.operation_id = operation_id;
        call.generation = generation;
        call.invocation_generation = generation;
        call.state = "accepted";
        call_next_ = (slot + 1) % calls_.size();
        return &call;
    }
    return nullptr;
}

cJSON* DeviceUseWifi::CallSnapshot(const CallRecord& call) const {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "callId", call.id.c_str());
    if (!call.parent_id.empty()) cJSON_AddStringToObject(result, "parentId", call.parent_id.c_str());
    cJSON_AddStringToObject(result, "tool", call.tool.c_str());
    cJSON_AddStringToObject(result, "scope", call.scope.c_str());
    cJSON_AddStringToObject(result, "state", call.state);
    cJSON_AddNumberToObject(result, "generation", call.generation);
    cJSON_AddNumberToObject(result, "operationId", call.operation_id);
    return result;
}

void DeviceUseWifi::FinishCall(uint32_t operation_id, bool success) {
    for (auto& call : calls_) {
        if (call.operation_id == operation_id && strcmp(call.state, "accepted") == 0) {
            call.state = success ? "completed" : "failed";
            return;
        }
    }
}

bool DeviceUseWifi::CompleteCall(CallRecord& call) {
    if (strcmp(call.state, "accepted") != 0) return false;
    call.state = "completed";
    ++revision_;
    Event(2, call.operation_id, 0);
    for (auto& child : calls_) {
        if (child.parent_id == call.id && child.scope == "CALL") CancelCall(child);
    }
    StateNotice();
    return true;
}

bool DeviceUseWifi::CancelCall(CallRecord& call) {
    if (strcmp(call.state, "accepted") != 0) return false;
    ++call.generation;
    call.state = "canceled";
    if (call.tool == "screen.render" || call.tool == "screen.bitmap") {
        screen_cancelled_.store(call.operation_id);
        if (call.tool == "screen.bitmap" && bitmap_operation_id_ == call.operation_id) {
            const uint32_t scene_id = scene_id_;
            Application::GetInstance().Schedule([scene_id]() {
                RlcdDisplay()->SetEmotion(Expression(scene_id));
            });
        }
    } else if (call.tool == "media.play" && media_operation_id_ == call.operation_id) {
        Application::GetInstance().StopMedia();
        media_queued_ = false;
        media_operation_id_ = 0;
    }
    ++revision_;
    Event(3, call.operation_id, call.generation);
    StateNotice();
    for (auto& child : calls_) {
        if (child.parent_id == call.id && child.scope == "CALL") CancelCall(child);
    }
    return true;
}

void DeviceUseWifi::HandleMessage(cJSON* message) {
    if (!authorized_) {
        const cJSON* id = cJSON_GetObjectItem(message, "id");
        const cJSON* result = cJSON_GetObjectItem(message, "result");
        const cJSON* accepted = cJSON_GetObjectItem(result, "accepted");
        const cJSON* wire_version = cJSON_GetObjectItem(result, "wireVersion");
        const cJSON* session_id = cJSON_GetObjectItem(result, "sessionId");
        const cJSON* epoch = cJSON_GetObjectItem(result, "connectionEpoch");
        if (!Integer(id, 1, 1) || !cJSON_IsTrue(accepted) ||
            !Integer(wire_version, 3, 3) || !Integer(session_id, 1, INT32_MAX) ||
            !Integer(epoch, 1, INT32_MAX) || epoch->valueint != connection_epoch_) {
            Disconnect();
            return;
        }
        host_session_id_ = session_id->valueint;
        authorized_ = true;
        Event(13, 0, 1);
        StateNotice();
        return;
    }
    const cJSON* id = cJSON_GetObjectItem(message, "id");
    const cJSON* method = cJSON_GetObjectItem(message, "method");
    if (!Integer(id, 2, INT32_MAX) || !cJSON_IsString(method)) return;
    int request_id = id->valueint;
    const cJSON* params = cJSON_GetObjectItem(message, "params");
    const char* name = method->valuestring;
    if (strcmp(name, "initialize") == 0) {
        Reply(request_id, ParseLiteral(R"({"name":"s3-rlcd-device-agent","protocolVersion":"device-use/0.3","wireVersion":3,"capabilities":{"tools":true,"resources":true,"subscribe":true,"prompts":true,"compactNotifications":true,"calls":true}})"));
    } else if (strcmp(name, "prompts/list") == 0) {
        Reply(request_id, ParseLiteral(R"({"prompts":[{"name":"device.capabilities","description":"RLCD actions and parameters"}]})"));
    } else if (strcmp(name, "prompts/get") == 0) {
        const cJSON* prompt = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(prompt) || strcmp(prompt->valuestring, "device.capabilities") != 0) {
            Reply(request_id, nullptr, "unknown prompt");
            return;
        }
        Reply(request_id, ParseLiteral(R"({"name":"device.capabilities","actions":[{"name":"screen.render","description":"Show an RLCD expression; cancels a temporary bitmap","parameters":[{"name":"sceneId","type":"integer","required":true,"minimum":1,"maximum":4}]},{"name":"screen.bitmap","description":"Show one centered monochrome frame temporarily; MSB-first, row-padded 1bpp, white bits on black background, base64 data","parameters":[{"name":"width","type":"integer","required":true,"minimum":1,"maximum":160},{"name":"height","type":"integer","required":true,"minimum":1,"maximum":120},{"name":"durationMs","type":"integer","required":true,"minimum":100,"maximum":60000},{"name":"dataBase64","type":"string","required":true,"maxLength":3200}]},{"name":"media.play","description":"Queue an Opus media URL; conversation may pause and resume it","parameters":[{"name":"url","type":"string","required":true,"maxLength":1024}]},{"name":"media.stop","description":"Stop queued or playing media","parameters":[]}],"expressions":[{"id":"happy","label":"开心","sceneId":1},{"id":"sad","label":"难过","sceneId":2},{"id":"surprised","label":"惊讶","sceneId":3},{"id":"neutral","label":"平静","sceneId":4}],"messages":[{"role":"user","content":{"type":"text","text":"Subscribe to device state, then call an advertised action and follow event feedback."}}]})"));
    } else if (strcmp(name, "tools/list") == 0) {
        Reply(request_id, ParseLiteral(R"({"tools":[{"name":"screen.render","parameters":[{"name":"sceneId","type":"integer","required":true,"minimum":1,"maximum":4}]},{"name":"screen.bitmap","parameters":[{"name":"width","type":"integer","required":true,"minimum":1,"maximum":160},{"name":"height","type":"integer","required":true,"minimum":1,"maximum":120},{"name":"durationMs","type":"integer","required":true,"minimum":100,"maximum":60000},{"name":"dataBase64","type":"string","required":true,"maxLength":3200}]},{"name":"media.play","parameters":[{"name":"url","type":"string","required":true,"maxLength":1024}]},{"name":"media.stop","parameters":[]}]})"));
    } else if (strcmp(name, "resources/read") == 0) {
        const cJSON* uri = cJSON_GetObjectItem(params, "uri");
        if (cJSON_IsString(uri) && strcmp(uri->valuestring, "device://rlcd/state") == 0) {
            Reply(request_id, StateSnapshot());
        } else {
            Reply(request_id, nullptr, "unknown resource");
        }
    } else if (strcmp(name, "resources/subscribe") == 0) {
        const cJSON* uri = cJSON_GetObjectItem(params, "uri");
        if (!cJSON_IsString(uri) || strcmp(uri->valuestring, "device://rlcd/state") != 0) {
            Reply(request_id, nullptr, "unknown resource");
            return;
        }
        subscribed_ = true;
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", true);
        cJSON_AddItemToObject(result, "snapshot", StateSnapshot());
        Reply(request_id, result);
    } else if (strcmp(name, "calls/create") == 0) {
        const cJSON* call_id = cJSON_GetObjectItem(params, "callId");
        const cJSON* parent_id = cJSON_GetObjectItem(params, "parentId");
        const cJSON* scope = cJSON_GetObjectItem(params, "scope");
        const cJSON* generation = cJSON_GetObjectItem(params, "generation");
        if (!cJSON_IsString(call_id) || !CallId(call_id->valuestring) ||
            !cJSON_IsString(scope) || strcmp(scope->valuestring, "CALL") != 0 ||
            !Integer(generation, 1, UINT16_MAX - 1) ||
            (parent_id && (!cJSON_IsString(parent_id) || !CallId(parent_id->valuestring)))) {
            Reply(request_id, nullptr, "invalid call metadata");
            return;
        }
        if (CallRecord* existing = FindCall(call_id->valuestring)) {
            if (existing->tool != "agent.turn" || existing->scope != "CALL" ||
                existing->parent_id != (parent_id ? parent_id->valuestring : "") ||
                existing->invocation_generation != generation->valueint) {
                Reply(request_id, nullptr, "callId reused with different operation");
            } else Reply(request_id, CallSnapshot(*existing));
            return;
        }
        if (parent_id) {
            CallRecord* parent = FindCall(parent_id->valuestring);
            if (!parent || strcmp(parent->state, "accepted") != 0) {
                Reply(request_id, nullptr, "parent call is not active");
                return;
            }
        }
        uint32_t operation_id = ++next_operation_id_;
        if (operation_id == 0) operation_id = ++next_operation_id_;
        CallRecord* call = ReserveCall(call_id->valuestring,
            parent_id ? parent_id->valuestring : nullptr, "agent.turn", "CALL",
            generation->valueint, 0, operation_id);
        if (!call) Reply(request_id, nullptr, "call table full");
        else {
            ++revision_;
            Reply(request_id, CallSnapshot(*call));
            Event(1, operation_id, 0);
            StateNotice();
        }
    } else if (strcmp(name, "calls/complete") == 0) {
        const cJSON* call_id = cJSON_GetObjectItem(params, "callId");
        const cJSON* generation = cJSON_GetObjectItem(params, "generation");
        CallRecord* call = cJSON_IsString(call_id) ? FindCall(call_id->valuestring) : nullptr;
        if (!call || call->tool != "agent.turn" ||
            !Integer(generation, 1, UINT16_MAX - 1) ||
            call->generation != generation->valueint) {
            Reply(request_id, nullptr, "unknown call or stale generation");
            return;
        }
        CompleteCall(*call);
        Reply(request_id, CallSnapshot(*call));
    } else if (strcmp(name, "calls/read") == 0) {
        const cJSON* call_id = cJSON_GetObjectItem(params, "callId");
        CallRecord* call = cJSON_IsString(call_id) ? FindCall(call_id->valuestring) : nullptr;
        if (!call) Reply(request_id, nullptr, "unknown call; reconcile device state");
        else Reply(request_id, CallSnapshot(*call));
    } else if (strcmp(name, "calls/active") == 0) {
        cJSON* result = cJSON_CreateObject();
        cJSON* calls = cJSON_AddArrayToObject(result, "calls");
        for (const auto& call : calls_) {
            if (!call.id.empty() && strcmp(call.state, "accepted") == 0)
                cJSON_AddItemToArray(calls, CallSnapshot(call));
        }
        cJSON_AddNumberToObject(result, "bootId", boot_id_);
        cJSON_AddNumberToObject(result, "revision", revision_);
        Reply(request_id, result);
    } else if (strcmp(name, "calls/cancel") == 0) {
        const cJSON* call_id = cJSON_GetObjectItem(params, "callId");
        const cJSON* generation = cJSON_GetObjectItem(params, "generation");
        CallRecord* call = cJSON_IsString(call_id) ? FindCall(call_id->valuestring) : nullptr;
        if (!call || !Integer(generation, 1, UINT16_MAX - 1) ||
            (call->generation != generation->valueint &&
             !(strcmp(call->state, "canceled") == 0 &&
               call->generation == generation->valueint + 1))) {
            Reply(request_id, nullptr, "unknown call or stale generation");
            return;
        }
        CancelCall(*call);
        Reply(request_id, CallSnapshot(*call));
    } else if (strcmp(name, "tools/call") == 0) {
        const cJSON* tool = cJSON_GetObjectItem(params, "name");
        const cJSON* args = cJSON_GetObjectItem(params, "arguments");
        if (!cJSON_IsString(tool)) {
            Reply(request_id, nullptr, "missing tool name");
            return;
        }
        const cJSON* metadata = cJSON_GetObjectItem(params, "call");
        const cJSON* call_id = cJSON_GetObjectItem(metadata, "callId");
        const cJSON* parent_id = cJSON_GetObjectItem(metadata, "parentId");
        const cJSON* scope = cJSON_GetObjectItem(metadata, "scope");
        const cJSON* generation = cJSON_GetObjectItem(metadata, "generation");
        if (!cJSON_IsObject(metadata) || !cJSON_IsString(call_id) ||
            !CallId(call_id->valuestring) || !cJSON_IsString(scope) ||
            (strcmp(scope->valuestring, "CALL") != 0 &&
             strcmp(scope->valuestring, "SESSION") != 0 &&
             strcmp(scope->valuestring, "DEVICE") != 0) ||
            !Integer(generation, 1, UINT16_MAX - 1) ||
            (parent_id && (!cJSON_IsString(parent_id) || !CallId(parent_id->valuestring))) ||
            !cJSON_IsObject(args)) {
            Reply(request_id, nullptr, "invalid call metadata");
            return;
        }
        const uint32_t argument_crc = ArgumentsCrc(tool->valuestring, args);
        if (CallRecord* previous = FindCall(call_id->valuestring)) {
            if (previous->tool != tool->valuestring || previous->scope != scope->valuestring ||
                previous->parent_id != (parent_id ? parent_id->valuestring : "") ||
                previous->argument_crc != argument_crc ||
                previous->invocation_generation != generation->valueint) {
                Reply(request_id, nullptr, "callId reused with different operation");
                return;
            }
            cJSON* result = CallSnapshot(*previous);
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddBoolToObject(result, "duplicate", true);
            Reply(request_id, result);
            return;
        }
        if (parent_id) {
            CallRecord* parent = FindCall(parent_id->valuestring);
            if (!parent || strcmp(parent->state, "accepted") != 0) {
                Reply(request_id, nullptr, "parent call is not active");
                return;
            }
        }
        if (((strcmp(tool->valuestring, "media.play") == 0 ||
              strcmp(tool->valuestring, "screen.bitmap") == 0) &&
             strcmp(scope->valuestring, "SESSION") != 0) ||
            (strcmp(tool->valuestring, "media.play") != 0 &&
             strcmp(tool->valuestring, "screen.bitmap") != 0 &&
             strcmp(scope->valuestring, "CALL") != 0)) {
            Reply(request_id, nullptr, "invalid scope for tool");
            return;
        }
        uint32_t operation_id = ++next_operation_id_;
        if (operation_id == 0) operation_id = ++next_operation_id_;
        auto reserve = [&]() -> CallRecord* {
            return ReserveCall(call_id->valuestring,
                parent_id ? parent_id->valuestring : nullptr, tool->valuestring,
                scope->valuestring, generation->valueint, argument_crc, operation_id);
        };
        if (strcmp(tool->valuestring, "screen.render") == 0) {
            const cJSON* scene = cJSON_GetObjectItem(args, "sceneId");
            if (!Integer(scene, 1, 4) || screen_pending_.load() != 0) {
                Reply(request_id, nullptr, "invalid scene or screen busy");
                return;
            }
            CallRecord* call = reserve();
            if (!call) { Reply(request_id, nullptr, "call table full"); return; }
            screen_pending_.store(operation_id);
            const uint32_t scene_id = scene->valueint;
            Application::GetInstance().Schedule([this, operation_id, scene_id]() {
                if (screen_cancelled_.load() == operation_id) {
                    screen_done_scene_.store(UINT32_MAX);
                } else {
                    Board::GetInstance().GetDisplay()->SetEmotion(Expression(scene_id));
                    screen_done_scene_.store(scene_id);
                }
                screen_done_.store(operation_id);
            });
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
            cJSON_AddItemToObject(result, "call", CallSnapshot(*call));
            Reply(request_id, result);
            Event(1, operation_id, 0);
        } else if (strcmp(tool->valuestring, "screen.bitmap") == 0) {
            const cJSON* width = cJSON_GetObjectItem(args, "width");
            const cJSON* height = cJSON_GetObjectItem(args, "height");
            const cJSON* duration = cJSON_GetObjectItem(args, "durationMs");
            const cJSON* data = cJSON_GetObjectItem(args, "dataBase64");
            if (!Integer(width, 1, 160) || !Integer(height, 1, 120) ||
                !Integer(duration, 100, 60000) || !cJSON_IsString(data) ||
                screen_pending_.load() != 0) {
                Reply(request_id, nullptr, "invalid bitmap or screen busy");
                return;
            }
            std::vector<uint8_t> bits;
            const size_t expected = ((width->valueint + 7) / 8) * height->valueint;
            if (!DecodeBitmap(data->valuestring, expected, bits)) {
                Reply(request_id, nullptr, "invalid bitmap data");
                return;
            }
            CallRecord* call = reserve();
            if (!call) { Reply(request_id, nullptr, "call table full"); return; }
            screen_pending_.store(operation_id);
            const uint16_t bitmap_width = width->valueint;
            const uint16_t bitmap_height = height->valueint;
            const uint32_t duration_ms = duration->valueint;
            Application::GetInstance().Schedule(
                [this, operation_id, bitmap_width, bitmap_height, duration_ms,
                 bits = std::move(bits)]() {
                    bool shown = screen_cancelled_.load() != operation_id &&
                        RlcdDisplay()->ShowTemporaryBitmap(bits, bitmap_width,
                                                           bitmap_height, duration_ms);
                    screen_done_scene_.store(shown ? 0 : UINT32_MAX);
                    screen_done_.store(operation_id);
                });
            bitmap_width_ = bitmap_width;
            bitmap_height_ = bitmap_height;
            bitmap_duration_ms_ = duration_ms;
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
            cJSON_AddItemToObject(result, "call", CallSnapshot(*call));
            Reply(request_id, result);
            Event(1, operation_id, 0);
        } else if (strcmp(tool->valuestring, "media.play") == 0) {
            const cJSON* url = cJSON_GetObjectItem(args, "url");
            if (!cJSON_IsString(url) || strlen(url->valuestring) > 1024 ||
                (strncmp(url->valuestring, "http://", 7) != 0 &&
                 strncmp(url->valuestring, "https://", 8) != 0) ||
                !strstr(url->valuestring, "/media/opus/")) {
                Reply(request_id, nullptr, "invalid Opus media URL");
                return;
            }
            CallRecord* call = reserve();
            if (!call) { Reply(request_id, nullptr, "call table full"); return; }
            if (media_operation_id_ != 0) FinishCall(media_operation_id_, true);
            if (!Application::GetInstance().QueueMedia(url->valuestring)) {
                call->state = "failed";
                Reply(request_id, nullptr, "media queue failed");
                return;
            }
            media_operation_id_ = operation_id;
            media_queued_ = true;
            ++revision_;
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
            cJSON_AddItemToObject(result, "call", CallSnapshot(*call));
            Reply(request_id, result);
            Event(1, operation_id, 0);
            Event(2, operation_id, 0);
            StateNotice();
        } else if (strcmp(tool->valuestring, "media.stop") == 0) {
            CallRecord* call = reserve();
            if (!call) { Reply(request_id, nullptr, "call table full"); return; }
            Application::GetInstance().StopMedia();
            if (media_operation_id_ != 0) FinishCall(media_operation_id_, true);
            media_operation_id_ = 0;
            call->state = "completed";
            media_queued_ = false;
            ++revision_;
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "accepted", true);
            cJSON_AddNumberToObject(result, "operationId", operation_id);
            cJSON_AddItemToObject(result, "call", CallSnapshot(*call));
            Reply(request_id, result);
            Event(1, operation_id, 0);
            Event(2, operation_id, 0);
            StateNotice();
        } else {
            Reply(request_id, nullptr, "unknown tool");
        }
    } else {
        Reply(request_id, nullptr, "unknown method");
    }
}

void DeviceUseWifi::ProcessBytes(const uint8_t* data, size_t length) {
    receive_.insert(receive_.end(), data, data + length);
    while (receive_.size() >= 10 && socket_ >= 0) {
        if (receive_[0] != kMagic || receive_[1] != kJsonFrame) {
            Disconnect();
            return;
        }
        uint32_t length = Read32(receive_.data() + 2);
        if (length == 0 || length > kMaxRequest) {
            Disconnect();
            return;
        }
        if (receive_.size() < length + 10) return;
        uint32_t expected = Read32(receive_.data() + 6);
        const char* payload = reinterpret_cast<const char*>(receive_.data() + 10);
        if (Crc32(reinterpret_cast<const uint8_t*>(payload), length) != expected) {
            Disconnect();
            return;
        }
        cJSON* message = cJSON_ParseWithLength(payload, length);
        receive_.erase(receive_.begin(), receive_.begin() + length + 10);
        if (!message) {
            Disconnect();
            return;
        }
        HandleMessage(message);
        cJSON_Delete(message);
    }
    if (receive_.size() > kMaxRequest + 10) Disconnect();
}

void DeviceUseWifi::Run() {
#if defined(RLCD_DEVICE_USE_HOST) && defined(RLCD_DEVICE_USE_TOKEN)
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (true) {
        if (!WifiManager::GetInstance().IsConnected()) {
            Disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!Connect()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(kTag, "Device Use Wi-Fi connected");
        last_heartbeat_ms_ = NowMs();
        uint32_t handshake_started = NowMs();
        while (socket_ >= 0 && WifiManager::GetInstance().IsConnected()) {
            uint32_t now = NowMs();
            if (!authorized_ && now - handshake_started > 5000) break;
            if (authorized_ && now - last_heartbeat_ms_ >= 1000) {
                Heartbeat();
                last_heartbeat_ms_ = now;
            }
            uint32_t done = authorized_ ? screen_done_.exchange(0) : 0;
            if (done != 0) {
                const uint32_t scene = screen_done_scene_.load();
                CallRecord* completed_call = nullptr;
                for (auto& call : calls_) {
                    if (call.operation_id == done) { completed_call = &call; break; }
                }
                if (completed_call && strcmp(completed_call->state, "canceled") == 0) {
                    screen_pending_.store(0);
                    continue;
                }
                ++revision_;
                if (scene == UINT32_MAX) {
                    FinishCall(done, false);
                    Event(3, done, 0);
                } else {
                    if (scene != 0) scene_id_ = scene;
                    if (scene == 0) {
                        if (bitmap_was_active_) {
                            FinishCall(bitmap_operation_id_, true);
                            Event(14, bitmap_operation_id_, 1);
                        }
                        bitmap_operation_id_ = done;
                        bitmap_was_active_ = true;
                        bitmap_expires_at_ms_ = NowMs() + bitmap_duration_ms_;
                    }
                    if (scene != 0) FinishCall(done, true);
                    Event(2, done, scene);
                    Event(4, done, scene);
                }
                StateNotice();
                screen_pending_.store(0);
            }
            if (bitmap_was_active_ && !RlcdDisplay()->TemporaryBitmapActive()) {
                bitmap_was_active_ = false;
                FinishCall(bitmap_operation_id_, true);
                ++revision_;
                Event(14, bitmap_operation_id_, 0);
                StateNotice();
            }
            if (authorized_ && media_queued_ && media_operation_id_ != 0) {
                const uint8_t media_status = Application::GetInstance().MediaRequestStatus();
                if (media_status == 2 || media_status == 3) {
                    const uint32_t completed = media_operation_id_;
                    media_operation_id_ = 0;
                    media_queued_ = false;
                    FinishCall(completed, media_status == 2);
                    ++revision_;
                    Event(media_status == 2 ? 12 : 3, completed, 0);
                    StateNotice();
                }
            }
            uint8_t buffer[512];
            int received = recv(socket_, buffer, sizeof(buffer), 0);
            if (received > 0) {
                ProcessBytes(buffer, received);
            } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                break;
            }
        }
        Disconnect();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
#endif
    vTaskDelete(nullptr);
}
