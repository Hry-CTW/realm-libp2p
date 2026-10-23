#!/bin/bash
# run_test.sh - 自动化压力测试

set -e
cd "$(dirname "$0")"

echo "🔨 编译..."
go build -buildmode=c-shared -o libp2p_wrapper.dylib libp2p_wrapper.go
g++ -std=c++17 -O2 test_stress.cpp p2p_libp2p.cpp -ldl -o test_stress

echo "🚀 启动服务端..."
./test_stress > server.log 2>&1 &
SERVER_PID=$!

# 等 Multiaddr 输出
for i in {1..20}; do
    if grep -q "Multiaddr:" server.log 2>/dev/null; then break; fi
    sleep 0.5
done

SERVER_ADDR=$(grep "Multiaddr:" server.log | head -1 | awk '{print $2}')
if [ -z "$SERVER_ADDR" ]; then
    echo "❌ 服务端启动失败"
    cat server.log
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

echo "📍 服务端: $SERVER_ADDR"
echo ""
cp ./test_stress ./test/test_stress
cp libp2p_wrapper.dylib ./test/libp2p_wrapper.dylib
cd ./test
# 运行客户端
./test_stress "$SERVER_ADDR" | tee client.log

# 清理
sleep 1
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "═══════════ 服务端日志 ═══════════"
cat server.log

echo ""
echo "═══════════ 结果汇总 ═══════════"
grep -E "(✅|❌)" client.log || true