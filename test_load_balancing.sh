#!/bin/bash

echo "======================================"
echo "智能负载均衡测试脚本"
echo "======================================"

# 检查服务是否启动
echo "1. 检查服务状态..."
for port in 9527 9528 9529; do
    echo -n "服务器 $port: "
    if curl -s -f http://127.0.0.1:$port/health > /dev/null; then
        echo "✅ 正常"
    else
        echo "❌ 无法连接"
        exit 1
    fi
done
echo

# 测试负载统计接口
echo "2. 检查负载统计接口..."
for port in 9527 9528 9529; do
    echo "服务器 $port 统计信息:"
    curl -s http://127.0.0.1:$port/stats | jq '.' 2>/dev/null || echo "无法获取统计信息"
    echo
done

# 测试写入操作
echo "3. 测试写入操作..."
for i in {1..10}; do
    key="test-key-$i"
    value="test-value-$i"
    echo -n "写入 $key: "
    
    # 随机选择一个服务器进行写入
    server_port=$((9527 + (RANDOM % 3)))
    response=$(curl -s -X POST -H "Content-Type: application/json" \
        -d "{\"$key\":\"$value\"}" \
        http://127.0.0.1:$server_port/)
    
    if [[ "$response" == "OK" ]]; then
        echo "✅ 成功 (服务器 $server_port)"
    else
        echo "❌ 失败: $response"
    fi
done
echo

# 测试读取操作
echo "4. 测试读取操作..."
for i in {1..5}; do
    key="test-key-$i"
    echo -n "读取 $key: "
    
    # 随机选择一个服务器进行读取
    server_port=$((9527 + (RANDOM % 3)))
    response=$(curl -s http://127.0.0.1:$server_port/$key)
    
    if echo "$response" | jq -e ".\"$key\"" > /dev/null 2>&1; then
        echo "✅ 成功 (服务器 $server_port): $response"
    else
        echo "❌ 失败: $response"
    fi
done
echo

# 测试负载分布
echo "5. 测试负载分布..."
echo "向不同服务器发送大量请求，观察负载分布..."

# 向服务器1发送请求
for i in {1..20}; do
    curl -s -X POST -H "Content-Type: application/json" \
        -d "{\"load-test-1-$i\":\"value-$i\"}" \
        http://127.0.0.1:9527/ > /dev/null &
done

# 向服务器2发送请求
for i in {1..15}; do
    curl -s -X POST -H "Content-Type: application/json" \
        -d "{\"load-test-2-$i\":\"value-$i\"}" \
        http://127.0.0.1:9528/ > /dev/null &
done

# 向服务器3发送请求
for i in {1..10}; do
    curl -s -X POST -H "Content-Type: application/json" \
        -d "{\"load-test-3-$i\":\"value-$i\"}" \
        http://127.0.0.1:9529/ > /dev/null &
done

echo "等待请求完成..."
wait

echo "6. 检查负载分布结果..."
for port in 9527 9528 9529; do
    echo "服务器 $port 负载统计:"
    curl -s http://127.0.0.1:$port/stats | jq '.nodes | to_entries[] | select(.key | contains("cache-server")) | {node: .key, request_count: .value.request_count, avg_response_time: .value.avg_response_time, is_healthy: .value.is_healthy}' 2>/dev/null
    echo
done

# 测试故障转移
echo "7. 测试故障转移机制..."
echo "模拟节点故障（停止一个容器）..."
echo "请手动停止一个容器来测试故障转移："
echo "docker stop sdcs-cache-server-1-1"
echo "然后按任意键继续..."
read -n 1

echo "测试故障转移..."
for i in {1..5}; do
    key="failover-test-$i"
    echo -n "测试 $key: "
    
    response=$(curl -s -X POST -H "Content-Type: application/json" \
        -d "{\"$key\":\"failover-value-$i\"}" \
        http://127.0.0.1:9527/)
    
    if [[ "$response" == "OK" ]]; then
        echo "✅ 成功 (故障转移生效)"
    else
        echo "❌ 失败: $response"
    fi
done

echo
echo "======================================"
echo "负载均衡测试完成"
echo "======================================"
