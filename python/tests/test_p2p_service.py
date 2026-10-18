"""P2PService 无 GUI 冒烟测试（不依赖 PySide6，可在 CI / 本机直接跑）。

验证 P2#6 的核心：节点能随服务启动、给出身份、关闭后资源释放、且进程 CWD
不被永久改变（GUI 安全前提）。
"""
import os
import sys
import unittest

# 把项目根加入 sys.path，使 common / apps 都可按包导入
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from apps.desk_widget.python.p2p.service import P2PService  # noqa: E402


class TestP2PService(unittest.TestCase):
    def test_start_stop_lifecycle(self):
        svc = P2PService()
        self.assertFalse(svc.running)
        self.assertIsNone(svc.peer_id)

        ok = svc.start()
        self.assertTrue(ok, "P2PService.start() 应成功（Go 库需已编译）")
        self.assertTrue(svc.running)

        pid = svc.peer_id
        ma = svc.multiaddr
        self.assertTrue(pid, "peer_id 不应为空")
        self.assertTrue(ma, "multiaddr 不应为空")
        print(f"[test] peer_id={pid}")
        print(f"[test] multiaddr={ma}")

        # CWD 必须已在 start() 内还原，不能被永久切到 libp2p 目录
        self.assertNotEqual(
            os.getcwd(), svc._libp2p_dir,
            "start() 不应永久改变进程 CWD",
        )

        # 钩子应能被调用（不崩）
        got = {}
        svc.on_connect = lambda p: got.setdefault("connect", p)
        svc.on_disconnect = lambda p: got.setdefault("disconnect", p)
        svc.on_message = lambda p, d: got.setdefault("msg", (p, d))

        svc.stop()
        self.assertFalse(svc.running)
        self.assertIsNone(svc.peer_id)

    def test_context_manager(self):
        with P2PService() as svc:
            self.assertTrue(svc.running)
            self.assertTrue(svc.peer_id)
        self.assertFalse(svc.running)

    def test_shutdown_with_callbacks_no_crash(self):
        """回归 P0#4：注册过 C 回调后显式 shutdown 不得触发 nil cgo 陷阱(SIGBUS/139)。

        P2PService.stop() 现走真实 p2p_c_shutdown；此处直接对 LibP2P 注册
        connect/disconnect/message/subscribe 回调并调公开 shutdown()，复现此前
        崩溃的精确条件（Go libp2p_stop -> g_host.Close() 拆台期）。
        """
        import sys as _sys
        _bridge_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
            "python",
        )
        if _bridge_path not in _sys.path:
            _sys.path.insert(0, _bridge_path)
        from bridge.libp2p_ctypes import LibP2P, libp2p_wrapper_dir  # noqa: E402

        saved = os.getcwd()
        os.chdir(libp2p_wrapper_dir())
        try:
            node = LibP2P()
            self.assertTrue(node.init(0), "init 失败")
            node.set_connect_callback(lambda p: None)
            node.set_disconnect_callback(lambda p: None)
            node.set_message_callback(lambda p, d: None)
            node.set_subscribe_callback(lambda p, d: None)
            # 公开 API 现走真实 p2p_c_shutdown，应干净返回（不崩）
            node.shutdown()
            self.assertIsNone(node.handle)
        finally:
            os.chdir(saved)


if __name__ == "__main__":
    unittest.main(verbosity=2)
