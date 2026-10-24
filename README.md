# libp2p_wrapper

> 跨语言 libp2p 封装库 —— 基于 Go 的 `go-libp2p`，暴露 C++ 和 C ABI 接口。

提供简洁的 P2P 通信能力：节点发现、连接、消息、Stream、PubSub。适用于需要去中心化网络的应用（REALM、文件传输、远程 shell、分布式协作等）。

---

## 特性

- **多传输协议**：TCP / QUIC / WebTransport / WebRTC-Direct
- **NAT 穿透**：STUN / ICE / Circuit Relay / Hole Punching
- **节点发现**：mDNS / DHT / Bootstrap
- **PubSub**：GossipSub 主题消息
- **持久身份**：密钥文件存储，Peer ID 重启不变
- **多语言**：C++（推荐）、C、Python（ctypes）、其他 FFI

---

## 目录结构

```
libp2p/
├── libp2p_wrapper.go       # Go 实现（编译为 .dylib/.so/.dll）
├── libp2p_wrapper.h        # Go c-shared 自动生成
├── p2p_libp2p.h            # C++ 接口头文件
├── p2p_libp2p.cpp          # C++ 接口实现
├── p2p_libp2p_c.h          # C ABI 头文件
├── p2p_libp2p_c.cpp        # C ABI 实现
├── test_p2p.cpp            # C++ 测试
├── test_stream.cpp         # Stream 测试
├── test_stress.cpp         # 压力测试
├── test.py                 # Python 测试
└── README.md
```

---

## 依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| Go | 1.21+ | 编译 Go 动态库 |
| C++ 编译器 | C++17 | g++ / clang++ |
| Python | 3.8+（可选） | ctypes 测试 |

---

## 编译

### 1. 编译 Go 动态库

```bash
# macOS
go build -buildmode=c-shared -o libp2p_wrapper.dylib libp2p_wrapper.go

# Linux
go build -buildmode=c-shared -o libp2p_wrapper.so libp2p_wrapper.go

# Windows（用 mingw）
CC=x86_64-w64-mingw32-gcc \
CGO_ENABLED=1 GOOS=windows GOARCH=amd64 \
go build -buildmode=c-shared -o libp2p_wrapper.dll libp2p_wrapper.go
```

### 2. 编译 C++ ABI 动态库

```bash
# macOS
g++ -std=c++17 -shared -fPIC \
    p2p_libp2p.cpp p2p_libp2p_c.cpp \
    -o libp2p_cpp.dylib -ldl

# Linux
g++ -std=c++17 -shared -fPIC \
    p2p_libp2p.cpp p2p_libp2p_c.cpp \
    -o libp2p_cpp.so -ldl

# Windows（mingw）
x86_64-w64-mingw32-g++ -std=c++17 -shared \
    p2p_libp2p.cpp p2p_libp2p_c.cpp \
    -o libp2p_cpp.dll -lws2_32
```

### 3. 编译测试

```bash
g++ -std=c++17 test_p2p.cpp p2p_libp2p.cpp -ldl -o test_p2p
g++ -std=c++17 test_stream.cpp p2p_libp2p.cpp -ldl -o test_stream
g++ -std=c++17 test_stress.cpp p2p_libp2p.cpp -ldl -o test_stress
```

---

## 快速开始

### C++ 示例

```cpp
#include "p2p_libp2p.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    // 1. 日志
    p2p_set_log_callback([](P2PLogLevel lv, const std::string& msg) {
        std::cout << "[P2P] " << msg << std::endl;
    });

    // 2. 连接回调
    p2p_set_connect_callback([](const std::string& peer) {
        std::cout << "连入: " << peer << std::endl;
    });

    // 3. 初始化
    if (!p2p_init(0, "./.libp2p_key")) {
        return 1;
    }
    std::cout << "Peer ID: " << p2p_get_peer_id() << std::endl;
    std::cout << "Multiaddr: " << p2p_get_multiaddr() << std::endl;

    // 4. 订阅
    p2p_set_subscribe_callback([](const std::string& peer, const std::string& data) {
        std::cout << peer << ": " << data << std::endl;
    });
    p2p_subscribe("realm");

    // 5. 连对方 + 发消息
    if (argc > 1) {
        p2p_connect(argv[1]);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        p2p_publish("realm", "hello");
    }

    std::this_thread::sleep_for(std::chrono::seconds(60));
    p2p_shutdown();
    return 0;
}
```

