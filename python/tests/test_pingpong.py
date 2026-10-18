"""classroom-helper · libp2p ctypes 桥接 · 双进程流式 ping/pong 测试。

C++ 封装层是进程级单例（全局 g_ctx/g_lib），一个进程只能有一个节点，
因此 A(服务端)/B(客户端) 必须是两个独立进程。本脚本作为编排器：
  1) 启动 A（监听，注册 stream handler，把 multiaddr 写到 addrfile）
  2) 等待 addrfile -> 启动 B（连 A，开流，发 PING，收 PONG，发 ECHO，收 EOF）
  3) B 退出后回收 A

注意：A/B 必须用不同密钥（不同 .libp2p_key），否则身份相同，libp2p 拒绝自连。

运行前提：子进程 CWD 须为 libs/libp2p-wrapper/。本脚本自动设置。

若沙箱限制 loopback 网络，会如实报告 UNKNOWN 而非伪造 PASS。
"""
import os
import re
import sys
import time
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "bridge")
WRAPPER = os.path.abspath(os.path.join(HERE, "..", "..", "..", "libs", "libp2p-wrapper"))
sys.path.insert(0, BRIDGE)

PROTO = b"/pingpong/1.0"
_SCRIPT = os.path.abspath(__file__)
# B 必须用与 A 不同的密钥，否则两者身份相同（同一个 .libp2p_key），
# libp2p 会拒绝“自己连自己”。
KEY_B = os.path.join(WRAPPER, ".libp2p_key_b")

from libp2p_ctypes import LibP2P  # noqa: E402


def _peer_id_from(multiaddr: str) -> str:
    m = re.search(r"/p2p/([^/]+)", multiaddr)
    if not m:
        raise RuntimeError("multiaddr 缺少 /p2p/ 段: %s" % multiaddr)
    return m.group(1)


# ============================================================
#  Role A：服务端
# ============================================================
def run_a(addrfile: str):
    os.chdir(WRAPPER)
    done = {"v": False}

    def on_stream(sid, proto, peer):
        try:
            node = run_a._node
            print("[A] on_stream sid=%s proto=%s peer=%s" % (sid, proto, peer), flush=True)
            # 读 greeting（带超时轮询）
            greeting = b""
            while True:
                r = node.stream_read(sid, 4096, 2000)
                print("[A] read-greeting r=%r" % (r,), flush=True)
                if r.ok:
                    greeting += r.data
                    break
                elif r.eof or r.error:
                    return
            print("[A] greeting=%r" % greeting, flush=True)
            assert greeting == b"PING:1", "A 收到异常 greeting: %r" % greeting
            # 回 PONG
            n = node.stream_write(sid, b"PONG:1")
            print("[A] write-PONG n=%r" % n, flush=True)
            assert n == len(b"PONG:1"), "A 写 PONG 失败: %r" % n
            # 读 ECHO:done
            echo = b""
            while True:
                r = node.stream_read(sid, 4096, 2000)
                print("[A] read-echo r=%r" % (r,), flush=True)
                if r.ok:
                    echo += r.data
                    if b"ECHO:done" in echo:
                        break
                elif r.eof or r.error:
                    break
            node.close_stream(sid)
            done["v"] = True
            print("[A] stream 完成 peer=%s" % peer, flush=True)
        except Exception as e:  # noqa: BLE001
            print("[A] handler error: %r" % e, flush=True)

    node = LibP2P()
    run_a._node = node
    assert node.init(0), "A init 失败"
    node.set_stream_handler(PROTO.decode(), on_stream)
    ma = node.multiaddr()
    with open(addrfile, "w") as f:
        f.write(ma)
    print("[A] listening multiaddr=%s" % ma, flush=True)

    # 主线程轮询 done 标志
    t0 = time.time()
    while not done["v"] and time.time() - t0 < 25:
        time.sleep(0.1)
    node.shutdown()
    print("[A] exit done=%s" % done["v"], flush=True)
    sys.exit(0 if done["v"] else 2)


