package main

/*
#include <stdlib.h>
#include <string.h>

typedef void (*msg_cb_t)(char* peer, char* data);
typedef void (*peer_cb_t)(char* peer);
typedef void (*stream_cb_t)(unsigned long long id, char* proto, char* peer);

static msg_cb_t g_msg_cb = NULL;
static peer_cb_t g_conn_cb = NULL;
static peer_cb_t g_disc_cb = NULL;
static msg_cb_t g_sub_cb = NULL;
static stream_cb_t g_stream_cb = NULL;

static inline void set_msg_cb(msg_cb_t cb) { g_msg_cb = cb; }
static inline void set_conn_cb(peer_cb_t cb) { g_conn_cb = cb; }
static inline void set_disc_cb(peer_cb_t cb) { g_disc_cb = cb; }
static inline void set_sub_cb(msg_cb_t cb) { g_sub_cb = cb; }
static inline void set_stream_cb(stream_cb_t cb) { g_stream_cb = cb; }

static inline void call_msg_cb(char* p, char* d) { if (g_msg_cb) g_msg_cb(p, d); }
static inline void call_conn_cb(char* p) { if (g_conn_cb) g_conn_cb(p); }
static inline void call_disc_cb(char* p) { if (g_disc_cb) g_disc_cb(p); }
static inline void call_sub_cb(char* p, char* d) { if (g_sub_cb) g_sub_cb(p, d); }
static inline void call_stream_cb(unsigned long long id, char* proto, char* peer) {
    if (g_stream_cb) g_stream_cb(id, proto, peer);
}
// ★ 清空所有 cgo 回调全局（在 g_host.Close() 前调用，避免拆台期经
//   realmNotifiee -> C.call_*_cb -> realm_*_cb_bridge 触发 nil cgo 陷阱）。
static inline void realm_clear_c_callbacks() {
    g_conn_cb = NULL; g_disc_cb = NULL; g_msg_cb = NULL;
    g_sub_cb = NULL; g_stream_cb = NULL;
}
static inline void* realm_get_handle() {
    static int g_handle = 0;
    return &g_handle;
}
*/
import "C"

import (
	"context"
	"crypto/rand"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unsafe"

	"github.com/libp2p/go-libp2p"
	pubsub "github.com/libp2p/go-libp2p-pubsub"
	"github.com/libp2p/go-libp2p/core/crypto"
	"github.com/libp2p/go-libp2p/core/host"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/core/peerstore"
	"github.com/libp2p/go-libp2p/core/protocol"
	"github.com/libp2p/go-libp2p/p2p/protocol/circuitv2/relay"
	ma "github.com/multiformats/go-multiaddr"
)

// ============================================================
//
//	全局状态
//
// ============================================================
var (
	g_host   host.Host
	g_ctx    context.Context
	g_cancel context.CancelFunc
	g_ps     *pubsub.PubSub
	g_relay  *relay.Relay

	g_streams   = make(map[uint64]network.Stream)
	g_stream_id uint64
	g_stream_mu sync.Mutex

	g_topics   = make(map[string]*pubsub.Topic)
	g_subs     = make(map[string]*pubsub.Subscription)
	g_topic_mu sync.Mutex
)

func main() {}

// ---- 从文件加载密钥，不存在则生成并保存 ----
func loadOrCreateKey(path string) (crypto.PrivKey, error) {
	if data, err := os.ReadFile(path); err == nil {
		priv, err := crypto.UnmarshalPrivateKey(data)
		if err == nil {
			return priv, nil
		}
	}

	priv, _, err := crypto.GenerateEd25519Key(rand.Reader)
	if err != nil {
		return nil, err
	}
	data, err := crypto.MarshalPrivateKey(priv)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		return nil, err
	}
	return priv, nil
}

// ---- 连接事件监听器 ----
type realmNotifiee struct{}

func (n *realmNotifiee) Listen(network.Network, ma.Multiaddr)      {}
func (n *realmNotifiee) ListenClose(network.Network, ma.Multiaddr) {}

func (n *realmNotifiee) Connected(_ network.Network, conn network.Conn) {
	peerID := conn.RemotePeer().String()
	C.call_conn_cb(C.CString(peerID))
}

