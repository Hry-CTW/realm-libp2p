#include "p2p_libp2p.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>

static const char* PROTO = "/realm/stress/1.0.0";

// ============================================================
//  分帧协议：8 字节 LE 长度 + payload
// ============================================================
static bool write_all(uint64_t sid, const char* data, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = p2p_stream_write(sid, data + total, (int)(len - total));
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

static bool read_all(uint64_t sid, char* data, size_t len, int timeout_ms) {
    size_t total = 0;
    while (total < len) {
        int n = p2p_stream_read(sid, data + total, (int)(len - total), timeout_ms);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

static bool send_frame(uint64_t sid, const std::string& payload) {
    uint64_t len = payload.size();
    char header[8];
    for (int i = 0; i < 8; ++i) header[i] = (char)((len >> (i * 8)) & 0xFF);
    if (!write_all(sid, header, 8)) return false;
    if (len > 0 && !write_all(sid, payload.data(), payload.size())) return false;
    return true;
}

static bool recv_frame(uint64_t sid, std::string& out, int timeout_ms) {
    char header[8];
    if (!read_all(sid, header, 8, timeout_ms)) return false;
    uint64_t len = 0;
    for (int i = 0; i < 8; ++i) len |= ((uint64_t)(unsigned char)header[i]) << (i * 8);
    out.clear();
    out.reserve(len);
    char buf[65536];
    while (out.size() < len) {
        size_t remain = len - out.size();
        int want = (int)std::min(remain, sizeof(buf));
        int n = p2p_stream_read(sid, buf, want, timeout_ms);
        if (n <= 0) return false;
        out.append(buf, n);
    }
    return true;
}

// ============================================================
//  服务端
// ============================================================
static void run_server() {
    p2p_set_stream_handler(PROTO, [](uint64_t sid, const std::string& peer) {
        std::cout << "[服务端] 收到流 " << sid << " 来自 " << peer << std::endl;
        std::thread([sid]() {
            int frames = 0;
            uint64_t total_bytes = 0;
            while (true) {
                std::string payload;
                if (!recv_frame(sid, payload, 30000)) {
                    std::cout << "[服务端] 读结束" << std::endl;
                    break;
                }
                total_bytes += payload.size();
                frames++;

                if (frames <= 5 || frames % 100 == 0) {
                    std::cout << "[服务端] 帧 #" << frames
                              << " (" << payload.size() << " 字节)" << std::endl;
                }

                if (payload == "__DONE__") {
                    std::cout << "[服务端] 收到 DONE" << std::endl;
                    break;
                }
                // echo（DONE 不回）
                if (!send_frame(sid, payload)) {
                    std::cout << "[服务端] 写失败" << std::endl;
                    break;
                }
            }
            std::cout << "[服务端] 共 " << frames << " 帧, "
                      << total_bytes << " 字节" << std::endl;
            p2p_close_stream(sid);
        }).detach();
    });
}

// ============================================================
//  客户端
// ============================================================
static void run_client(const std::string& target) {
    if (!p2p_connect(target)) {
        std::cerr << "❌ 连接失败" << std::endl;
        return;
    }
    std::cout << "✅ 连接成功" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    size_t pos = target.find("/p2p/");
    if (pos == std::string::npos) { std::cerr << "无 peer_id" << std::endl; return; }
    std::string peer_id = target.substr(pos + 5);

    uint64_t sid = p2p_open_stream(peer_id, PROTO);
    if (sid == 0) { std::cerr << "❌ 打开流失败" << std::endl; return; }
    std::cout << "✅ 流 ID = " << sid << "\n" << std::endl;

    // ==================== 测试 1: 1000 条小消息 ====================
    {
        const int N = 1000;
        const int SIZE = 64;
        std::cout << "=== 测试 1: " << N << " 条小消息 (" << SIZE << " 字节/条) ===" << std::endl;
        auto t1 = std::chrono::steady_clock::now();
        int ok = 0;
        for (int i = 0; i < N; ++i) {
            std::string payload(SIZE, (char)('A' + (i % 26)));
            if (!send_frame(sid, payload)) { std::cerr << "写失败 #" << i << std::endl; break; }
            std::string reply;
            if (!recv_frame(sid, reply, 5000)) { std::cerr << "读失败 #" << i << std::endl; break; }
            if (reply == payload) ok++;
        }
        auto t2 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        std::cout << "结果: " << ok << "/" << N << " 通过, 耗时 " << ms << " ms" << std::endl;
        std::cout << (ok == N ? "✅ 小消息测试通过" : "❌ 小消息测试失败") << std::endl;
    }

    // ==================== 测试 2: 1MB 大消息 ====================
    {
        const size_t SIZE = 1024 * 1024;
        std::cout << "\n=== 测试 2: 1MB 大消息 ===" << std::endl;
        std::string big(SIZE, 'X');
        for (size_t i = 0; i < big.size(); ++i) big[i] = (char)(i & 0xFF);

        auto t1 = std::chrono::steady_clock::now();

        // 独立线程写，避免 libp2p 流控阻塞
        std::atomic<bool> write_ok{false};
        std::thread writer([&]() {
            write_ok = send_frame(sid, big);
        });

        // 主线程读回显
        std::string reply;
        bool read_ok = recv_frame(sid, reply, 60000);

        writer.join();
        auto t2 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

        std::cout << "发送: " << (write_ok ? "✅" : "❌")
                  << "  接收: " << (read_ok ? "✅" : "❌")
                  << "  大小: " << reply.size() << " 字节"
                  << "  耗时: " << ms << " ms" << std::endl;

        if (write_ok && read_ok && reply == big) {
            std::cout << "✅ 大消息测试通过" << std::endl;
        } else {
            std::cout << "❌ 大消息测试失败" << std::endl;
            if (reply.size() != big.size()) {
                std::cout << "   大小不匹配: 期望 " << big.size()
                          << ", 实际 " << reply.size() << std::endl;
            } else {
                for (size_t i = 0; i < big.size(); ++i) {
                    if (big[i] != reply[i]) {
                        std::cout << "   内容不匹配 at " << i << std::endl;
                        break;
                    }
                }
            }
        }
    }

    // 通知服务端结束
    std::cout << "\n发送 DONE" << std::endl;
    send_frame(sid, "__DONE__");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    p2p_close_stream(sid);
}

// ============================================================
//  main
// ============================================================
int main(int argc, char* argv[]) {
    p2p_set_log_callback([](P2PLogLevel lv, const std::string& msg) {
        if (lv == P2PLogLevel::ERR || lv == P2PLogLevel::WARNING)
            std::cerr << "[P2P] " << msg << std::endl;
    });

    if (!p2p_init(0, "./.libp2p_key")) {
        std::cerr << "初始化失败" << std::endl;
        return 1;
    }

    std::cout << "Peer ID: " << p2p_get_peer_id() << std::endl;
    std::cout << "Multiaddr: " << p2p_get_multiaddr() << std::endl;

    if (argc > 1) {
        run_client(argv[1]);
    } else {
        run_server();
        std::this_thread::sleep_for(std::chrono::seconds(120));
    }
    p2p_shutdown();
    return 0;
}