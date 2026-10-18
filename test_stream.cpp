#include "p2p_libp2p.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <string>

static const char* SHELL_PROTO = "/realm/shell/1.0.0";

int main(int argc, char* argv[]) {
    // ---- 日志 ----
    p2p_set_log_callback([](P2PLogLevel, const std::string& msg) {
        std::cout << "[P2P] " << msg << std::endl;
    });

    // ---- 连接回调 ----
    p2p_set_connect_callback([](const std::string& peer) {
        std::cout << "🔗 连入: " << peer << std::endl;
    });

    // ---- ★ 服务端：注册 stream handler ----
    p2p_set_stream_handler(SHELL_PROTO, [](uint64_t sid, const std::string& peer) {
        std::cout << "📥 [服务端] 收到流 " << sid << " 来自 " << peer << std::endl;

        // 每个流用独立线程处理，避免阻塞 Go 的回调线程
        std::thread([sid, peer]() {
            char buf[4096];
            int echo_count = 0;

            while (true) {
                int n = p2p_stream_read(sid, buf, sizeof(buf), 3000);

                if (n > 0) {
                    std::string msg(buf, n);
                    std::cout << "📨 [服务端] 收到 " << n << " 字节: " << msg << std::endl;

                    // Echo 回去
                    std::string reply = "[echo] " + msg;
                    p2p_stream_write(sid, reply.c_str(), (int)reply.size());
                    echo_count++;

                    // 收到 "bye" 就关闭
                    if (msg.find("bye") != std::string::npos) {
                        std::cout << "👋 [服务端] 收到 bye，关闭流" << std::endl;
                        break;
                    }
                }
                else if (n == 0) {
                    std::cout << "📪 [服务端] 对端关闭流 " << sid << std::endl;
                    break;
                }
                else if (n == -2) {
                    // 超时，继续等
                    continue;
                }
                else {
                    std::cout << "❌ [服务端] 流错误 " << sid << std::endl;
                    break;
                }
            }

            p2p_close_stream(sid);
            std::cout << "🔚 [服务端] 流 " << sid << " 已关闭，共 echo " 
                      << echo_count << " 次" << std::endl;
        }).detach();
    });

    // ---- 初始化 ----
    if (!p2p_init(0, "./.libp2p_key")) {
        std::cerr << "初始化失败" << std::endl;
        return 1;
    }

    std::cout << "Peer ID: " << p2p_get_peer_id() << std::endl;
    std::cout << "Multiaddr: " << p2p_get_multiaddr() << std::endl;

    // ---- 如果是客户端模式 ----
    if (argc > 1) {
        std::string target = argv[1];
        std::cout << "\n=== 客户端模式 ===" << std::endl;
        std::cout << "连接到: " << target << std::endl;

        if (!p2p_connect(target)) {
            std::cerr << "❌ 连接失败" << std::endl;
            return 1;
        }
        std::cout << "✅ 连接成功" << std::endl;

        // 等连接稳定
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 从 multiaddr 提取 peer_id
        size_t pos = target.find("/p2p/");
        if (pos == std::string::npos) {
            std::cerr << "❌ 无法解析 peer_id" << std::endl;
            return 1;
        }
        std::string peer_id = target.substr(pos + 5);

        // ---- 打开流 ----
        std::cout << "\n=== 打开流 ===" << std::endl;
        uint64_t sid = p2p_open_stream(peer_id, SHELL_PROTO);
        if (sid == 0) {
            std::cerr << "❌ 打开流失败" << std::endl;
            return 1;
        }
        std::cout << "✅ 流已打开，ID = " << sid << std::endl;

        // ---- 发送 + 接收 5 轮 ----
        std::cout << "\n=== 开始收发测试 ===" << std::endl;
        for (int i = 1; i <= 5; ++i) {
            std::string msg = "message #" + std::to_string(i);
            std::cout << "📤 [客户端] 发送: " << msg << std::endl;
            int sent = p2p_stream_write(sid, msg.c_str(), (int)msg.size());
            if (sent < 0) {
                std::cerr << "❌ 写入失败" << std::endl;
                break;
            }

            // 读回显（超时 3 秒）
            char buf[4096];
            int n = p2p_stream_read(sid, buf, sizeof(buf), 3000);
            if (n > 0) {
                std::string reply(buf, n);
                std::cout << "📥 [客户端] 收到回显: " << reply << std::endl;
            } else if (n == 0) {
                std::cout << "📪 [客户端] 对端关闭" << std::endl;
                break;
            } else if (n == -2) {
                std::cout << "⏱️ [客户端] 读超时" << std::endl;
            } else {
                std::cerr << "❌ [客户端] 读错误" << std::endl;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // ---- 发 bye 关闭 ----
        std::cout << "\n=== 发送 bye ===" << std::endl;
        std::string bye = "bye";
        p2p_stream_write(sid, bye.c_str(), (int)bye.size());

        // 收最后一条回显
        char buf[4096];
        int n = p2p_stream_read(sid, buf, sizeof(buf), 3000);
        if (n > 0) {
            std::cout << "📥 [客户端] 最后回显: " << std::string(buf, n) << std::endl;
        }

        // ---- 关闭流 ----
        std::cout << "\n=== 关闭流 ===" << std::endl;
        p2p_close_stream(sid);
        std::cout << "✅ 测试完成" << std::endl;
    }
    else {
        std::cout << "\n=== 服务端模式，等待连接 ===" << std::endl;
    }

    // ---- 保持运行 ----
    std::this_thread::sleep_for(std::chrono::seconds(30));
    p2p_shutdown();
    return 0;
}