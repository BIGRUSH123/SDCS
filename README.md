# 简单分布式缓存系统 (SDCS)

## 项目简介

这是一个基于C++实现的简易分布式缓存系统，支持多节点部署和数据分片存储。系统采用一致性哈希算法进行数据分布，通过HTTP协议提供REST API接口，节点间通过内部RPC进行通信。

## 系统特性

- **分布式存储**: 数据通过一致性哈希算法分布在多个节点
- **HTTP接口**: 提供标准的REST API (GET/POST/DELETE)
- **内部RPC**: 节点间自动路由和数据同步
- **容器化部署**: 基于Docker和Docker Compose
- **高可用性**: 支持3个或更多节点部署

## 系统架构

```
Client
  ↓ HTTP Request
[Node 1] ←→ [Node 2] ←→ [Node 3]
  ↑           ↑           ↑
  内部RPC通信 (基于HTTP)
```

### 核心组件

1. **ConsistentHash**: 一致性哈希实现，负责数据分片
2. **CacheNode**: 缓存节点实现，包含HTTP服务器和本地存储
3. **内部RPC**: 基于HTTP的节点间通信协议

## API 接口

### 1. 写入/更新缓存
```bash
POST /
Content-Type: application/json

# 示例
curl -X POST -H "Content-type: application/json" http://127.0.0.1:9527/ -d '{"myname": "电子科技大学@2023"}'
```

### 2. 读取缓存
```bash
GET /{key}

# 示例
curl http://127.0.0.1:9527/myname
# 返回: {"myname": "电子科技大学@2023"}
```

### 3. 删除缓存
```bash
DELETE /{key}

# 示例
curl -X DELETE http://127.0.0.1:9527/myname
# 返回: 1 (删除成功) 或 0 (键不存在)
```

### 4. 健康检查
```bash
GET /health

# 示例
curl http://127.0.0.1:9527/health
# 返回: {"status":"ok","node":"node9527"}
```

## 快速开始

### 前置要求
- Docker
- Docker Compose

### 启动服务
```bash
# 构建并启动所有缓存节点
docker-compose up --build

# 后台运行
docker-compose up --build -d
```

### 停止服务
```bash
docker-compose down
```

## 测试

### 自动化测试
```bash
# 给测试脚本执行权限
chmod +x test.sh

# 运行测试
./test.sh
```

### 手动测试
```bash
# 1. 写入数据
curl -X POST -H "Content-type: application/json" http://127.0.0.1:9527/ -d '{"name": "test"}'

# 2. 读取数据（可以从任意节点读取）
curl http://127.0.0.1:9528/name

# 3. 删除数据
curl -X DELETE http://127.0.0.1:9529/name
```

## 技术实现

### 数据分布策略
- 使用一致性哈希算法
- 每个节点创建150个虚拟节点以实现负载均衡
- 支持节点的动态扩容（理论上）

### 通信协议
- **客户端接口**: HTTP REST API
- **内部通信**: HTTP-based RPC
- **数据格式**: JSON

### 错误处理
- HTTP 200: 操作成功
- HTTP 404: 键不存在
- HTTP 400: 请求格式错误
- HTTP 500: 内部服务器错误

## 项目结构

```
.
├── main.cpp              # 主程序代码
├── httplib.h             # 简化的HTTP库实现
├── json.hpp              # 简化的JSON库实现
├── Dockerfile            # Docker构建文件
├── docker-compose.yaml   # Docker Compose配置
├── Makefile             # 编译脚本
├── test.sh              # 测试脚本
└── README.md            # 项目文档
```

## 开发说明

### 本地编译
```bash
# 编译
make

# 运行单个节点
./cache_server 9527
```

### 清理
```bash
# 清理编译文件
make clean

# 清理Docker
docker-compose down --rmi all
```

## 系统限制

1. **内存存储**: 数据仅存储在内存中，重启后丢失
2. **无持久化**: 不支持数据持久化到磁盘
3. **无副本**: 每个数据只存储一份，无冗余备份
4. **静态节点**: 不支持运行时动态添加/删除节点

## 扩展方向

1. 添加数据持久化功能
2. 实现数据副本和故障恢复
3. 支持动态节点管理
4. 添加监控和日志功能
5. 实现更高效的网络协议（如gRPC）

## 许可证

本项目仅用于学习和研究目的。