func (n *realmNotifiee) Disconnected(_ network.Network, conn network.Conn) {
	peerID := conn.RemotePeer().String()
	C.call_disc_cb(C.CString(peerID))
}

func (n *realmNotifiee) OpenedStream(network.Network, network.Stream) {}
func (n *realmNotifiee) ClosedStream(network.Network, network.Stream) {}

// ============================================================
//
//	Stream 管理
//
// ============================================================
func registerStream(s network.Stream) C.ulonglong {
	g_stream_mu.Lock()
	defer g_stream_mu.Unlock()
	g_stream_id++
	g_streams[g_stream_id] = s
	return C.ulonglong(g_stream_id)
}

func getStream(id C.ulonglong) network.Stream {
	g_stream_mu.Lock()
	defer g_stream_mu.Unlock()
	return g_streams[uint64(id)]
}

func unregisterStream(id C.ulonglong) {
	g_stream_mu.Lock()
	defer g_stream_mu.Unlock()
	delete(g_streams, uint64(id))
}

// ============================================================
//
//	生命周期
//
// ============================================================
//
//export libp2p_start
func libp2p_start(port C.int, key_path *C.char) unsafe.Pointer {
	keyFile := C.GoString(key_path)
	if keyFile == "" {
		keyFile = "./.libp2p_key"
	}
	priv, err := loadOrCreateKey(keyFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[p2p] 密钥加载失败: %v\n", err)
		return nil
	}

	g_ctx, g_cancel = context.WithCancel(context.Background())

	h, err := libp2p.New(
		libp2p.Identity(priv),
		libp2p.ListenAddrStrings(
			fmt.Sprintf("/ip4/0.0.0.0/tcp/%d", port),
			fmt.Sprintf("/ip4/0.0.0.0/udp/%d/quic-v1", port),
		),
		libp2p.EnableRelay(),
		libp2p.EnableHolePunching(),
		libp2p.NATPortMap(),
	)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[p2p] host 创建失败: %v\n", err)
		return nil
	}
	g_host = h
	g_host.Network().Notify(&realmNotifiee{})

	// ★ 注册默认消息流 handler
	g_host.SetStreamHandler("/realm/1.0.0", func(s network.Stream) {
		defer s.Close()
		buf := make([]byte, 4096)
		for {
			n, err := s.Read(buf)
			if n > 0 {
				C.call_msg_cb(
					C.CString(s.Conn().RemotePeer().String()),
					C.CString(string(buf[:n])),
				)
			}
			if err != nil {
				break
			}
		}
	})

	// ★ GossipSub + readiness（订阅者就绪后才 deliver）
	ps, err := pubsub.NewGossipSub(g_ctx, g_host)
	if err != nil {
		g_host.Close()
		g_host = nil
		return nil
	}
	g_ps = ps

	return C.realm_get_handle()
}

//export libp2p_stop
func libp2p_stop(ctx unsafe.Pointer) {
	if g_host != nil {
		// ★ 先摘除 notifiee 并清空 cgo 回调全局，再关闭主机。
		//   原因：注册过任意 C 回调后，g_host.Close() 在拆台期会经
		//   realmNotifiee -> C.call_*_cb -> realm_*_cb_bridge 触发
		//   nil cgo 函数指针陷阱（EXC_BAD_ACCESS / SIGBUS），进程崩溃；
		//   裸节点（从不注册回调）则 Close() 正常。StopNotify + 清空全局
		//   彻底切断这条调用链。
		g_host.Network().StopNotify(&realmNotifiee{})
		C.realm_clear_c_callbacks()
		g_host.Close()
		g_host = nil
	}
	g_ps = nil
	g_relay = nil
	if g_cancel != nil {
		g_cancel()
		g_cancel = nil
	}
	g_ctx = nil

	// ★ 清理 stream
	g_stream_mu.Lock()
	g_streams = make(map[uint64]network.Stream)
	g_stream_id = 0
	g_stream_mu.Unlock()

	// ★ 清理 topic
	g_topic_mu.Lock()
	g_topics = make(map[string]*pubsub.Topic)
	g_subs = make(map[string]*pubsub.Subscription)
	g_topic_mu.Unlock()
}