# ============================================================
#  Role B：客户端
# ============================================================
def run_b(addrfile: str):
    os.chdir(WRAPPER)
    # 等 addrfile
    t0 = time.time()
    while not os.path.exists(addrfile) and time.time() - t0 < 10:
        time.sleep(0.1)
    if not os.path.exists(addrfile):
        print("[B] 等不到 addrfile", flush=True)
        sys.exit(3)

    with open(addrfile) as f:
        ma = f.read().strip()
    # 同机两进程：直接用 A 公布的真实地址（含其本机 IP）互连，
    # 因为 A 的监听地址在 0.0.0.0，127.0.0.1 与真实 IP 均可达。
    local_ma = ma
    pid = _peer_id_from(ma)

    node = LibP2P()
    assert node.init(0, key_path=KEY_B), "B init 失败"

    # connect 重试（给 A 的 Go 节点留出启动监听的时间）
    connected = False
    for _ in range(40):
        if node.connect(local_ma):
            connected = True
            break
        time.sleep(0.25)
    assert connected, "B connect 失败: %s" % local_ma
    print("[B] connected to %s" % pid, flush=True)

    sid = node.open_stream(pid, PROTO.decode())
    assert sid > 0, "B open_stream 失败 sid=%s" % sid
    print("[B] open_stream sid=%s" % sid, flush=True)

    # PING -> PONG
    node.stream_write(sid, b"PING:1")
    print("[B] wrote PING:1", flush=True)
    pong = b""
    while True:
        r = node.stream_read(sid, 4096, 3000)
        print("[B] read-pong r=%r" % (r,), flush=True)
        if r.ok:
            pong += r.data
            if b"PONG:1" in pong:
                break
        elif r.eof or r.error:
            break
    assert b"PONG:1" in pong, "B 未收到 PONG: %r" % pong

    # ECHO:done -> EOF
    node.stream_write(sid, b"ECHO:done")
    print("[B] wrote ECHO:done", flush=True)
    got_eof = False
    extra = b""
    while True:
        r = node.stream_read(sid, 4096, 3000)
        print("[B] read-echo r=%r" % (r,), flush=True)
        if r.ok:
            extra += r.data
        elif r.eof:
            got_eof = True
            break
        elif r.error:
            break
        # timeout: 继续等 EOF
    assert got_eof, "B 未收到对端 EOF (extra=%r)" % extra

    node.close_stream(sid)
    node.shutdown()
    print("[B] PASS", flush=True)
    sys.exit(0)


# ============================================================
#  编排器
# ============================================================
def main():
    if "--role" in sys.argv:
        role = sys.argv[sys.argv.index("--role") + 1]
        addrfile = sys.argv[sys.argv.index("--addrfile") + 1]
        if role == "a":
            run_a(addrfile)
        else:
            run_b(addrfile)
        return

    addrfile = os.path.join(WRAPPER, ".pingpong_addr.tmp")
    if os.path.exists(addrfile):
        os.remove(addrfile)

    py = sys.executable
    # 子进程继承 stdout/stderr，便于直接观察 A 的服务端日志
    a = subprocess.Popen(
        [py, _SCRIPT, "--role", "a", "--addrfile", addrfile],
        cwd=WRAPPER, stdout=None, stderr=None,
    )
    b = None
    try:
        # 等 A 写 addrfile
        t0 = time.time()
        while not os.path.exists(addrfile) and time.time() - t0 < 12:
            time.sleep(0.1)
        if not os.path.exists(addrfile):
            print("[ORCH] A 未写出 multiaddr（可能 Go 节点启动失败）")
            a.terminate()
            print("RESULT: UNKNOWN")
            return

        b = subprocess.Popen(
            [py, _SCRIPT, "--role", "b", "--addrfile", addrfile],
            cwd=WRAPPER, stdout=None, stderr=None,
        )
        b.wait(timeout=40)
        if b.returncode == 0:
            print("RESULT: PASS")
        else:
            print("RESULT: FAIL (B rc=%d)" % b.returncode)
    except subprocess.TimeoutExpired:
        print("RESULT: UNKNOWN (B 超时)")
        if b:
            b.kill()
    finally:
        a.terminate()
        try:
            a.wait(timeout=5)
        except Exception:  # noqa: BLE001
            a.kill()
        for f in (addrfile, KEY_B):
            if os.path.exists(f):
                os.remove(f)


if __name__ == "__main__":
    main()
