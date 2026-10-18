// p2p_libp2p_c.h - 完整 C ABI 接口
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
//  不透明句柄
// ============================================================
typedef struct p2p_handle_s p2p_handle;

// ============================================================
//  日志级别（与 C++ P2PLogLevel 对应）
// ============================================================
enum {
    P2P_LOG_DEBUG   = 0,
    P2P_LOG_INFO    = 1,
    P2P_LOG_WARNING = 2,
    P2P_LOG_ERROR   = 3
};

// ============================================================
//  回调类型
// ============================================================
typedef void (*p2p_log_cb_t)   (int level, const char* msg);
typedef void (*p2p_msg_cb_t)   (const char* peer, const char* data);
typedef void (*p2p_peer_cb_t)  (const char* peer);
typedef void (*p2p_stream_cb_t)(unsigned long long sid, const char* protocol, const char* peer);

// ============================================================
//  日志
// ============================================================
void p2p_c_set_log_callback(p2p_log_cb_t cb);

// ============================================================
//  生命周期
// ============================================================
p2p_handle* p2p_c_init(int port, const char* key_path);
void        p2p_c_shutdown(p2p_handle* h);
int         p2p_c_is_running(p2p_handle* h);

// ============================================================
//  节点信息（返回的字符串需 p2p_c_free_string 释放）
// ============================================================
char* p2p_c_get_peer_id(p2p_handle* h);
char* p2p_c_get_multiaddr(p2p_handle* h);
void  p2p_c_free_string(char* s);
int   p2p_c_peer_count(p2p_handle* h);
int   p2p_c_is_connected(p2p_handle* h, const char* peer_id);

// ============================================================
//  连接
// ============================================================
int p2p_c_connect(p2p_handle* h, const char* multiaddr);
int p2p_c_disconnect(p2p_handle* h, const char* peer_id);

// ============================================================
//  消息
// ============================================================
int p2p_c_send(p2p_handle* h, const char* peer_id, const char* data, int len);
int p2p_c_broadcast(p2p_handle* h, const char* data, int len);

// ============================================================
//  Stream
// ============================================================
unsigned long long p2p_c_open_stream(p2p_handle* h, const char* peer_id, const char* protocol);
int  p2p_c_stream_write(p2p_handle* h, unsigned long long sid, const char* buf, int len);
int  p2p_c_stream_read(p2p_handle* h, unsigned long long sid, char* buf, int len, int timeout_ms);
void p2p_c_close_stream(p2p_handle* h, unsigned long long sid);

// ============================================================
//  回调
// ============================================================
void p2p_c_set_message_callback(p2p_handle* h, p2p_msg_cb_t cb);
void p2p_c_set_connect_callback(p2p_handle* h, p2p_peer_cb_t cb);
void p2p_c_set_disconnect_callback(p2p_handle* h, p2p_peer_cb_t cb);
void p2p_c_set_subscribe_callback(p2p_handle* h, p2p_msg_cb_t cb);
void p2p_c_set_stream_handler(p2p_handle* h, const char* protocol, p2p_stream_cb_t cb);

// ============================================================
//  PubSub
// ============================================================
int p2p_c_subscribe(p2p_handle* h, const char* topic);
int p2p_c_unsubscribe(p2p_handle* h, const char* topic);
int p2p_c_publish(p2p_handle* h, const char* topic, const char* data, int len);

// ============================================================
//  中继
// ============================================================
int p2p_c_enable_relay(p2p_handle* h, int enable);
int p2p_c_connect_via_relay(p2p_handle* h, const char* peer_id, const char* relay_addr);

// ============================================================
//  版本
// ============================================================
const char* p2p_c_version();

#ifdef __cplusplus
}
#endif