//export libp2p_version
func libp2p_version() *C.char {
	return C.CString("realm-libp2p 1.0.0")
}

//export libp2p_free_string
func libp2p_free_string(s *C.char) {
	C.free(unsafe.Pointer(s))
}

// ============================================================
//
//	节点信息
//
// ============================================================
//
//export libp2p_get_peer_id
func libp2p_get_peer_id(ctx unsafe.Pointer) *C.char {
	if g_host == nil {
		return C.CString("")
	}
	return C.CString(g_host.ID().String())
}

//export libp2p_get_multiaddr
func libp2p_get_multiaddr(ctx unsafe.Pointer) *C.char {
	if g_host == nil {
		return C.CString("")
	}
	addrs := g_host.Addrs()
	if len(addrs) == 0 {
		return C.CString("")
	}
	// 优先找非回环的 TCP 地址
	var best ma.Multiaddr
	for _, a := range addrs {
		s := a.String()
		if strings.Contains(s, "127.0.0.1") || strings.Contains(s, "::1") {
			continue
		}
		if strings.Contains(s, "/tcp/") {
			best = a
			break
		}
	}
	if best == nil {
		best = addrs[0]
	}
	return C.CString(fmt.Sprintf("%s/p2p/%s", best, g_host.ID()))
}

//export libp2p_peer_count
func libp2p_peer_count(ctx unsafe.Pointer) C.int {
	if g_host == nil {
		return 0
	}
	return C.int(len(g_host.Network().Peers()))
}

//export libp2p_is_connected
func libp2p_is_connected(ctx unsafe.Pointer, peer_id *C.char) C.int {
	if g_host == nil {
		return 0
	}
	pid, err := peer.Decode(C.GoString(peer_id))
	if err != nil {
		return 0
	}
	if g_host.Network().Connectedness(pid) == network.Connected {
		return 1
	}
	return 0
}

// ============================================================
//
//	连接管理
//
// ============================================================
//
//export libp2p_connect
func libp2p_connect(ctx unsafe.Pointer, multiaddr *C.char) C.int {
	if g_host == nil {
		return -1
	}
	maddr, err := ma.NewMultiaddr(C.GoString(multiaddr))
	if err != nil {
		return -2
	}
	info, err := peer.AddrInfoFromP2pAddr(maddr)
	if err != nil {
		return -3
	}
	g_host.Peerstore().AddAddrs(info.ID, info.Addrs, peerstore.PermanentAddrTTL)
	if err := g_host.Connect(g_ctx, *info); err != nil {
		return -4
	}
	return 0
}

//export libp2p_disconnect
func libp2p_disconnect(ctx unsafe.Pointer, peer_id *C.char) C.int {
	if g_host == nil {
		return -1
	}
	pid, err := peer.Decode(C.GoString(peer_id))
	if err != nil {
		return -2
	}
	if err := g_host.Network().ClosePeer(pid); err != nil {
		return -3
	}
	return 0
}

// ============================================================
//
//	消息收发
//
// ============================================================
//
//export libp2p_send
func libp2p_send(ctx unsafe.Pointer, peer_id *C.char, data *C.char, length C.int) C.int {
	if g_host == nil {
		return -1
	}
	pid, err := peer.Decode(C.GoString(peer_id))
	if err != nil {
		return -2
	}
	s, err := g_host.NewStream(g_ctx, pid, "/realm/1.0.0")
	if err != nil {
		return -3
	}
	defer s.Close()

	payload := C.GoBytes(unsafe.Pointer(data), length)
	if _, err := s.Write(payload); err != nil {
		return -4
	}
	return 0
}

//export libp2p_broadcast
func libp2p_broadcast(ctx unsafe.Pointer, data *C.char, length C.int) C.int {
	if g_host == nil {
		return -1
	}
	payload := C.GoBytes(unsafe.Pointer(data), length)
	peers := g_host.Network().Peers()
	for _, pid := range peers {
		s, err := g_host.NewStream(g_ctx, pid, "/realm/1.0.0")
		if err != nil {
			continue
		}
		s.Write(payload)
		s.Close()
	}
	return 0
}

