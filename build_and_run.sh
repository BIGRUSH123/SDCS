#!/bin/bash

# 分布式缓存系统 - Linux 构建和运行脚本

set -e  # 遇到错误立即退出

echo "========================================"
echo "        分布式缓存系统 (Linux)"
echo "========================================"

# 检查依赖
echo "1. 检查系统依赖..."

# 检查Docker
if ! command -v docker &> /dev/null; then
    echo "❌ Docker未安装，请先安装Docker"
    echo "运行: curl -fsSL https://get.docker.com -o get-docker.sh && sudo sh get-docker.sh"
    exit 1
fi

# 检查Docker Compose
if ! command -v docker-compose &> /dev/null; then
    echo "❌ Docker Compose未安装，请先安装"
    echo "运行安装命令请参考README_UBUNTU.md"
    exit 1
fi

echo "✅ Docker和Docker Compose已安装"

# 检查端口
echo "2. 检查端口占用..."
for port in 9527 9528 9529; do
    if lsof -i:$port &> /dev/null; then
        echo "⚠️  端口 $port 被占用，尝试停止相关进程..."
        sudo pkill -f "cache_server.*$port" || true
    fi
done

echo "✅ 端口检查完成"

# 停止可能运行的容器
echo "3. 停止现有容器..."
docker-compose down &> /dev/null || true
echo "✅ 容器清理完成"

# 构建和启动
echo "4. 构建并启动分布式缓存系统..."
docker-compose up --build -d

# 等待服务启动
echo "5. 等待所有服务完全启动..."
sleep 30

# 检查服务状态
echo "6. 检查服务状态..."
for port in 9527 9528 9529; do
    echo "检查节点 $port..."
    if curl -s -f "http://localhost:$port/health" >/dev/null 2>&1; then
        echo "✅ 缓存节点 $port 运行正常"
    else
        echo "⚠️  缓存节点 $port 可能还在启动中"
        echo "查看日志: docker-compose logs cache-server-$((port-9526))"
    fi
done

echo "========================================"
echo "🎉 分布式缓存系统启动完成！"
echo ""
echo "📊 服务信息："
echo "   - 节点1: http://localhost:9527"
echo "   - 节点2: http://localhost:9528"
echo "   - 节点3: http://localhost:9529"
echo ""
echo "🧪 运行测试："
echo "   基础测试: ./test_stress.sh 3"
echo "   查看日志: docker-compose logs -f"
echo "   停止服务: docker-compose down"
echo "========================================"