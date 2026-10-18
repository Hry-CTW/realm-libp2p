// p2p_libp2p.h
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// 日志级别
enum class P2PLogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERR
};

// 日志回调
using P2PLogCallback = std::function<void(P2PLogLevel level, const std::string& msg)>;
// 注册日志回调（不注册则默认输出到 stderr）
void p2p_set_log_callback(P2PLogCallback cb);

// ---- 回调类型 ----
using MessageCallback    = std::function<void(const std::string& peer_id, const std::string& data)>;
using ConnectCallback    = std::function<void(const std::string& peer_id)>;
using DisconnectCallback = std::function<void(const std::string& peer_id)>;
using SubscribeCallback  = std::function<void(const std::string& peer_id, const std::string& data)>;
using StreamHandler      = std::function<void(uint64_t stream_id, const std::string& peer_id)>;

// ---- 生命周期 ----
bool p2p_init(uint16_t port = 0, const std::string& key_path = "");
void p2p_shutdown();
bool p2p_is_running();

// ---- 节点信息 ----
std::string p2p_get_peer_id();
std::string p2p_get_multiaddr();
size_t      p2p_peer_count();
bool        p2p_is_connected(const std::string& peer_id);

// ---- 连接 ----
bool p2p_connect(const std::string& multiaddr);
bool p2p_disconnect(const std::string& peer_id);

// ---- 消息 ----
bool p2p_send(const std::string& peer_id, const std::string& data);
bool p2p_broadcast(const std::string& data);

// ---- Stream ----
uint64_t p2p_open_stream(const std::string& peer_id, const std::string& protocol);
int      p2p_stream_write(uint64_t stream_id, const char* buf, int len);
int      p2p_stream_read(uint64_t stream_id, char* buf, int len, int timeout_ms);
void     p2p_close_stream(uint64_t stream_id);
void     p2p_set_stream_handler(const std::string& protocol, StreamHandler handler);

// ---- PubSub ----
bool p2p_subscribe(const std::string& topic);
bool p2p_unsubscribe(const std::string& topic);
bool p2p_publish(const std::string& topic, const std::string& data);

// ---- 中继 ----
bool p2p_enable_relay(bool enable);
bool p2p_connect_via_relay(const std::string& peer_id, const std::string& relay_addr);

// ---- 回调 ----
void p2p_set_message_callback(MessageCallback cb);
void p2p_set_connect_callback(ConnectCallback cb);
void p2p_set_disconnect_callback(DisconnectCallback cb);
void p2p_set_subscribe_callback(SubscribeCallback cb);

// ---- 工具 ----
std::string p2p_version();