// ============================================================
//
//	Stream
//
// ============================================================
//
//export libp2p_open_stream
func libp2p_open_stream(ctx unsafe.Pointer, peer_id *C.char, proto *C.char) C.ulonglong {
	if g_host == nil {
		return 0
	}
	pid, err := peer.Decode(C.GoString(peer_id))
	if err != nil {
		return 0
	}
	s, err := g_host.NewStream(g_ctx, pid, protocol.ID(C.GoString(proto)))
	if err != nil {
		return 0
	}
	return registerStream(s)
}

//export libp2p_stream_write
func libp2p_stream_write(ctx unsafe.Pointer, id C.ulonglong, buf *C.char, length C.int) C.int {
	s := getStream(id)
	if s == nil {
		return -1
	}
	payload := C.GoBytes(unsafe.Pointer(buf), length)
	n, err := s.Write(payload)
	if err != nil {
		return -1
	}
	return C.int(n)
}

//export libp2p_stream_read
func libp2p_stream_read(ctx unsafe.Pointer, id C.ulonglong, buf *C.char, length C.int, timeout_ms C.int) C.int {
	if length <= 0 {
		return -1
	}
	s := getStream(id)
	if s == nil {
		return -1
	}
	if timeout_ms > 0 {
		s.SetReadDeadline(time.Now().Add(time.Duration(timeout_ms) * time.Millisecond))
	}
	goBuf := make([]byte, int(length))
	n, err := s.Read(goBuf)

	// 有数据先返回
	if n > 0 {
		C.memcpy(unsafe.Pointer(buf), unsafe.Pointer(&goBuf[0]), C.size_t(n))
		return C.int(n)
	}

	// 无数据，看错误
	if err != nil {
		if err == io.EOF {
			return 0 // 对端关闭
		}
		if ne, ok := err.(net.Error); ok && ne.Timeout() {
			return -2 // ★ 超时（无数据）
		}
		return -1 // 真错误
	}
	return 0
}

//export libp2p_close_stream
func libp2p_close_stream(ctx unsafe.Pointer, id C.ulonglong) {
	s := getStream(id)
	if s != nil {
		s.Close()
	}
	unregisterStream(id)
}

//export libp2p_set_stream_handler
func libp2p_set_stream_handler(ctx unsafe.Pointer, proto *C.char, cb C.stream_cb_t) {
	if g_host == nil {
		return
	}
	// ★ 把 C++ 侧传入的回调（realm_stream_cb_bridge）登记为全局 stream 回调。
	//   原代码直接丢弃了 cb，导致 call_stream_cb 调用的是从未被设置的全局 g_stream_cb，回调丢失。
	//   单一全局即可：call_stream_cb 会把协议名一并回传，由 C++ 侧按协议分发。
	C.set_stream_cb(cb)
	pid := protocol.ID(C.GoString(proto))
	g_host.SetStreamHandler(pid, func(s network.Stream) {
		id := registerStream(s)
		C.call_stream_cb(
			C.ulonglong(id),
			C.CString(string(pid)), // ★ 把协议名传回 C++
			C.CString(s.Conn().RemotePeer().String()),
		)
	})
}

// ============================================================
//
//	PubSub
//
// ============================================================
//
//export libp2p_subscribe
func libp2p_subscribe(ctx unsafe.Pointer, topic *C.char) C.int {
	if g_ps == nil {
		return -1
	}
	name := C.GoString(topic)
	if name == "" {
		return -1
	}
	g_topic_mu.Lock()
	defer g_topic_mu.Unlock()
	if _, ok := g_topics[name]; ok {
		return 0
	}
	t, err := g_ps.Join(name)
	if err != nil {
		return -2
	}
	sub, err := t.Subscribe()
	if err != nil {
		t.Close()
		return -3
	}
	g_topics[name] = t
	g_subs[name] = sub

	go func() {
		for {
			msg, err := sub.Next(g_ctx)
			if err != nil {
				return
			}
			C.call_sub_cb(
				C.CString(msg.GetFrom().String()),
				C.CString(string(msg.Data)),
			)
		}
	}()
	return 0
}

