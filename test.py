import ctypes
from ctypes import c_char_p, c_int, c_void_p, c_ulonglong, CFUNCTYPE
import time
import sys
import threading


lib = ctypes.CDLL("./libp2p_cpp.dylib")

# ============================================================
# 回调类型
# ============================================================
LOG_CB    = CFUNCTYPE(None, c_int, c_char_p)
PEER_CB   = CFUNCTYPE(None, c_char_p)
MSG_CB    = CFUNCTYPE(None, c_char_p, c_char_p)
STREAM_CB = CFUNCTYPE(None, c_ulonglong, c_char_p, c_char_p)

# ★ 防止回调被 GC
callbacks = []

def keep(cb):
    callbacks.append(cb)
    return cb

# ============================================================
# 回调实现
# ============================================================
@keep
@LOG_CB
def on_log(level, msg):
    try:
        print(f"[P2P] {msg.decode('utf-8', errors='replace')}")
    except Exception:
        pass

@keep
@PEER_CB
def on_conn(peer):
    try:
        print(f"🔗 连入: {peer.decode('utf-8', errors='replace')}")
    except Exception:
        pass

@keep
@PEER_CB
def on_disc(peer):
    try:
        print(f"🔌 断开: {peer.decode('utf-8', errors='replace')}")
    except Exception:
        pass

@keep
@MSG_CB
def on_sub(peer, data):
    try:
        print(f"[订阅] {peer.decode()}: {data.decode()}")
    except Exception:
        pass

@keep
@STREAM_CB
def on_stream(sid, proto, peer):
    sid_val = int(sid)
    proto_s = proto.decode('utf-8', errors='replace')
    peer_s = peer.decode('utf-8', errors='replace')
    threading.Thread(
        target=handle_stream, args=(sid_val, proto_s, peer_s), daemon=True
    ).start()

def handle_stream(sid, proto, peer):
    try:
        print(f"📥 流 {sid} 协议 {proto} 来自 {peer[:16]}...")
        while True:
            buf = ctypes.create_string_buffer(4096)
            n = lib.p2p_c_stream_read(g_handle, sid, buf, 4096, 5000)
            buf = ctypes.create_string_buffer(4096)
            n = lib.p2p_c_stream_read(g_handle, sid, buf, 4096, 5000)
            if n > 0:
                data = buf.raw[:n]
                print(f"📨 [服务端] 收到 [{n}]: {data.decode('utf-8', errors='replace')}")
                # echo 回去
                lib.p2p_c_stream_write(g_handle, sid, buf, n)
            elif n == 0:
                print(f"📪 [服务端] 流 {sid} 对端关闭")
                break
            elif n == -2:
                continue  # 超时
            else:
                print(f"❌ [服务端] 流 {sid} 错误: {n}")
                break

        lib.p2p_c_close_stream(g_handle, sid)
    except Exception as e:
        print(f"[stream 回调异常] {e}")

# ============================================================
# 函数签名
# ============================================================
lib.p2p_c_init.restype  = c_void_p
lib.p2p_c_init.argtypes = [c_int, c_char_p]
lib.p2p_c_shutdown.restype  = None
lib.p2p_c_shutdown.argtypes = [c_void_p]
lib.p2p_c_get_peer_id.restype  = c_void_p
lib.p2p_c_get_peer_id.argtypes = [c_void_p]
lib.p2p_c_get_multiaddr.restype  = c_void_p
lib.p2p_c_get_multiaddr.argtypes = [c_void_p]
lib.p2p_c_free_string.restype  = None
lib.p2p_c_free_string.argtypes = [c_void_p]
lib.p2p_c_set_log_callback.restype  = None
lib.p2p_c_set_log_callback.argtypes = [LOG_CB]
lib.p2p_c_set_connect_callback.restype  = None
lib.p2p_c_set_connect_callback.argtypes = [c_void_p, PEER_CB]
lib.p2p_c_set_disconnect_callback.restype  = None
lib.p2p_c_set_disconnect_callback.argtypes = [c_void_p, PEER_CB]
lib.p2p_c_set_subscribe_callback.restype  = None
lib.p2p_c_set_subscribe_callback.argtypes = [c_void_p, MSG_CB]
lib.p2p_c_set_stream_handler.restype  = None
lib.p2p_c_set_stream_handler.argtypes = [c_void_p, c_char_p, STREAM_CB]
lib.p2p_c_subscribe.restype  = c_int
lib.p2p_c_subscribe.argtypes = [c_void_p, c_char_p]
lib.p2p_c_publish.restype  = c_int
lib.p2p_c_publish.argtypes = [c_void_p, c_char_p, c_char_p, c_int]
lib.p2p_c_connect.restype  = c_int
lib.p2p_c_connect.argtypes = [c_void_p, c_char_p]
lib.p2p_c_open_stream.restype  = c_ulonglong
lib.p2p_c_open_stream.argtypes = [c_void_p, c_char_p, c_char_p]
lib.p2p_c_stream_read.restype  = c_int
lib.p2p_c_stream_read.argtypes = [c_void_p, c_ulonglong, c_char_p, c_int, c_int]
lib.p2p_c_stream_write.restype  = c_int
lib.p2p_c_stream_write.argtypes = [c_void_p, c_ulonglong, c_char_p, c_int]
lib.p2p_c_close_stream.restype  = None
lib.p2p_c_close_stream.argtypes = [c_void_p, c_ulonglong]

