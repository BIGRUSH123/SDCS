#!/bin/bash

# 性能测试脚本 - 针对优化后的分布式缓存系统

if [[ $# -ne 1 ]]; then
    echo "用法: $0 {cache server number}"
    exit 1
fi

cs_num=$1

# 检查依赖
which jq >/dev/null 2>&1 || {
    echo "错误: 请先安装 jq"
    exit 3
}

PORT_BASE=9526
HOST_BASE=127.0.0.1
MAX_ITER=1000  # 增加测试量
CONCURRENT_REQUESTS=50  # 并发请求数

PASS_PROMPT="\e[1;32m通过\e[0m"
FAIL_PROMPT="\e[1;31m失败\e[0m"

function get_cs() {
    port=$(($PORT_BASE + $(shuf -i 1-$cs_num -n 1)))
    echo http://$HOST_BASE:$port
}

function get_key() {
    echo "perf-key-$(shuf -i 1-$MAX_ITER -n 1)"
}

function gen_json_with_idx() {
    local idx=$1
    jq -n --arg key "perf-key-$idx" --arg value "performance test value $idx" '{($key): ($value)}'
}

# 性能测试函数
function test_concurrent_set() {
    echo "测试并发SET操作..."
    local start_time=$(date +%s%N)
    
    # 创建临时文件存储PID
    local pids=()
    
    for i in $(seq 1 $CONCURRENT_REQUESTS); do
        (
            for j in $(seq 1 10); do
                local key_idx=$((($i - 1) * 10 + $j))
                curl -s -o /dev/null -w "%{http_code}" \
                    -XPOST -H "Content-type: application/json" \
                    -d "$(gen_json_with_idx $key_idx)" \
                    $(get_cs) > /dev/null
            done
        ) &
        pids+=($!)
    done
    
    # 等待所有后台进程完成
    for pid in "${pids[@]}"; do
        wait $pid
    done
    
    local end_time=$(date +%s%N)
    local duration=$((($end_time - $start_time) / 1000000)) # 转换为毫秒
    echo "并发SET测试完成，耗时: ${duration}ms"
    echo "QPS: $((($CONCURRENT_REQUESTS * 10 * 1000) / $duration))"
}

function test_concurrent_get() {
    echo "测试并发GET操作..."
    local start_time=$(date +%s%N)
    
    local pids=()
    
    for i in $(seq 1 $CONCURRENT_REQUESTS); do
        (
            for j in $(seq 1 10); do
                local key="perf-key-$(shuf -i 1-$MAX_ITER -n 1)"
                curl -s -o /dev/null -w "%{http_code}" \
                    $(get_cs)/$key > /dev/null
            done
        ) &
        pids+=($!)
    done
    
    for pid in "${pids[@]}"; do
        wait $pid
    done
    
    local end_time=$(date +%s%N)
    local duration=$((($end_time - $start_time) / 1000000))
    echo "并发GET测试完成，耗时: ${duration}ms"
    echo "QPS: $((($CONCURRENT_REQUESTS * 10 * 1000) / $duration))"
}

function test_mixed_operations() {
    echo "测试混合操作..."
    local start_time=$(date +%s%N)
    
    local pids=()
    
    for i in $(seq 1 $CONCURRENT_REQUESTS); do
        (
            for j in $(seq 1 5); do
                # 随机选择操作类型
                local op=$(shuf -i 1-3 -n 1)
                case $op in
                    1) # SET
                        local key_idx=$(shuf -i 1-$MAX_ITER -n 1)
                        curl -s -o /dev/null -w "%{http_code}" \
                            -XPOST -H "Content-type: application/json" \
                            -d "$(gen_json_with_idx $key_idx)" \
                            $(get_cs) > /dev/null
                        ;;
                    2) # GET
                        local key="perf-key-$(shuf -i 1-$MAX_ITER -n 1)"
                        curl -s -o /dev/null -w "%{http_code}" \
                            $(get_cs)/$key > /dev/null
                        ;;
                    3) # DELETE
                        local key="perf-key-$(shuf -i 1-$MAX_ITER -n 1)"
                        curl -s -o /dev/null -w "%{http_code}" \
                            -XDELETE $(get_cs)/$key > /dev/null
                        ;;
                esac
            done
        ) &
        pids+=($!)
    done
    
    for pid in "${pids[@]}"; do
        wait $pid
    done
    
    local end_time=$(date +%s%N)
    local duration=$((($end_time - $start_time) / 1000000))
    echo "混合操作测试完成，耗时: ${duration}ms"
    echo "QPS: $((($CONCURRENT_REQUESTS * 5 * 1000) / $duration))"
}

function get_system_stats() {
    echo "获取系统统计信息..."
    for i in $(seq 1 $cs_num); do
        local port=$(($PORT_BASE + $i))
        echo "=== 节点 $i (端口 $port) 统计 ==="
        curl -s http://$HOST_BASE:$port/stats | jq '.' 2>/dev/null || echo "无法获取统计信息"
        echo
    done
}

function test_rate_limiting() {
    echo "测试限流功能..."
    local port=$(($PORT_BASE + 1))
    local success_count=0
    local rate_limit_count=0
    
    # 快速发送大量请求
    for i in $(seq 1 200); do
        local response=$(curl -s -w "%{http_code}" -o /dev/null \
            -XPOST -H "Content-type: application/json" \
            -d '{"test":"value"}' \
            http://$HOST_BASE:$port)
        
        if [[ $response == "200" ]]; then
            ((success_count++))
        elif [[ $response == "429" ]]; then
            ((rate_limit_count++))
        fi
    done
    
    echo "成功请求: $success_count"
    echo "限流请求: $rate_limit_count"
    if [[ $rate_limit_count -gt 0 ]]; then
        echo -e "限流功能: ${PASS_PROMPT}"
    else
        echo -e "限流功能: ${FAIL_PROMPT}"
    fi
}

# 主测试流程
echo "开始性能测试..."
echo "测试参数:"
echo "  服务器数量: $cs_num"
echo "  最大迭代数: $MAX_ITER"
echo "  并发请求数: $CONCURRENT_REQUESTS"
echo "  基础端口: $PORT_BASE"
echo

# 等待服务启动
echo "等待服务启动..."
sleep 5

# 运行测试
test_concurrent_set
echo

test_concurrent_get
echo

test_mixed_operations
echo

test_rate_limiting
echo

get_system_stats

echo "性能测试完成！"
