#!/bin/bash

echo "======================================"
echo "负载均衡性能对比测试"
echo "======================================"

# 测试参数
REQUESTS=100
CONCURRENT=10

echo "测试参数:"
echo "- 总请求数: $REQUESTS"
echo "- 并发数: $CONCURRENT"
echo

# 测试函数
test_performance() {
    local test_name="$1"
    local server_port="$2"
    
    echo "测试: $test_name (服务器 $server_port)"
    
    # 记录开始时间
    start_time=$(date +%s%N)
    
    # 发送并发请求
    for i in $(seq 1 $REQUESTS); do
        key="perf-test-$i"
        value="value-$i"
        
        curl -s -X POST -H "Content-Type: application/json" \
            -d "{\"$key\":\"$value\"}" \
            http://127.0.0.1:$server_port/ > /dev/null &
        
        # 控制并发数
        if (( i % CONCURRENT == 0 )); then
            wait
        fi
    done
    
    # 等待所有请求完成
    wait
    
    # 记录结束时间
    end_time=$(date +%s%N)
    
    # 计算耗时
    duration=$(( (end_time - start_time) / 1000000 ))
    rps=$(( REQUESTS * 1000 / duration ))
    
    echo "  耗时: ${duration}ms"
    echo "  RPS: $rps"
    echo "  平均响应时间: $(( duration / REQUESTS ))ms"
    echo
}

# 测试各个服务器
echo "1. 测试各服务器性能..."
test_performance "服务器1" 9527
test_performance "服务器2" 9528
test_performance "服务器3" 9529

# 测试负载均衡效果
echo "2. 测试负载均衡效果..."
echo "随机请求分布测试..."

start_time=$(date +%s%N)

for i in $(seq 1 $REQUESTS); do
    key="balance-test-$i"
    value="value-$i"
    
    # 随机选择服务器
    server_port=$((9527 + (RANDOM % 3)))
    
    curl -s -X POST -H "Content-Type: application/json" \
        -d "{\"$key\":\"$value\"}" \
        http://127.0.0.1:$server_port/ > /dev/null &
    
    if (( i % CONCURRENT == 0 )); then
        wait
    fi
done

wait

end_time=$(date +%s%N)
duration=$(( (end_time - start_time) / 1000000 ))
rps=$(( REQUESTS * 1000 / duration ))

echo "负载均衡测试结果:"
echo "  总耗时: ${duration}ms"
echo "  总RPS: $rps"
echo "  平均响应时间: $(( duration / REQUESTS ))ms"
echo

# 检查负载分布
echo "3. 检查负载分布..."
for port in 9527 9528 9529; do
    echo "服务器 $port 统计:"
    stats=$(curl -s http://127.0.0.1:$port/stats)
    echo "$stats" | jq -r '.nodes | to_entries[] | select(.key | contains("cache-server")) | "  \(.key): 请求数=\(.value.request_count), 平均响应时间=\(.value.avg_response_time)ms, 健康状态=\(.value.is_healthy)"' 2>/dev/null
    echo
done

# 测试读取性能
echo "4. 测试读取性能..."
echo "读取之前写入的数据..."

start_time=$(date +%s%N)

for i in $(seq 1 $REQUESTS); do
    key="balance-test-$i"
    
    # 随机选择服务器
    server_port=$((9527 + (RANDOM % 3)))
    
    curl -s http://127.0.0.1:$server_port/$key > /dev/null &
    
    if (( i % CONCURRENT == 0 )); then
        wait
    fi
done

wait

end_time=$(date +%s%N)
duration=$(( (end_time - start_time) / 1000000 ))
rps=$(( REQUESTS * 1000 / duration ))

echo "读取性能测试结果:"
echo "  总耗时: ${duration}ms"
echo "  总RPS: $rps"
echo "  平均响应时间: $(( duration / REQUESTS ))ms"
echo

echo "======================================"
echo "性能对比测试完成"
echo "======================================"