# ============================================================
# 主流程
# ============================================================
g_handle = None

def main():
    global g_handle

    # 1. 日志
    lib.p2p_c_set_log_callback(on_log)

    # 2. 初始化
    g_handle = lib.p2p_c_init(0, b"./.libp2p_key")
    if not g_handle:
        print("❌ 初始化失败")
        sys.exit(1)

    # 3. Peer ID
    p = lib.p2p_c_get_peer_id(g_handle)
    if p:
        print(f"Peer ID: {ctypes.cast(p, c_char_p).value.decode()}")
        lib.p2p_c_free_string(p)

    p = lib.p2p_c_get_multiaddr(g_handle)
    if p:
        print(f"Multiaddr: {ctypes.cast(p, c_char_p).value.decode()}")
        lib.p2p_c_free_string(p)

    # 4. 回调
    lib.p2p_c_set_connect_callback(g_handle, on_conn)
    lib.p2p_c_set_disconnect_callback(g_handle, on_disc)
    lib.p2p_c_set_subscribe_callback(g_handle, on_sub)
    lib.p2p_c_set_stream_handler(g_handle, b"/realm/shell/1.0.0", on_stream)

    # 5. 订阅
    lib.p2p_c_subscribe(g_handle, b"realm")

    # 6. 客户端模式
    if len(sys.argv) > 1:
        target = sys.argv[1].encode()
        print(f"连接到: {sys.argv[1]}")
        rc = lib.p2p_c_connect(g_handle, target)
        if rc == 0:
            print("✅ 连接成功")
            time.sleep(1)

            # ---- PubSub ----
            msg = b"hello from Python"
            lib.p2p_c_publish(g_handle, b"realm", msg, len(msg))

            # ---- Stream ----
            time.sleep(1)
            peer_id = sys.argv[1].split("/p2p/")[-1].encode()
            proto = b"/realm/shell/1.0.0"

            print(f"\n=== 打开流 ===")
            sid = lib.p2p_c_open_stream(g_handle, peer_id, proto)
            if sid == 0:
                print("❌ 打开流失败")
            else:
                print(f"✅ 流已打开: {sid}")
                for i in range(3):
                    line = f"python-msg-{i+1}".encode()
                    sent = lib.p2p_c_stream_write(g_handle, sid, line, len(line))
                    print(f"📤 发送 [{sent}]: {line.decode()}")

                    buf = ctypes.create_string_buffer(4096)
                    n = lib.p2p_c_stream_read(g_handle, sid, buf, 4096, 3000)
                    if n > 0:
                        print(f"📥 收到 [{n}]: {buf.raw[:n].decode()}")
                    elif n == 0:
                        print("📪 对端关闭")
                        break
                    elif n == -2:
                        print("⏱️ 读超时")
                    else:
                        print(f"❌ 读错误: {n}")
                        break
                    time.sleep(0.3)

                lib.p2p_c_close_stream(g_handle, sid)
                print("✅ 流已关闭")
        else:
            print(f"❌ 连接失败: {rc}")

    # 7. 等待
    print("\n按 Ctrl+C 退出...")
    try:
        time.sleep(120)
    except KeyboardInterrupt:
        pass
    lib.p2p_c_shutdown(g_handle)

if __name__ == "__main__":
    main()