"""classroom-helper · libp2p ctypes 桥接 · 确定性生命周期测试。

不需网络，仅验证：CDLL 加载 -> p2p_c_init 拉起 Go 节点 ->
peer_id/multiaddr/version 取回 -> is_running/peer_count -> 幂等 shutdown。

运行前提：进程 CWD 须为 libs/libp2p-wrapper/（C++ 层按 ./libp2p_wrapper.dylib dlopen Go 库）。
本脚本会在入口自动 chdir 到 wrapper 目录。
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "bridge")
WRAPPER = os.path.abspath(os.path.join(HERE, "..", "..", "..", "libs", "libp2p-wrapper"))
sys.path.insert(0, BRIDGE)

from libp2p_ctypes import LibP2P  # noqa: E402


def _chdir_wrapper():
    os.chdir(WRAPPER)


def test_lifecycle():
    _chdir_wrapper()
    node = LibP2P()

    # 未初始化时取信息应安全返回 None / False
    assert node.peer_id() is None, "未初始化 peer_id 应为 None"
    assert node.is_running() is False, "未初始化 is_running 应为 False"

    # init
    ok = node.init(0)
    assert ok, "p2p_c_init 失败"
    assert node.handle is not None, "handle 为空"

    # version（C++ 层在 init 后才加载 Go 库）
    v = node.version()
    assert v and v != "unknown", f"version 异常: {v!r}"

    # peer_id
    pid = node.peer_id()
    assert pid and len(pid) > 20, f"peer_id 异常: {pid!r}"

    # multiaddr 含 /p2p/<peerid>
    ma = node.multiaddr()
    assert ma and "/p2p/" in ma, f"multiaddr 异常: {ma!r}"
    assert pid in ma, "multiaddr 未含本节点 peer_id"

    # 运行状态
    assert node.is_running() is True
    assert node.peer_count() == 0, f"初始 peer_count 应为 0, 实得 {node.peer_count()}"

    # 关闭 + 幂等
    node.shutdown()
    assert node.is_running() is False, "shutdown 后 is_running 应为 False"
    assert node.peer_id() is None, "shutdown 后取信息应安全返回 None"
    node.shutdown()  # 二次调用不应抛错

    print("[test_bridge] PASS  version=%s" % v)


def test_context_manager():
    _chdir_wrapper()
    with LibP2P() as node:
        assert node.is_running()
        assert node.peer_id()
    assert node.is_running() is False
    print("[test_bridge] PASS  context-manager")


if __name__ == "__main__":
    t0 = time.time()
    test_lifecycle()
    test_context_manager()
    print("ALL PASS  (%.2fs)" % (time.time() - t0))
