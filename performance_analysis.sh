#!/bin/bash

echo "======================================"
echo "分布式缓存系统性能分析"
echo "======================================"

echo "1. 系统资源信息:"
echo "CPU核心数: $(nproc)"
echo "内存信息:"
free -h
echo "磁盘信息:"
df -h /
echo

echo "2. Docker容器状态:"
docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
echo

echo "3. 容器资源使用情况:"
docker stats --no-stream --format "table {{.Container}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.MemPerc}}"
echo

echo "4. 网络连接测试:"
for port in 9527 9528 9529; do
    echo -n "端口 $port: "
    if curl -s -f http://127.0.0.1:$port/health > /dev/null; then
        echo "✅ 正常"
    else
        echo "❌ 无法连接"
    fi
done
echo

echo "5. 单个请求响应时间测试:"
for port in 9527 9528 9529; do
    echo -n "服务器 $port 响应时间: "
    time_start=$(date +%s%N)
    curl -s http://127.0.0.1:$port/health > /dev/null
    time_end=$(date +%s%N)
    time_diff=$(( (time_end - time_start) / 1000000 ))
    echo "${time_diff}ms"
done
echo

echo "6. 并发请求测试 (10个并发):"
echo "测试写入操作并发性能..."
time_start=$(date +%s%N)
for i in {1..10}; do
    curl -s -X POST -H "Content-Type: application/json" -d "{\"test-key-$i\":\"test-value-$i\"}" http://127.0.0.1:9527/ &
done
wait
time_end=$(date +%s%N)
time_diff=$(( (time_end - time_start) / 1000000 ))
echo "10个并发写入请求耗时: ${time_diff}ms"
echo

echo "7. 系统负载监控:"
echo "当前负载: $(uptime | awk -F'load average:' '{print $2}')"
echo "进程数: $(ps aux | wc -l)"
echo "Docker进程数: $(docker ps -q | wc -l)"
echo

echo "======================================"
echo "性能分析完成"
echo "======================================"
