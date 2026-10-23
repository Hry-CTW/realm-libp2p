#include "p2p_libp2p.h"
#include <iostream>
#include <thread>
#include <chrono>

int main(int argc, char* argv[]) {
    p2p_set_log_callback([](P2PLogLevel, const std::string& msg) {
        std::cout << "[P2P] " << msg << std::endl;
    });
    p2p_set_connect_callback([](const std::string& peer) {
        std::cout << "🔗 节点连入: " << peer << std::endl;
    });
    p2p_set_disconnect_callback([](const std::string& peer) {
        std::cout << "🔌 节点断开: " << peer << std::endl;
    });

    // ★ 删掉 peer_id 参数
    if (!p2p_init(0, "./.libp2p_key")) {
        std::cerr << "初始化失败" << std::endl;
        return 1;
    }

    std::cout << "Peer ID: " << p2p_get_peer_id() << std::endl;
    std::cout << "Multiaddr: " << p2p_get_multiaddr() << std::endl;

    // ★ 先注册回调，再订阅（顺序更清晰）
    p2p_set_subscribe_callback([](const std::string& peer, const std::string& data) {
        std::cout << "[订阅消息] " << peer << ": " << data << std::endl;
    });
    p2p_subscribe("realm");

    if (argc > 1) {
        std::string target = argv[1];
        std::cout << "连接到: " << target << std::endl;
        if (p2p_connect(target)) {
            std::cout << "✅ 连接成功" << std::endl;

            // ★ 等待 pubsub mesh 建立
            std::cout << "等待 3 秒让 pubsub mesh 建立..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));

            // ★ 循环发布 5 次
            for (int i = 0; i < 5; ++i) {
                std::string msg = "hello from C++ #" + std::to_string(i + 1);
                bool ok = p2p_publish("realm", msg);
                std::cout << "发布 [" << (ok ? "OK" : "FAIL") << "]: " << msg << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            // 直接 send
            size_t pos = target.find("/p2p/");
            if (pos != std::string::npos) {
                std::string peer_id = target.substr(pos + 5);
                p2p_send(peer_id, "direct message");
            }
        } else {
            std::cout << "❌ 连接失败" << std::endl;
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(60));
    p2p_shutdown();
    return 0;
}