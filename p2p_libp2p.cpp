// p2p_libp2p.cpp
#include "p2p_libp2p.h"
#include <iostream>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <map>

// ---- 跨平台动态库 ----
#ifdef _WIN32
    #include <windows.h>
    using lib_handle_t = HMODULE;
    static lib_handle_t dl_open(const char* name) { return LoadLibraryA(name); }
    static void* dl_sym(lib_handle_t h, const char* name) { return (void*)GetProcAddress(h, name); }
    static void dl_close(lib_handle_t h) { FreeLibrary(h); }
    static const char* LIB_NAMES[] = {"libp2p_wrapper.dll", "./libp2p_wrapper.dll", nullptr};
#elif defined(__APPLE__)
    #include <dlfcn.h>
    using lib_handle_t = void*;
    static lib_handle_t dl_open(const char* name) { return dlopen(name, RTLD_NOW); }
    static void* dl_sym(lib_handle_t h, const char* name) { return dlsym(h, name); }
    static void dl_close(lib_handle_t h) { dlclose(h); }
    static const char* LIB_NAMES[] = {"libp2p_wrapper.dylib", "./libp2p_wrapper.dylib", nullptr};
#else
    #include <dlfcn.h>
    using lib_handle_t = void*;
    static lib_handle_t dl_open(const char* name) { return dlopen(name, RTLD_NOW); }
    static void* dl_sym(lib_handle_t h, const char* name) { return dlsym(h, name); }
    static void dl_close(lib_handle_t h) { dlclose(h); }
    static const char* LIB_NAMES[] = {"libp2p_wrapper.so", "./libp2p_wrapper.so", nullptr};
#endif

// ---- C 回调的真实类型 ----
using c_msg_cb_t    = void(*)(char*, char*);
using c_peer_cb_t   = void(*)(char*);
using c_stream_cb_t = void(*)(unsigned long long, char*, char*);

// ---- 函数指针 ----
using fn_start              = void*(*)(int, const char*);  
using fn_stop               = void(*)(void*);
using fn_version            = const char*(*)();
using fn_free_string        = void(*)(char*);
using fn_get_peer_id        = const char*(*)(void*);
using fn_get_multiaddr      = const char*(*)(void*);
using fn_peer_count         = int(*)(void*);
using fn_is_connected       = int(*)(void*, const char*);
using fn_connect            = int(*)(void*, const char*);
using fn_disconnect         = int(*)(void*, const char*);
using fn_send               = int(*)(void*, const char*, const char*, int);
using fn_broadcast          = int(*)(void*, const char*, int);
using fn_open_stream        = unsigned long long(*)(void*, const char*, const char*);
using fn_stream_write       = int(*)(void*, unsigned long long, const char*, int);
using fn_stream_read        = int(*)(void*, unsigned long long, char*, int, int);
using fn_close_stream       = void(*)(void*, unsigned long long);
using fn_set_stream_handler = void(*)(void*, const char*, c_stream_cb_t);
using fn_subscribe          = int(*)(void*, const char*);
using fn_unsubscribe        = int(*)(void*, const char*);
using fn_publish            = int(*)(void*, const char*, const char*, int);
using fn_enable_relay       = int(*)(void*, int);
using fn_connect_relay      = int(*)(void*, const char*, const char*);
using fn_set_msg_cb         = void(*)(c_msg_cb_t);
using fn_set_peer_cb        = void(*)(c_peer_cb_t);

// ---- 全局 ----
static lib_handle_t g_lib = nullptr;
static void*        g_ctx = nullptr;
static std::atomic<bool> g_running{false};
static std::mutex   g_mutex;

// ---- 全局回调（只缓存，init 时应用）----
static MessageCallback    g_msg_cb;
static ConnectCallback    g_conn_cb;
static DisconnectCallback g_disc_cb;
static SubscribeCallback  g_sub_cb;
static std::map<std::string, StreamHandler> g_stream_handlers;
static std::mutex         g_stream_handlers_mutex;
static P2PLogCallback     g_log_cb = nullptr;

