"""ctypes 绑定 libs/libp2p-wrapper/libp2p_cpp.{dylib,so,dll}（C ABI: p2p_c_*）。

链路：
    Python (本文件)
      -> libp2p_cpp.dylib  (C++ 封装层, p2p_libp2p_c.cpp / p2p_libp2p.cpp)
        -> libp2p_wrapper.dylib (Go libp2p, c-shared, 运行时由 C++ 层 dlopen)

注意：
  * C++ 封装层在 p2p_c_init 时按相对 CWD 的 "./libp2p_wrapper.dylib" 去 dlopen Go 库，
    因此调用方进程的工作目录应为 libs/libp2p-wrapper/ （或显式指定 libp2p 工作目录）。
  * p2p_c_get_peer_id / p2p_c_get_multiaddr 返回的是 malloc 的 char*，
    必须调用 p2p_c_free_string 释放，否则泄漏。本桥接已封装。
  * p2p_c_stream_read 返回值语义：n>0 字节数 / 0 对端干净关闭(EOF)
    / -2 超时(无数据, 非错误, 可重试) / -1 错误(无效句柄或参数)。
  * 所有回调来自 Go 的 goroutine（非 Python 主线程），ctypes trampoline 必须持有 GIL。

C 接口签名见 libs/libp2p-wrapper/p2p_libp2p_c.h
"""
import ctypes
import os
import platform
from typing import Callable, Optional

# ============================================================
#  GIL 保护（回调来自 Go 线程）
# ============================================================
_PyGILState_Ensure = ctypes.pythonapi.PyGILState_Ensure
_PyGILState_Release = ctypes.pythonapi.PyGILState_Release
_PyGILState_Ensure.restype = ctypes.c_int
_PyGILState_Release.argtypes = [ctypes.c_int]


def _with_gil(fn: Callable) -> Callable:
    """把用户回调包进 GIL 持有，避免从 Go 线程回调 Python 时崩溃。"""
    def trampoline(*args):
        state = _PyGILState_Ensure()
        try:
            return fn(*args)
        finally:
            _PyGILState_Release(state)
    return trampoline


# ============================================================
#  stream_read 返回码
# ============================================================
READ_EOF = 0        # 对端干净关闭
READ_TIMEOUT = -2   # 超时，无数据（非错误，可重试）
READ_ERROR = -1     # 错误（无效句柄/参数）


class StreamReadResult:
    """p2p_c_stream_read 的统一返回对象，避免把 EOF/超时/错误混为一谈。"""

    __slots__ = ("n", "data")

    def __init__(self, n: int, data: bytes):
        self.n = n
        self.data = data

    @property
    def ok(self) -> bool:
        """读到数据。"""
        return self.n > 0

    @property
    def eof(self) -> bool:
        return self.n == READ_EOF

    @property
    def timeout(self) -> bool:
        return self.n == READ_TIMEOUT

    @property
    def error(self) -> bool:
        return self.n == READ_ERROR

    def __repr__(self):
        return f"StreamReadResult(n={self.n}, data={self.data!r})"


# ============================================================
#  异常
# ============================================================
class P2PError(RuntimeError):
    pass


# ============================================================
#  C 回调类型
# ============================================================
C_LOG_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)
C_MSG_CB = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_char_p)
C_PEER_CB = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
C_STREAM_CB = ctypes.CFUNCTYPE(None, ctypes.c_ulonglong, ctypes.c_char_p, ctypes.c_char_p)


def _lib_name() -> str:
    s = platform.system()
    if s == "Darwin":
        return "libp2p_cpp.dylib"
    if s == "Windows":
        return "libp2p_cpp.dll"
    return "libp2p_cpp.so"


def libp2p_wrapper_dir() -> str:
    """返回 libs/libp2p-wrapper 目录的绝对路径。

    C++ 封装层在 p2p_c_init 时会按相对 CWD 的 "./libp2p_wrapper.{dylib,so,dll}"
    dlopen Go 库，因此 start 节点前需把进程 CWD 切到这里（init 后还原即可）。
    """
    here = os.path.dirname(os.path.abspath(__file__))
    # bridge 位于 common/python/bridge/ -> 上 3 级到项目根 -> libs/libp2p-wrapper
    return os.path.abspath(os.path.join(here, "..", "..", "..", "libs", "libp2p-wrapper"))