### C 示例

```c
#include "p2p_libp2p_c.h"
#include <stdio.h>
#include <unistd.h>

void on_log(int level, const char* msg) {
    printf("[P2P] %s\n", msg);
}

void on_conn(const char* peer) {
    printf("连入: %s\n", peer);
}

int main() {
    p2p_c_set_log_callback(on_log);

    p2p_handle* h = p2p_c_init(0, "./.libp2p_key");
    if (!h) return 1;

    char* pid = p2p_c_get_peer_id(h);
    printf("Peer ID: %s\n", pid);
    p2p_c_free_string(pid);

    p2p_c_set_connect_callback(h, on_conn);
    p2p_c_subscribe(h, "realm");

    sleep(60);
    p2p_c_shutdown(h);
    return 0;
}
```

### Python 示例

```python
import ctypes
from ctypes import c_char_p, c_int, c_void_p, CFUNCTYPE

lib = ctypes.CDLL("./libp2p_cpp.dylib")

# ★ 必须保留回调引用，否则崩溃
callbacks = []

LOG_CB = CFUNCTYPE(None, c_int, c_char_p)

@LOG_CB
def on_log(level, msg):
    print(f"[P2P] {msg.decode()}")

callbacks.append(on_log)

lib.p2p_c_set_log_callback.argtypes = [LOG_CB]
lib.p2p_c_set_log_callback(on_log)

lib.p2p_c_init.restype = c_void_p
lib.p2p_c_init.argtypes = [c_int, c_char_p]
h = lib.p2p_c_init(0, b"./.libp2p_key")

lib.p2p_c_get_peer_id.restype = c_void_p
lib.p2p_c_get_peer_id.argtypes = [c_void_p]
lib.p2p_c_free_string.argtypes = [c_void_p]

p = lib.p2p_c_get_peer_id(h)
print("Peer ID:", ctypes.cast(p, c_char_p).value.decode())
lib.p2p_c_free_string(p)
```

---

## API 参考

### C++ 接口（`p2p_libp2p.h`）

#### 生命周期

| 函数 | 说明 |
|------|------|
| `p2p_init(port, key_path)` | 初始化节点 |
| `p2p_shutdown()` | 关闭节点 |
| `p2p_is_running()` | 是否运行中 |

#### 节点信息

| 函数 | 说明 |
|------|------|
| `p2p_get_peer_id()` | 获取本节点 ID |
| `p2p_get_multiaddr()` | 获取本节点完整地址 |
| `p2p_peer_count()` | 已连接节点数 |
| `p2p_is_connected(peer_id)` | 是否已连接 |

#### 连接

| 函数 | 说明 |
|------|------|
| `p2p_connect(multiaddr)` | 连接对方 |
| `p2p_disconnect(peer_id)` | 断开连接 |

#### 消息

| 函数 | 说明 |
|------|------|
| `p2p_send(peer_id, data)` | 发消息给单个节点 |
| `p2p_broadcast(data)` | 广播给所有连接 |

#### Stream

| 函数 | 说明 |
|------|------|
| `p2p_open_stream(peer_id, protocol)` | 打开长连接流 |
| `p2p_stream_write(sid, buf, len)` | 写入 |
| `p2p_stream_read(sid, buf, len, timeout_ms)` | 读取 |
| `p2p_close_stream(sid)` | 关闭 |
| `p2p_set_stream_handler(protocol, cb)` | 注册入站流处理 |

#### PubSub

| 函数 | 说明 |
|------|------|
| `p2p_subscribe(topic)` | 订阅主题 |
| `p2p_unsubscribe(topic)` | 退订 |
| `p2p_publish(topic, data)` | 发布消息 |

#### 回调

| 函数 | 说明 |
|------|------|
| `p2p_set_log_callback(cb)` | 日志 |
| `p2p_set_message_callback(cb)` | 收到直发消息 |
| `p2p_set_connect_callback(cb)` | 节点连入 |
| `p2p_set_disconnect_callback(cb)` | 节点断开 |
| `p2p_set_subscribe_callback(cb)` | 收到订阅消息 |
| `p2p_set_stream_handler(protocol, cb)` | 收到入站流 |

### C 接口（`p2p_libp2p_c.h`）