// ★ 通用缓存列表
using DeferredAction = std::function<void()>;
static std::vector<DeferredAction> g_deferred_actions;
static std::mutex g_deferred_mutex;

// ★ 统一的注册辅助
static void defer_or_run(DeferredAction action) {
    std::lock_guard<std::mutex> lock(g_deferred_mutex);
    if (g_ctx && g_lib) {
        action();  // 已 init，立即执行
    } else {
        g_deferred_actions.push_back(std::move(action));  // 缓存
    }
}

// ★ init 成功后调用
static void apply_deferred() {
    std::lock_guard<std::mutex> lock(g_deferred_mutex);
    for (auto& action : g_deferred_actions) {
        action();
    }
    g_deferred_actions.clear();
}

// ---- C 回调桥接 ----
extern "C" void realm_msg_cb_bridge(char* peer, char* data) {
    if (g_msg_cb) g_msg_cb(peer ? peer : "", data ? data : "");
    if (peer) free(peer);   // ★
    if (data) free(data);   // ★
}
extern "C" void realm_conn_cb_bridge(char* peer) {
    if (g_conn_cb) g_conn_cb(peer ? peer : "");
    if (peer) free(peer);   // ★
}
extern "C" void realm_disc_cb_bridge(char* peer) {
    if (g_disc_cb) g_disc_cb(peer ? peer : "");
    if (peer) free(peer);   // ★
}
extern "C" void realm_sub_cb_bridge(char* peer, char* data) {
    if (g_sub_cb) g_sub_cb(peer ? peer : "", data ? data : "");
    if (peer) free(peer);   // ★
    if (data) free(data);   // ★
}
extern "C" void realm_stream_cb_bridge(unsigned long long id, char* proto, char* peer) {
    std::string p = proto ? proto : "";
    StreamHandler handler;
    {
        std::lock_guard<std::mutex> lock(g_stream_handlers_mutex);
        auto it = g_stream_handlers.find(p);
        if (it != g_stream_handlers.end()) {
            handler = it->second;    // 拷贝出来，锁外执行
        }
    }
    if (handler) handler((uint64_t)id, peer ? peer : "");
    if (proto) free(proto);
    if (peer) free(peer);
}

// ---- 日志 ----
static void p2p_log(P2PLogLevel level, const std::string& msg) {
    if (g_log_cb) {
        g_log_cb(level, msg);
        return;
    }
    const char* prefix = "INFO";
    switch (level) {
        case P2PLogLevel::DEBUG:   prefix = "DEBUG"; break;
        case P2PLogLevel::INFO:    prefix = "INFO";  break;
        case P2PLogLevel::WARNING: prefix = "WARN";  break;
        case P2PLogLevel::ERR:   prefix = "ERROR"; break;
    }
    std::cerr << "[P2P][" << prefix << "] " << msg << std::endl;
}

#define GET_FN(name, type) (type)dl_sym(g_lib, #name)

// ---- 日志 ----
void p2p_set_log_callback(P2PLogCallback cb) {
    g_log_cb = cb;
}

// ---- 消息 ----
void p2p_set_message_callback(MessageCallback cb) {
    g_msg_cb = cb;
    defer_or_run([]() {
        auto fn = GET_FN(libp2p_set_message_callback, fn_set_msg_cb);
        if (fn) fn(realm_msg_cb_bridge);
    });
}

// ---- 连接 ----
void p2p_set_connect_callback(ConnectCallback cb) {
    g_conn_cb = cb;
    defer_or_run([]() {
        auto fn = GET_FN(libp2p_set_connect_callback, fn_set_peer_cb);
        if (fn) fn(realm_conn_cb_bridge);
    });
}

// ---- 断开 ----
void p2p_set_disconnect_callback(DisconnectCallback cb) {
    g_disc_cb = cb;
    defer_or_run([]() {
        auto fn = GET_FN(libp2p_set_disconnect_callback, fn_set_peer_cb);
        if (fn) fn(realm_disc_cb_bridge);
    });
}

