package main

// 中继回归测试（纯 Go，绕过 C++/dylib）：验证 circuit v2 中继的「预约 → 拨号 → 开流收发」链路。
//
// 关键验证点：经中继建立的连接被标记为 Limited，NewStream 必须配合
// network.WithAllowLimitedConn 才能在其上开流——这是 realm-shell 跨局域网
// 能跑通 shell 协议的前提（见 libp2p_wrapper.go 的 libp2p_open_stream）。
//
// 回环专属现象（非缺陷）：中继绑 127.0.0.1 时 Reservation.Addrs 为空（中继只通告公网地址），
// 因此本测试直接用中继真实地址构造电路拨号目标，与生产环境（中继绑公网 IP）逻辑一致。
//
// 退出码：0 = 成功；1 = 任一环节失败。

import (
	"context"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/libp2p/go-libp2p"
	circuit "github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/client"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	ma "github.com/multiformats/go-multiaddr"
)

func addrInfoFrom(t string) peer.AddrInfo {
	m, err := ma.NewMultiaddr(t)
	if err != nil {
		panic(err)
	}
	ai, err := peer.AddrInfoFromP2pAddr(m)
	if err != nil {
		panic(err)
	}
	return *ai
}

func fail(msg string) {
	fmt.Println("[repro] FAILED:", msg)
	os.Exit(1)
}

func main() {
	ctx := context.Background()

	// ---- Relay：强制公网可达，确保 circuit v2 中继服务真正激活 ----
	relay, err := libp2p.New(
		libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"),
		libp2p.EnableRelayService(),
		libp2p.EnableNATService(),
		libp2p.EnableHolePunching(),
		libp2p.ForceReachabilityPublic(),
	)
	if err != nil {
		fail(fmt.Sprintf("relay New: %v", err))
	}
	relayAI := addrInfoFrom(fmt.Sprintf("%s/p2p/%s", relay.Addrs()[0], relay.ID()))
	fmt.Println("[repro] relay id=", relay.ID())

	// ---- Server：仅 EnableRelay（中继客户端 + 接受中继入站），显式 Reserve ----
	server, err := libp2p.New(
		libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"),
		libp2p.EnableRelay(),
		libp2p.EnableHolePunching(),
		libp2p.EnableNATService(),
	)
	if err != nil {
		fail(fmt.Sprintf("server New: %v", err))
	}
	server.SetStreamHandler("/realm/shell/1.0.0", func(s network.Stream) {
		buf := make([]byte, 1024)
		n, _ := s.Read(buf)
		s.Write([]byte("PONG"))
		s.Close()
		fmt.Printf("[repro] server: got %q\n", string(buf[:n]))
	})
	fmt.Println("[repro] server id=", server.ID())

	// 显式预约中继槽位（等价 AutoRelay 的 relayFinder，但绕开通告过滤）
	resvCtx, resvCancel := context.WithTimeout(ctx, 30*time.Second)
	defer resvCancel()
	resv, err := circuit.Reserve(resvCtx, server, relayAI)
	if err != nil {
		fail(fmt.Sprintf("Reserve: %v", err))
	}
	fmt.Println("[repro] Reserve OK, 过期=", resv.Expiration)

	// 回环下中继通告地址为空 → 改用中继真实地址构造电路目标（生产环境 resv.Addrs 含公网地址）
	var circuitTarget string
	if len(resv.Addrs) > 0 {
		circuitTarget = resv.Addrs[0].String() + "/p2p/" + server.ID().String()
	} else {
		circuitTarget = relayAI.Addrs[0].String() + "/p2p/" + relayAI.ID.String() +
			"/p2p-circuit/p2p/" + server.ID().String()
	}
	targetAI := addrInfoFrom(circuitTarget)

	// ---- Client：经中继拨号 ----
	client, err := libp2p.New(
		libp2p.ListenAddrStrings("/ip4/127.0.0.1/tcp/0"),
		libp2p.EnableRelay(),
		libp2p.EnableHolePunching(),
	)
	if err != nil {
		fail(fmt.Sprintf("client New: %v", err))
	}
	fmt.Println("[repro] client id=", client.ID(), "connecting via relay...")
	cconnCtx, cconnCancel := context.WithTimeout(ctx, 30*time.Second)
	defer cconnCancel()
	if err := client.Connect(cconnCtx, targetAI); err != nil {
		fail(fmt.Sprintf("client Connect: %v", err))
	}
	fmt.Println("[repro] client Connect OK")

	// ★ 中继连接是 Limited，必须 WithAllowLimitedConn 才能在上面开流
	sctx, scancel := context.WithTimeout(ctx, 20*time.Second)
	defer scancel()
	s, err := client.NewStream(network.WithAllowLimitedConn(sctx, "realm-shell-relay"), server.ID(), "/realm/shell/1.0.0")
	if err != nil {
		fail(fmt.Sprintf("client NewStream: %v", err))
	}
	fmt.Println("[repro] client NewStream OK")
	s.Write([]byte("PING"))
	buf := make([]byte, 1024)
	n, err := s.Read(buf)
	if err != nil && err != io.EOF {
		fail(fmt.Sprintf("client Read: %v", err))
	}
	if string(buf[:n]) != "PONG" {
		fail(fmt.Sprintf("unexpected response %q", string(buf[:n])))
	}
	fmt.Printf("[repro] client got response: %q\n", string(buf[:n]))
	fmt.Println("[repro] === SUCCESS ===")
	os.Exit(0)
}
