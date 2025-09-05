#!/bin/bash

echo "=== 简单分布式缓存系统 构建和运行脚本 ==="
echo

# 检查Docker是否安装
if ! command -v docker &> /dev/null; then
    echo "错误: Docker未安装，请先安装Docker"
    exit 1
fi

# 检查Docker Compose是否安装
if ! command -v docker-compose &> /dev/null; then
    echo "错误: Docker Compose未安装，请先安装Docker Compose"
    exit 1
fi

echo "1. 清理之前的容器和镜像..."
docker-compose down --rmi all --volumes --remove-orphans 2>/dev/null || true

echo "2. 构建和启动缓存服务..."
docker-compose up --build -d

echo "3. 等待服务启动..."
sleep 10

echo "4. 检查服务状态..."
docker-compose ps

echo "5. 运行功能测试..."
if [ -f "test.sh" ]; then
    chmod +x test.sh
    ./test.sh
else
    echo "测试脚本不存在，手动测试："
    echo "curl -X POST -H 'Content-type: application/json' http://127.0.0.1:9527/ -d '{\"test\": \"hello\"}'"
    echo "curl http://127.0.0.1:9528/test"
fi

echo
echo "=== 服务已启动完成 ==="
echo "访问地址："
echo "  - 节点1: http://127.0.0.1:9527"
echo "  - 节点2: http://127.0.0.1:9528"  
echo "  - 节点3: http://127.0.0.1:9529"
echo
echo "停止服务命令: docker-compose down"
