// realm-relay — 独立的 libp2p circuit v2 中继 + AutoNAT 服务节点。
//
// 用途：在不同 NAT 后的 realm-shell 节点之间提供中继跳板。
// 运行：go run ./cmd/realm-relay   或   go build -o realm-relay ./cmd/realm-relay && ./realm-relay
//       （可选）REALM_RELAY_PORT=4001 指定监听端口；身份持久化到 realm-relay.key。
//
// 启动后打印 Peer ID 与 Multiaddr，将形如
//   /ip4/<公网IP>/tcp/<端口>/p2p/<PeerID>
// 的 Multiaddr 用作各 shell 节点的 REALM_RELAY 环境变量即可实现跨局域网互联。
package main

import (
	"crypto/rand"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/host"
)

const keyFile = "realm-relay.key"

// loadOrCreateKey 从 keyFile 读取持久化私钥；不存在则生成 Ed25519 并保存，
// 保证中继 Peer ID 在不同次运行间稳定（否则 REALM_RELAY 地址会失效）。
func loadOrCreateKey() (crypto.PrivKey, error) {
	if data, err := os.ReadFile(keyFile); err == nil {
		return crypto.UnmarshalPrivateKey(data)
	}
	priv, _, err := crypto.GenerateKeyPairWithReader(crypto.Ed25519, -1, rand.Reader)
	if err != nil {
		return nil, err
	}
	data, err := crypto.MarshalPrivateKey(priv)
	if err != nil {
		return nil, err
	}
	if err := os.WriteFile(keyFile, data, 0600); err != nil {
		return nil, err
	}
	return priv, nil
}

func main() {
	priv, err := loadOrCreateKey()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[relay] 密钥加载失败: %v\n", err)
		os.Exit(1)
	}

	port := 4001
	if p := os.Getenv("REALM_RELAY_PORT"); p != "" {
		fmt.Sscanf(p, "%d", &port)
	}

	h, err := libp2p.New(
		libp2p.Identity(priv),
		libp2p.ListenAddrStrings(
			fmt.Sprintf("/ip4/0.0.0.0/tcp/%d", port),
			fmt.Sprintf("/ip4/0.0.0.0/udp/%d/quic-v1", port),
		),
		libp2p.EnableRelayService(), // ★ circuit v2 中继服务端
		libp2p.EnableNATService(),   // ★ 提供 AutoNAT 服务，供 shell 节点判定 NAT 类型
		libp2p.EnableHolePunching(),
		// ★ 强制声明「公网可达」：go-libp2p 的 EnableRelayService 仅在节点被判定为公网可达时
		//   才真正激活 circuit v2 中继服务（否则不广播 /libp2p/circuit/relay/0.2.0，导致所有
		//   中继电路失败）。中继节点本就部署在公网机器上，强制 public 既符合语义，也能让回环/内网
		//   自测时中继正常生效。
		libp2p.ForceReachabilityPublic(),
	)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[relay] host 创建失败: %v\n", err)
		os.Exit(1)
	}
	defer h.Close()

	id := h.ID()
	fmt.Println("===== realm-relay 已启动 =====")
	fmt.Printf("Peer ID:   %s\n", id)
	for _, a := range h.Addrs() {
		fmt.Printf("Multiaddr: %s/p2p/%s\n", a, id)
	}
	fmt.Println("将上面的 Multiaddr（取 /ip4 公网可达的那条）用作 shell 节点的 REALM_RELAY 环境变量。")
	fmt.Println("按 Ctrl-C 退出。")

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
	<-ch
	fmt.Println("[relay] 正在关闭…")
	_ = h.Close()
}

// 避免未使用导入告警（host 包用于 h 的类型），保留显式引用以便清晰。
var _ host.Host