// ---- 订阅 ----
void p2p_set_subscribe_callback(SubscribeCallback cb) {
    g_sub_cb = cb;
    defer_or_run([]() {
        auto fn = GET_FN(libp2p_set_subscribe_callback, fn_set_msg_cb);
        if (fn) fn(realm_sub_cb_bridge);
    });
}

void p2p_set_stream_handler(const std::string& protocol, StreamHandler handler) {
    {
        std::lock_guard<std::mutex> lock(g_stream_handlers_mutex);
        g_stream_handlers[protocol] = handler;
    }
    defer_or_run([protocol]() {
        auto fn = GET_FN(libp2p_set_stream_handler, fn_set_stream_handler);
        if (fn) fn(g_ctx, protocol.c_str(), realm_stream_cb_bridge);
    });
}

// ---- 加载库 ----
static bool load_lib() {
    if (g_lib) return true;
    for (int i = 0; LIB_NAMES[i]; ++i) {
        g_lib = dl_open(LIB_NAMES[i]);
        if (g_lib) return true;
    }
    p2p_log(P2PLogLevel::ERR, "无法加载 libp2p_wrapper");
    return false;
}

// ============================================================
//  生命周期
// ============================================================
bool p2p_init(uint16_t port, const std::string& key_path) {
    std::string log_msg;
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_running.load()) return true;
        if (!load_lib()) {
            log_msg = "无法加载 libp2p_wrapper";
        } else {
            auto start = GET_FN(libp2p_start, fn_start);
            if (!start) {
                log_msg = "缺少 libp2p_start";
            } else {
                std::string kp = key_path.empty() ? "./.libp2p_key" : key_path;
                g_ctx = start((int)port, kp.c_str());
                if (!g_ctx) {
                    log_msg = "启动失败";
                } else {
                    g_running.store(true);
                    apply_deferred();
                    ok = true;
                }
            }
        }
    }   // ← 先释放锁

    // ★ 锁外发日志
    if (ok) {
        p2p_log(P2PLogLevel::INFO, "已启动，Peer ID: " + p2p_get_peer_id());
    } else if (!log_msg.empty()) {
        p2p_log(P2PLogLevel::ERR, log_msg);
    }
    return ok;
}

void p2p_shutdown() {
    if (!g_running.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto stop = GET_FN(libp2p_stop, fn_stop);
        if (stop && g_ctx) stop(g_ctx);
        g_ctx = nullptr;
        g_running.store(false);
    }
    // 锁外
    p2p_log(P2PLogLevel::INFO, "已关闭"); 
}

bool p2p_is_running() { return g_running.load(); }

// ============================================================
//  节点信息
// ============================================================
std::string p2p_get_peer_id() {
    if (!g_ctx || !g_lib) return "";
    auto fn = GET_FN(libp2p_get_peer_id, fn_get_peer_id);
    auto free_fn = GET_FN(libp2p_free_string, fn_free_string);
    if (!fn) return "";
    const char* s = fn(g_ctx);
    if (!s) return "";
    std::string result(s);
    if (free_fn) free_fn((char*)s);
    return result;
}

std::string p2p_get_multiaddr() {
    if (!g_ctx || !g_lib) return "";
    auto fn = GET_FN(libp2p_get_multiaddr, fn_get_multiaddr);
    auto free_fn = GET_FN(libp2p_free_string, fn_free_string);
    if (!fn) return "";
    const char* s = fn(g_ctx);
    if (!s) return "";
    std::string result(s);
    if (free_fn) free_fn((char*)s);
    return result;
}

size_t p2p_peer_count() {
    if (!g_ctx || !g_lib) return 0;
    auto fn = GET_FN(libp2p_peer_count, fn_peer_count);
    if (!fn) return 0;
    return (size_t)fn(g_ctx);
}

bool p2p_is_connected(const std::string& peer_id) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_is_connected, fn_is_connected);
    if (!fn) return false;
    return fn(g_ctx, peer_id.c_str()) == 1;
}