class LibP2P:
    """libp2p 节点的 Python 封装（对接 p2p_c_* C ABI）。"""

    def __init__(self, lib_path: Optional[str] = None):
        if lib_path is None:
            here = os.path.dirname(os.path.abspath(__file__))
            lib_path = os.path.join(
                here, "..", "..", "..", "libs", "libp2p-wrapper", _lib_name()
            )
        lib_path = os.path.abspath(lib_path)
        if not os.path.exists(lib_path):
            raise FileNotFoundError(f"找不到 libp2p 封装库: {lib_path}")
        self._lib = ctypes.CDLL(lib_path)
        self._h = None  # p2p_handle*
        # 保存 ctypes 回调对象，防止被 GC
        self._cbs: dict = {}
        self._bind()

    # ----------------------------------------------------------
    #  绑定
    # ----------------------------------------------------------
    def _bind(self):
        L = self._lib
        VOID = ctypes.c_void_p
        I = ctypes.c_int
        ULL = ctypes.c_ulonglong
        CP = ctypes.c_char_p

        L.p2p_c_init.argtypes = [I, CP]
        L.p2p_c_init.restype = VOID

        L.p2p_c_shutdown.argtypes = [VOID]
        L.p2p_c_shutdown.restype = None

        L.p2p_c_is_running.argtypes = [VOID]
        L.p2p_c_is_running.restype = I

        # 返回 malloc 的 char*，需 p2p_c_free_string 释放 -> restype 用 void*
        L.p2p_c_get_peer_id.argtypes = [VOID]
        L.p2p_c_get_peer_id.restype = VOID
        L.p2p_c_get_multiaddr.argtypes = [VOID]
        L.p2p_c_get_multiaddr.restype = VOID
        L.p2p_c_free_string.argtypes = [VOID]
        L.p2p_c_free_string.restype = None

        L.p2p_c_peer_count.argtypes = [VOID]
        L.p2p_c_peer_count.restype = I
        L.p2p_c_is_connected.argtypes = [VOID, CP]
        L.p2p_c_is_connected.restype = I

        L.p2p_c_connect.argtypes = [VOID, CP]
        L.p2p_c_connect.restype = I
        L.p2p_c_disconnect.argtypes = [VOID, CP]
        L.p2p_c_disconnect.restype = I

        L.p2p_c_send.argtypes = [VOID, CP, CP, I]
        L.p2p_c_send.restype = I
        L.p2p_c_broadcast.argtypes = [VOID, CP, I]
        L.p2p_c_broadcast.restype = I

        L.p2p_c_open_stream.argtypes = [VOID, CP, CP]
        L.p2p_c_open_stream.restype = ULL
        L.p2p_c_stream_write.argtypes = [VOID, ULL, CP, I]
        L.p2p_c_stream_write.restype = I
        L.p2p_c_stream_read.argtypes = [VOID, ULL, CP, I, I]
        L.p2p_c_stream_read.restype = I
        L.p2p_c_close_stream.argtypes = [VOID, ULL]
        L.p2p_c_close_stream.restype = None

        # 回调
        L.p2p_c_set_log_callback.argtypes = [VOID, C_LOG_CB]
        L.p2p_c_set_log_callback.restype = None
        L.p2p_c_set_message_callback.argtypes = [VOID, C_MSG_CB]
        L.p2p_c_set_message_callback.restype = None
        L.p2p_c_set_connect_callback.argtypes = [VOID, C_PEER_CB]
        L.p2p_c_set_connect_callback.restype = None
        L.p2p_c_set_disconnect_callback.argtypes = [VOID, C_PEER_CB]
        L.p2p_c_set_disconnect_callback.restype = None
        L.p2p_c_set_subscribe_callback.argtypes = [VOID, C_MSG_CB]
        L.p2p_c_set_subscribe_callback.restype = None
        L.p2p_c_set_stream_handler.argtypes = [VOID, CP, C_STREAM_CB]
        L.p2p_c_set_stream_handler.restype = None

        # PubSub
        L.p2p_c_subscribe.argtypes = [VOID, CP]
        L.p2p_c_subscribe.restype = I
        L.p2p_c_unsubscribe.argtypes = [VOID, CP]
        L.p2p_c_unsubscribe.restype = I
        L.p2p_c_publish.argtypes = [VOID, CP, CP, I]
        L.p2p_c_publish.restype = I

        # 中继
        L.p2p_c_enable_relay.argtypes = [VOID, I]
        L.p2p_c_enable_relay.restype = I
        L.p2p_c_connect_via_relay.argtypes = [VOID, CP, CP]
        L.p2p_c_connect_via_relay.restype = I

        # 版本（返回 static const char*，无需释放）
        L.p2p_c_version.argtypes = []
        L.p2p_c_version.restype = CP

    # ----------------------------------------------------------
    #  生命周期
    # ----------------------------------------------------------
    def init(self, port: int = 0, key_path: Optional[str] = None) -> bool:
        """启动节点。port=0 由系统分配；key_path=None 时 C++ 层用 ./libp2p_key（相对 CWD）。"""
        kp = key_path.encode() if key_path else None
        self._h = self._lib.p2p_c_init(ctypes.c_int(port), kp)
        return bool(self._h)

    def shutdown(self):
        # 根因已修复（libp2p_wrapper.go 的 libp2p_stop 在 g_host.Close() 前
        # 先 StopNotify(&realmNotifiee{}) 并清空 cgo 回调全局，杜绝拆台期
        # 经 realmNotifiee -> C.call_*_cb -> realm_*_cb_bridge 触发的 nil cgo
        # 函数指针陷阱 / SIGBUS）。故现在可安全地显式释放节点，不再依赖进程
        # 退出时 OS 回收。见 libs/libp2p-wrapper/ 下 Go 源码注释。
        if self._h is not None:
            try:
                self._lib.p2p_c_shutdown(self._h)
            except Exception:
                # 兜底：极端情况下即便 Go 侧已防护仍异常，也不应让调用方崩溃
                pass
            self._h = None
        # 断开回调引用，允许 GC
        self._cbs.clear()

    @property
    def handle(self):
        return self._h

    def is_running(self) -> bool:
        if self._h is None:
            return False
        return bool(self._lib.p2p_c_is_running(self._h))

    # ----------------------------------------------------------
    #  节点信息
    # ----------------------------------------------------------
    def _get_cstr(self, fn) -> Optional[str]:
        """调用返回 malloc char* 的 C 函数，复制成 Python str 后释放。"""
        if self._h is None:
            return None
        ptr = fn(self._h)
        if not ptr:
            return None
        try:
            return ctypes.string_at(ptr).decode("utf-8", "replace")
        finally:
            self._lib.p2p_c_free_string(ptr)

    def peer_id(self) -> Optional[str]:
        return self._get_cstr(self._lib.p2p_c_get_peer_id)

    def multiaddr(self) -> Optional[str]:
        return self._get_cstr(self._lib.p2p_c_get_multiaddr)

    def peer_count(self) -> int:
        if self._h is None:
            return 0
        return self._lib.p2p_c_peer_count(self._h)

    def is_connected(self, peer_id: str) -> bool:
        if self._h is None:
            return False
        return bool(self._lib.p2p_c_is_connected(self._h, peer_id.encode()))

    def version(self) -> str:
        s = self._lib.p2p_c_version()
        return s.decode() if s else ""

    # ----------------------------------------------------------
    #  连接
    # ----------------------------------------------------------
    def connect(self, multiaddr: str) -> bool:
        if self._h is None:
            return False
        return self._lib.p2p_c_connect(self._h, multiaddr.encode()) == 0

    def disconnect(self, peer_id: str) -> bool:
        if self._h is None:
            return False
        return self._lib.p2p_c_disconnect(self._h, peer_id.encode()) == 0

    # ----------------------------------------------------------
    #  消息
    # ----------------------------------------------------------
    def send(self, peer_id: str, data: bytes) -> bool:
        if self._h is None:
            return False
        if isinstance(data, str):
            data = data.encode()
        return self._lib.p2p_c_send(self._h, peer_id.encode(), data, len(data)) == 0

    def broadcast(self, data: bytes) -> bool:
        if self._h is None:
            return False
        if isinstance(data, str):
            data = data.encode()
        return self._lib.p2p_c_broadcast(self._h, data, len(data)) == 0

    # ----------------------------------------------------------
    #  Stream
    # ----------------------------------------------------------
    def open_stream(self, peer_id: str, protocol: str) -> int:
        """返回 sid（>0 成功；0 表示失败，如未连接/协议未注册）。"""
        if self._h is None:
            return 0
        return self._lib.p2p_c_open_stream(self._h, peer_id.encode(), protocol.encode())

    def stream_write(self, sid: int, data: bytes) -> int:
        """返回写入字节数；-1 表示错误。"""
        if self._h is None:
            return -1
        if isinstance(data, str):
            data = data.encode()
        return self._lib.p2p_c_stream_write(self._h, ctypes.c_ulonglong(sid), data, len(data))

    def stream_read(self, sid: int, max_len: int = 4096, timeout_ms: int = 1000) -> StreamReadResult:
        """读取一次。返回 StreamReadResult：
           ok=True 有数据；eof 对端关闭；timeout 超时无数据(可重试)；error 错误。
        """
        if self._h is None:
            return StreamReadResult(READ_ERROR, b"")
        buf = ctypes.create_string_buffer(max_len)
        n = self._lib.p2p_c_stream_read(
            self._h, ctypes.c_ulonglong(sid), buf, ctypes.c_int(max_len),
            ctypes.c_int(timeout_ms),
        )
        if n > 0:
            return StreamReadResult(n, buf.raw[:n])
        return StreamReadResult(n, b"")

    def close_stream(self, sid: int):
        if self._h is not None:
            self._lib.p2p_c_close_stream(self._h, ctypes.c_ulonglong(sid))

    # ----------------------------------------------------------
    #  PubSub
    # ----------------------------------------------------------
    def subscribe(self, topic: str) -> bool:
        if self._h is None:
            return False
        return self._lib.p2p_c_subscribe(self._h, topic.encode()) == 0

    def unsubscribe(self, topic: str) -> bool:
        if self._h is None:
            return False
        return self._lib.p2p_c_unsubscribe(self._h, topic.encode()) == 0

    def publish(self, topic: str, data: bytes) -> bool:
        if self._h is None:
            return False
        if isinstance(data, str):
            data = data.encode()
        return self._lib.p2p_c_publish(self._h, topic.encode(), data, len(data)) == 0

    # ----------------------------------------------------------
    #  中继
    # ----------------------------------------------------------
    def enable_relay(self, enable: bool = True) -> bool:
        if self._h is None:
            return False
        return self._lib.p2p_c_enable_relay(self._h, 1 if enable else 0) == 0

    def connect_via_relay(self, peer_id: str, relay_addr: str) -> bool:
        if self._h is None:
            return False
        return self._lib.p2p_c_connect_via_relay(
            self._h, peer_id.encode(), relay_addr.encode()
        ) == 0

    # ----------------------------------------------------------
    #  回调注册（持有 GIL；对象挂在 self._cbs 防 GC）
    # ----------------------------------------------------------
    def set_log_callback(self, cb: Callable[[int, str], None]):
        if self._h is None:
            raise P2PError("节点未初始化")
        def _f(level: int, msg):
            cb(level, msg.decode() if isinstance(msg, bytes) else msg)
        c = C_LOG_CB(_with_gil(_f))
        self._cbs["log"] = c
        self._lib.p2p_c_set_log_callback(self._h, c)

    def set_message_callback(self, cb: Callable[[str, bytes], None]):
        if self._h is None:
            raise P2PError("节点未初始化")
        def _f(peer, data):
            cb(peer.decode() if isinstance(peer, bytes) else peer,
               data if isinstance(data, (bytes, bytearray)) else b"")
        c = C_MSG_CB(_with_gil(_f))
        self._cbs["msg"] = c
        self._lib.p2p_c_set_message_callback(self._h, c)

    def set_connect_callback(self, cb: Callable[[str], None]):
        if self._h is None:
            raise P2PError("节点未初始化")
        def _f(peer):
            cb(peer.decode() if isinstance(peer, bytes) else peer)
        c = C_PEER_CB(_with_gil(_f))
        self._cbs["conn"] = c
        self._lib.p2p_c_set_connect_callback(self._h, c)

    def set_disconnect_callback(self, cb: Callable[[str], None]):
        if self._h is None:
            raise P2PError("节点未初始化")
        def _f(peer):
            cb(peer.decode() if isinstance(peer, bytes) else peer)
        c = C_PEER_CB(_with_gil(_f))
        self._cbs["disc"] = c
        self._lib.p2p_c_set_disconnect_callback(self._h, c)

    def set_subscribe_callback(self, cb: Callable[[str, bytes], None]):
        if self._h is None:
            raise P2PError("节点未初始化")
        def _f(peer, data):
            cb(peer.decode() if isinstance(peer, bytes) else peer,
               data if isinstance(data, (bytes, bytearray)) else b"")
        c = C_MSG_CB(_with_gil(_f))
        self._cbs["sub"] = c
        self._lib.p2p_c_set_subscribe_callback(self._h, c)

    def set_stream_handler(self, protocol: str, cb: Callable[[int, str, str], None]):
        if self._h is None:
            raise P2PError("节点未初始化")
        def _f(sid, proto, peer):
            cb(sid,
               proto.decode() if isinstance(proto, bytes) else proto,
               peer.decode() if isinstance(peer, bytes) else peer)
        c = C_STREAM_CB(_with_gil(_f))
        self._cbs[f"stream:{protocol}"] = c
        self._lib.p2p_c_set_stream_handler(self._h, protocol.encode(), c)

    # ----------------------------------------------------------
    #  上下文管理
    # ----------------------------------------------------------
    def __enter__(self):
        if not self.init():
            raise P2PError("init 失败")
        return self

    def __exit__(self, exc_type, exc, tb):
        self.shutdown()
        return False


if __name__ == "__main__":
    # 快速自检：仅验证桥接与 dylib 链路（无需网络）
    # C++ 层 p2p_c_init 需相对 CWD 的 ./libp2p_wrapper.{dylib,so,dll}，
    # 故先切到 libs/libp2p-wrapper 再 init，结束还原。
    saved = os.getcwd()
    try:
        os.chdir(libp2p_wrapper_dir())
        node = LibP2P()
        if not node.init(0):
            print("init 失败")
            raise SystemExit(1)
        print("version:", node.version())
        print("peer_id:", node.peer_id())
        print("multiaddr:", node.multiaddr())
        print("is_running:", node.is_running())
        node.shutdown()
        print("shutdown OK")
    finally:
        os.chdir(saved)