//export libp2p_unsubscribe
func libp2p_unsubscribe(ctx unsafe.Pointer, topic *C.char) C.int {
	name := C.GoString(topic)
	if name == "" {
		return -1
	}
	g_topic_mu.Lock()
	defer g_topic_mu.Unlock()
	if sub, ok := g_subs[name]; ok {
		sub.Cancel()
		delete(g_subs, name)
	}
	if t, ok := g_topics[name]; ok {
		t.Close()
		delete(g_topics, name)
	}
	return 0
}

//export libp2p_publish
func libp2p_publish(ctx unsafe.Pointer, topic *C.char, data *C.char, length C.int) C.int {
	if g_host == nil || g_ctx == nil {
		return -1
	}
	name := C.GoString(topic)
	if name == "" {
		return -1
	}
	g_topic_mu.Lock()
	t, ok := g_topics[name]
	g_topic_mu.Unlock()
	if !ok {
		return -1
	}

	payload := C.GoBytes(unsafe.Pointer(data), length)

	// ★ 3 次重试，间隔 500ms（应对 mesh 刚建立时的丢失）
	for i := 0; i < 3; i++ {
		publishCtx, cancel := context.WithTimeout(g_ctx, 2*time.Second)
		err := t.Publish(publishCtx, payload)
		cancel()
		if err == nil {
			return 0
		}
		time.Sleep(500 * time.Millisecond)
	}
	return -2
}

// ============================================================
//
//	中继
//
// ============================================================
//
//export libp2p_enable_relay
func libp2p_enable_relay(ctx unsafe.Pointer, enable C.int) C.int {
	if g_host == nil {
		return -1
	}
	if enable == 1 {
		if g_relay != nil {
			return 0 // 已启用
		}
		r, err := relay.New(g_host)
		if err != nil {
			return -2
		}
		g_relay = r // ★ 保留引用
	} else {
		g_relay = nil
	}
	return 0
}

//export libp2p_connect_via_relay
func libp2p_connect_via_relay(ctx unsafe.Pointer, peer_id *C.char, relay_addr *C.char) C.int {
	if g_host == nil {
		return -1
	}
	pid, err := peer.Decode(C.GoString(peer_id))
	if err != nil {
		return -2
	}
	relay_maddr, err := ma.NewMultiaddr(C.GoString(relay_addr))
	if err != nil {
		return -3
	}
	relay_info, err := peer.AddrInfoFromP2pAddr(relay_maddr)
	if err != nil {
		return -4
	}
	if err := g_host.Connect(g_ctx, *relay_info); err != nil {
		return -5
	}

	// ★ 用 relay_info.ID 显式构造完整地址
	if len(relay_info.Addrs) == 0 {
		return -6
	}
	circuit_str := fmt.Sprintf("%s/p2p/%s/p2p-circuit/p2p/%s",
		relay_info.Addrs[0].String(),
		relay_info.ID.String(),
		pid.String())
	circuit_maddr, err := ma.NewMultiaddr(circuit_str)
	if err != nil {
		return -7
	}
	target_info, err := peer.AddrInfoFromP2pAddr(circuit_maddr)
	if err != nil {
		return -8
	}
	if err := g_host.Connect(g_ctx, *target_info); err != nil {
		return -9
	}
	return 0
}

// ============================================================
//
//	回调注册
//
// ============================================================
//
//export libp2p_set_message_callback
func libp2p_set_message_callback(cb C.msg_cb_t) {
	C.set_msg_cb(cb)
}

//export libp2p_set_connect_callback
func libp2p_set_connect_callback(cb C.peer_cb_t) {
	C.set_conn_cb(cb)
}

//export libp2p_set_disconnect_callback
func libp2p_set_disconnect_callback(cb C.peer_cb_t) {
	C.set_disc_cb(cb)
}

//export libp2p_set_subscribe_callback
func libp2p_set_subscribe_callback(cb C.msg_cb_t) {
	C.set_sub_cb(cb)
}

//export libp2p_wakeup
func libp2p_wakeup(ctx unsafe.Pointer) {}