// ============================================================
//  连接
// ============================================================
bool p2p_connect(const std::string& multiaddr) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_connect, fn_connect);
    if (!fn) return false;
    return fn(g_ctx, multiaddr.c_str()) == 0;
}

bool p2p_disconnect(const std::string& peer_id) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_disconnect, fn_disconnect);
    if (!fn) return false;
    return fn(g_ctx, peer_id.c_str()) == 0;
}

// ============================================================
//  消息
// ============================================================
bool p2p_send(const std::string& peer_id, const std::string& data) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_send, fn_send);
    if (!fn) return false;
    return fn(g_ctx, peer_id.c_str(), data.c_str(), (int)data.size()) == 0;
}

bool p2p_broadcast(const std::string& data) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_broadcast, fn_broadcast);
    if (!fn) return false;
    return fn(g_ctx, data.c_str(), (int)data.size()) == 0;
}

// ============================================================
//  Stream
// ============================================================
uint64_t p2p_open_stream(const std::string& peer_id, const std::string& protocol) {
    if (!g_ctx || !g_lib) return 0;
    auto fn = GET_FN(libp2p_open_stream, fn_open_stream);
    if (!fn) return 0;
    return (uint64_t)fn(g_ctx, peer_id.c_str(), protocol.c_str());
}

int p2p_stream_write(uint64_t stream_id, const char* buf, int len) {
    if (!g_ctx || !g_lib) return -1;
    auto fn = GET_FN(libp2p_stream_write, fn_stream_write);
    if (!fn) return -1;
    return fn(g_ctx, (unsigned long long)stream_id, buf, len);
}

int p2p_stream_read(uint64_t stream_id, char* buf, int len, int timeout_ms) {
    if (!g_ctx || !g_lib) return -1;
    auto fn = GET_FN(libp2p_stream_read, fn_stream_read);
    if (!fn) return -1;
    return fn(g_ctx, (unsigned long long)stream_id, buf, len, timeout_ms);
}

void p2p_close_stream(uint64_t stream_id) {
    if (!g_ctx || !g_lib) return;
    auto fn = GET_FN(libp2p_close_stream, fn_close_stream);
    if (fn) fn(g_ctx, (unsigned long long)stream_id);
}


// ============================================================
//  PubSub
// ============================================================
bool p2p_subscribe(const std::string& topic) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_subscribe, fn_subscribe);
    if (!fn) return false;
    return fn(g_ctx, topic.c_str()) == 0;
}

bool p2p_unsubscribe(const std::string& topic) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_unsubscribe, fn_unsubscribe);
    if (!fn) return false;
    return fn(g_ctx, topic.c_str()) == 0;
}

bool p2p_publish(const std::string& topic, const std::string& data) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_publish, fn_publish);
    if (!fn) return false;
    return fn(g_ctx, topic.c_str(), data.c_str(), (int)data.size()) == 0;
}

// ============================================================
//  中继
// ============================================================
bool p2p_enable_relay(bool enable) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_enable_relay, fn_enable_relay);
    if (!fn) return false;
    return fn(g_ctx, enable ? 1 : 0) == 0;
}

bool p2p_connect_via_relay(const std::string& peer_id, const std::string& relay_addr) {
    if (!g_ctx || !g_lib) return false;
    auto fn = GET_FN(libp2p_connect_via_relay, fn_connect_relay);
    if (!fn) return false;
    return fn(g_ctx, peer_id.c_str(), relay_addr.c_str()) == 0;
}

// ============================================================
//  工具
// ============================================================
std::string p2p_version() {
    if (!g_lib) return "unknown";
    auto fn = GET_FN(libp2p_version, fn_version);
    auto free_fn = GET_FN(libp2p_free_string, fn_free_string);
    if (!fn) return "unknown";
    const char* s = fn();
    if (!s) return "unknown";
    std::string result(s);
    if (free_fn) free_fn((char*)s);
    return result;
}
