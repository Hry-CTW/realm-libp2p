// p2p_libp2p_c.cpp - C ABI 完整实现
#include "p2p_libp2p_c.h"
#include "p2p_libp2p.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>

// ============================================================
//  句柄定义
// ============================================================
struct p2p_handle_s {
    int magic;  // 校验用
    int port;
    std::string key_path;
};

static const int P2P_HANDLE_MAGIC = 0x50415032;  // "PAP2"

// ============================================================
//  C 回调函数指针（全局）
// ============================================================
static p2p_log_cb_t    g_c_log_cb  = nullptr;
static p2p_peer_cb_t   g_c_conn_cb = nullptr;
static p2p_peer_cb_t   g_c_disc_cb = nullptr;
static p2p_msg_cb_t    g_c_msg_cb  = nullptr;
static p2p_msg_cb_t    g_c_sub_cb  = nullptr;

// 每个协议对应一个 C 回调
static std::map<std::string, p2p_stream_cb_t> g_c_stream_cbs;
static std::mutex g_c_stream_cbs_mutex;

// 是否已经注册过 C++ 桥接
static bool g_bridge_installed = false;
static std::mutex g_bridge_mutex;

static std::mutex g_handles_mutex;
static std::set<p2p_handle*> g_handles;

// ============================================================
//  工具
// ============================================================
static char* dup_str(const std::string& s) {
    char* r = (char*)std::malloc(s.size() + 1);
    if (r) std::memcpy(r, s.c_str(), s.size() + 1);
    return r;
}

static bool valid_handle(p2p_handle* h) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    return h && g_handles.count(h) > 0;
}


// ============================================================
//  安装 C++ → C 的桥接（只装一次）
// ============================================================
static void install_bridges() {
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (g_bridge_installed) return;

    p2p_set_log_callback([](P2PLogLevel lv, const std::string& m) {
        if (g_c_log_cb) g_c_log_cb((int)lv, m.c_str());
    });
    p2p_set_connect_callback([](const std::string& p) {
        if (g_c_conn_cb) g_c_conn_cb(p.c_str());
    });
    p2p_set_disconnect_callback([](const std::string& p) {
        if (g_c_disc_cb) g_c_disc_cb(p.c_str());
    });
    p2p_set_message_callback([](const std::string& p, const std::string& d) {
        if (g_c_msg_cb) g_c_msg_cb(p.c_str(), d.c_str());
    });
    p2p_set_subscribe_callback([](const std::string& p, const std::string& d) {
        if (g_c_sub_cb) g_c_sub_cb(p.c_str(), d.c_str());
    });

    g_bridge_installed = true;
}

// ============================================================
//  日志
// ============================================================
void p2p_c_set_log_callback(p2p_log_cb_t cb) {
    g_c_log_cb = cb;
    install_bridges();
}

// ============================================================
//  生命周期
// ============================================================
p2p_handle* p2p_c_init(int port, const char* key_path) {
    // 先装桥接（确保后续回调能被转发）
    install_bridges();

    std::string kp = key_path ? key_path : "";
    if (!p2p_init((uint16_t)port, kp)) {
        return nullptr;
    }

    p2p_handle* h = new p2p_handle_s();
    h->magic = P2P_HANDLE_MAGIC;
    h->port = port;
    h->key_path = kp;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    g_handles.insert(h);
    return h;
}

void p2p_c_shutdown(p2p_handle* h) {
    if (!h) return;
    {
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        if (g_handles.find(h) == g_handles.end()) return;  // 无效句柄，直接返回
        g_handles.erase(h);  // 先从活动集合移除：后续 valid_handle() 必返回 false
    }
    // ★ 关键：先释放 g_handles_mutex 再调 p2p_shutdown()。
    //   否则 p2p_shutdown() 触发的断连回调里若又调回 p2p_c_*（如 p2p_c_disconnect），
    //   会重入同一把非递归锁 → 死锁。锁外调用即可规避。
    // ★ 拆台前先清空所有 C 回调全局（含 stream handler 表），避免 p2p_shutdown()
    //   触发的断连/日志/流事件在拆台期回灌进外部绑定层（cgo nil 陷阱 / Python
    //   trampoline 崩溃）。与 Go 侧 libp2p_stop 的 StopNotify + 清空 cgo 全局呼应。
    g_c_log_cb  = nullptr;
    g_c_conn_cb = nullptr;
    g_c_disc_cb = nullptr;
    g_c_msg_cb  = nullptr;
    g_c_sub_cb  = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_c_stream_cbs_mutex);
        g_c_stream_cbs.clear();
    }
    p2p_shutdown();
    // ★ 不再 delete h：保留内存，从根本上杜绝 "shutdown 后误用 handle" 的
    //   use-after-free（handle 已移出 g_handles，valid_handle 返回 false）。
    //   单进程生命周期内至多泄漏一个 handle，符合零依赖 / 单节点设计，可接受。
}

int p2p_c_is_running(p2p_handle* h) {
    (void)h;
    return p2p_is_running() ? 1 : 0;
}

// ============================================================
//  节点信息
// ============================================================
char* p2p_c_get_peer_id(p2p_handle* h) {
    if (!valid_handle(h)) return nullptr;
    return dup_str(p2p_get_peer_id());
}

char* p2p_c_get_multiaddr(p2p_handle* h) {
    if (!valid_handle(h)) return nullptr;
    return dup_str(p2p_get_multiaddr());
}

void p2p_c_free_string(char* s) {
    if (s) std::free(s);
}