C 接口一一对应 C++ 接口，函数名前缀 `p2p_c_`，需要传递 `p2p_handle*`。

**内存约定**：
- 返回 `char*` 的函数，调用者必须用 `p2p_c_free_string` 释放
- 回调中传入的 `char*` 由库管理，回调返回后失效（不要保存指针）

---

## 协议约定

| 协议 | 用途 |
|------|------|
| `/realm/1.0.0` | 默认消息流（`p2p_send` 使用） |
| `/realm/shell/1.0.0` | 远程 shell（应用层定义） |
| `/realm/file/1.0.0` | 文件传输（应用层定义） |

**注意**：`p2p_send` 内部使用 `/realm/1.0.0`，接收端必须有对应的 handler（已在 `libp2p_start` 中注册）。

---

<<<<<<< HEAD
## 跨局域网 / 中继（realm-relay）

当两端处于不同 NAT 后、直连不可达时，可用内置的独立中继节点 `cmd/realm-relay`
（circuit v2 中继 + AutoNAT 服务）做跳板。

- **编译**：`CGO_ENABLED=0 go build -o realm-relay ./cmd/realm-relay`（纯 Go，单文件零依赖）。
- **运行**：`./realm-relay`（可选 `REALM_RELAY_PORT=4001` 改端口；身份持久化于 `realm-relay.key`）。
- **shell 节点接入**：在 `shell_server` / `shell_client` 启动前设置环境变量
  `REALM_RELAY=<中继完整 multiaddr，含 /p2p/<ID>>`，节点会启用静态 AutoRelay。
- **控制端连接**：用「中继地址 /p2p-circuit/p2p/<被控端PeerID>」作为 `shell_client` 的参数。

完整操作步骤、本地回环自测、防火墙与排错见 **`../realm-shell/CROSS_LAN.md`**。

---

=======
>>>>>>> 52a2c94487ab90d6644e68837c40771a0a01a2fd
## 常见问题

### 1. `zsh: segmentation fault`（Python）

**原因**：回调对象被 GC 回收。

**修复**：把所有回调对象存到全局列表：

```python
callbacks = []
@LOG_CB
def on_log(...): ...
callbacks.append(on_log)
```

### 2. `dlopen failed` / `cannot load library`

**原因**：`libp2p_wrapper.dylib` 不在搜索路径。

**修复**：
- 把动态库放在可执行文件同目录
- 或设置环境变量：`DYLD_LIBRARY_PATH=. ./test`
- 或用 `-Wl,-rpath,/abs/path` 编译时指定

### 3. `p2p_connect` 返回失败

**原因**：
- 对方 multiaddr 格式错误（必须以 `/p2p/<peer_id>` 结尾）
- 对方未启动
- 网络不通

**排查**：
- 先用 `ping` 确认网络连通
- 用 `p2p_get_multiaddr()` 确认对方地址
- 检查回调是否被触发

### 4. PubSub 消息丢失

**原因**：GossipSub mesh 建立需要时间。

**修复**：
- `connect` 后 `sleep(3)` 再 `publish`
- 或多次重试 publish

### 5. 大消息卡住

**原因**：libp2p 有流控（backpressure），写满缓冲区会阻塞。

**修复**：
- 用独立线程写
- 或者用分帧协议（参考 `test_stress.cpp`）

### 6. Peer ID 每次重启都变

**原因**：密钥文件未正确加载/保存。

**修复**：
- 确认 `key_path` 指向有效目录
- 检查文件权限（`0600`）
- 检查密钥文件是否被损坏

---

## 架构

```
┌─────────────────────────────────────────┐
│  应用层（REALM / Python / 其他）         │
├─────────────────────────────────────────┤
│  C ABI 层（p2p_libp2p_c.cpp）            │
│  - 不透明句柄、C 函数指针桥接             │
├─────────────────────────────────────────┤
│  C++ 封装层（p2p_libp2p.cpp）            │
│  - std::function、dlopen 加载、回调缓存  │
├─────────────────────────────────────────┤
│  Go 层（libp2p_wrapper.go）              │
│  - go-libp2p、c-shared 导出              │
├─────────────────────────────────────────┤
│  go-libp2p（传输/加密/多路复用）         │
└─────────────────────────────────────────┘
```

---

## 版本

- 当前：`1.0.0`
- 更新日志见 `CHANGELOG.md`

---

## 许可

待定。