int p2p_c_peer_count(p2p_handle* h) {
    (void)h;
    return (int)p2p_peer_count();
}

int p2p_c_is_connected(p2p_handle* h, const char* peer_id) {
    (void)h;
    return p2p_is_connected(peer_id ? peer_id : "") ? 1 : 0;
}

// ============================================================
//  连接
// ============================================================
int p2p_c_connect(p2p_handle* h, const char* multiaddr) {
    if (!valid_handle(h)) return -1;
    return p2p_connect(multiaddr ? multiaddr : "") ? 0 : -1;
}

int p2p_c_disconnect(p2p_handle* h, const char* peer_id) {
    if (!valid_handle(h)) return -1;
    return p2p_disconnect(peer_id ? peer_id : "") ? 0 : -1;
}

// ============================================================
//  消息
// ============================================================
int p2p_c_send(p2p_handle* h, const char* peer_id, const char* data, int len) {
    if (!valid_handle(h)) return -1;
    if (!peer_id || !data || len < 0) return -1;
    return p2p_send(peer_id, std::string(data, len)) ? 0 : -1;
}

int p2p_c_broadcast(p2p_handle* h, const char* data, int len) {
    if (!valid_handle(h)) return -1;
    if (!data || len < 0) return -1;
    return p2p_broadcast(std::string(data, len)) ? 0 : -1;
}

// ============================================================
//  Stream
// ============================================================
unsigned long long p2p_c_open_stream(p2p_handle* h, const char* peer_id, const char* protocol) {
    if (!valid_handle(h)) return 0;
    if (!peer_id || !protocol) return 0;
    return p2p_open_stream(peer_id, protocol);
}

int p2p_c_stream_write(p2p_handle* h, unsigned long long sid, const char* buf, int len) {
    if (!valid_handle(h)) return -1;
    if (!buf || len < 0) return -1;
    return p2p_stream_write(sid, buf, len);
}

int p2p_c_stream_read(p2p_handle* h, unsigned long long sid, char* buf, int len, int timeout_ms) {
    if (!valid_handle(h)) return -1;
    if (!buf || len <= 0) return -1;
    return p2p_stream_read(sid, buf, len, timeout_ms);
}

void p2p_c_close_stream(p2p_handle* h, unsigned long long sid) {
    if (!valid_handle(h)) return;
    p2p_close_stream(sid);
}

// ============================================================
//  回调
// ============================================================
void p2p_c_set_message_callback(p2p_handle* h, p2p_msg_cb_t cb) {
    (void)h;
    g_c_msg_cb = cb;
    install_bridges();
}

void p2p_c_set_connect_callback(p2p_handle* h, p2p_peer_cb_t cb) {
    (void)h;
    g_c_conn_cb = cb;
    install_bridges();
}

void p2p_c_set_disconnect_callback(p2p_handle* h, p2p_peer_cb_t cb) {
    (void)h;
    g_c_disc_cb = cb;
    install_bridges();
}

void p2p_c_set_subscribe_callback(p2p_handle* h, p2p_msg_cb_t cb) {
    (void)h;
    g_c_sub_cb = cb;
    install_bridges();
}

void p2p_c_set_stream_handler(p2p_handle* h, const char* protocol, p2p_stream_cb_t cb) {
    (void)h;
    if (!protocol) return;
    std::string proto = protocol;

    {
        std::lock_guard<std::mutex> lock(g_c_stream_cbs_mutex);
        g_c_stream_cbs[proto] = cb;
    }

    // 注册到 C++ 层（每个协议独立注册）
    p2p_set_stream_handler(proto, [proto](uint64_t sid, const std::string& peer) {
        p2p_stream_cb_t ccb = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_c_stream_cbs_mutex);
            auto it = g_c_stream_cbs.find(proto);
            if (it != g_c_stream_cbs.end()) ccb = it->second;
        }
        if (ccb) ccb((unsigned long long)sid, proto.c_str(), peer.c_str());
    });
}

// ============================================================
//  PubSub
// ============================================================
int p2p_c_subscribe(p2p_handle* h, const char* topic) {
    if (!valid_handle(h)) return -1;
    if (!topic || topic[0] == '\0') return -1;
    return p2p_subscribe(topic) ? 0 : -1;
}

int p2p_c_unsubscribe(p2p_handle* h, const char* topic) {
    if (!valid_handle(h)) return -1;
    if (!topic || topic[0] == '\0') return -1;
    return p2p_unsubscribe(topic) ? 0 : -1;
}

int p2p_c_publish(p2p_handle* h, const char* topic, const char* data, int len) {
    if (!valid_handle(h)) return -1;
    if (!topic || topic[0] == '\0') return -1;
    if (!data || len < 0) return -1;
    return p2p_publish(topic, std::string(data, len)) ? 0 : -1;
}

// ============================================================
//  中继
// ============================================================
int p2p_c_enable_relay(p2p_handle* h, int enable) {
    if (!valid_handle(h)) return -1;
    return p2p_enable_relay(enable != 0) ? 0 : -1;
}

int p2p_c_connect_via_relay(p2p_handle* h, const char* peer_id, const char* relay_addr) {
    if (!valid_handle(h)) return -1;
    if (!peer_id || !relay_addr) return -1;
    return p2p_connect_via_relay(peer_id, relay_addr) ? 0 : -1;
}

// ============================================================
//  版本
// ============================================================
const char* p2p_c_version() {
    static std::string v = p2p_version();
    return v.c_str();
}