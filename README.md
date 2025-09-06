# 简单分布式缓存系统 (SDCS)

## 项目简介

这是一个基于C++实现的简易分布式缓存系统，支持多节点部署和数据分片存储。系统采用一致性哈希算法进行数据分布，通过HTTP协议提供REST API接口，节点间通过内部RPC进行通信。

## 系统特性

- **分布式存储**: 数据通过一致性哈希算法分布在多个节点
- **HTTP接口**: 提供标准的REST API (GET/POST/DELETE)
- **内部RPC**: 节点间自动路由和数据同步
- **容器化部署**: 基于Docker和Docker Compose
- **高可用性**: 支持3个或更多节点部署
- **跨平台支持**: Windows 11、Ubuntu 20.04+、macOS

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

## 🚀 快速开始

### 前置要求
- Docker
- Docker Compose

### 启动服务

#### Windows 11
```cmd
# 使用批处理脚本
build_and_run.bat

# 或使用PowerShell
powershell -ExecutionPolicy Bypass -File build_and_run.ps1

# 或手动执行
docker-compose up --build -d
```

#### Linux/macOS
```bash
# 使用脚本
chmod +x build_and_run.sh
./build_and_run.sh

# 或手动执行
docker-compose up --build -d
```

### 停止服务
```bash
docker-compose down
```

## 🧪 测试

### 自动化测试

#### Windows
```cmd
# 批处理版本
test.bat

# PowerShell版本
powershell -ExecutionPolicy Bypass -File test.ps1
```

#### Linux/macOS
```bash
# 基础测试
chmod +x test.sh
./test.sh

# 压力测试
chmod +x test_stress.sh
./test_stress.sh 3
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

## 📋 平台特定说明

### Windows 11

#### 环境准备
1. 安装 **Docker Desktop for Windows**
   - 下载地址：https://www.docker.com/products/docker-desktop/
2. 确保Docker Desktop正在运行
3. 可选：安装Windows Terminal以获得更好的体验

#### 常见问题
- **PowerShell执行策略**: 运行 `Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser`
- **端口占用**: 使用 `netstat -ano | findstr :9527` 检查端口
- **编码问题**: 在cmd中运行 `chcp 65001` 设置UTF-8编码

### Ubuntu 20.04+

#### 环境准备
```bash
# 更新系统
sudo apt update && sudo apt upgrade -y

# 安装依赖
sudo apt install -y git curl wget build-essential make g++ pkg-config jq lsof

# 安装Docker
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh
sudo usermod -aG docker $USER

# 安装Docker Compose
sudo curl -L "https://github.com/docker/compose/releases/download/v2.20.2/docker-compose-$(uname -s)-$(uname -m)" -o /usr/local/bin/docker-compose
sudo chmod +x /usr/local/bin/docker-compose

# 重新登录以应用Docker组权限
newgrp docker
```

#### 故障排除
```bash
# Docker权限问题
sudo usermod -aG docker $USER
newgrp docker

# 端口占用检查
sudo netstat -tulpn | grep -E "9527|9528|9529"

# 查看容器日志
docker-compose logs -f
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
├── Dockerfile            # Docker构建文件
├── docker-compose.yaml   # Docker Compose配置
├── Makefile             # 编译脚本
├── build_and_run.bat    # Windows构建脚本
├── build_and_run.ps1    # PowerShell构建脚本
├── build_and_run.sh     # Linux/macOS构建脚本
├── test.ps1             # PowerShell测试脚本
├── test_simple.ps1      # 简化PowerShell测试脚本
├── test.sh              # Linux/macOS测试脚本
├── test_stress.sh       # Linux压力测试脚本
├── .github/workflows/   # CI/CD配置
└── README.md            # 项目文档
```

## 开发说明

### 本地编译

#### Windows (Visual Studio)
```cmd
cl /EHsc main.cpp /I. /Fe:cache_server.exe
cache_server.exe 9527
```

#### Linux/macOS
```bash
# 安装nlohmann-json库
sudo apt install nlohmann-json3-dev  # Ubuntu/Debian
# 或
brew install nlohmann-json           # macOS

# 编译
g++ -std=c++17 -pthread -o cache_server main.cpp
./cache_server 9527
```

### 清理
```bash
# 清理编译文件
make clean

# 清理Docker
docker-compose down --rmi all --volumes
```

## 📊 性能监控

### 查看容器状态
```bash
# 查看运行中的容器
docker-compose ps

# 查看容器日志
docker-compose logs -f

# 查看资源使用
docker stats
```

### API性能测试
```bash
# 使用curl测试响应时间
curl -w "@curl-format.txt" -o /dev/null http://localhost:9527/health

# curl-format.txt内容：
#      time_total:  %{time_total}\n
#   time_connect:  %{time_connect}\n
#time_starttransfer:  %{time_starttransfer}\n
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

## CI/CD

项目包含GitHub Actions配置，支持自动化构建和测试：

- 自动构建Docker镜像
- 运行功能测试
- 跨平台兼容性检查

## 许可证

本项目仅用于学习和研究目的。

---

## 🆘 获取帮助

如果遇到问题：

1. 检查Docker是否正常运行
2. 确认端口9527-9529未被占用
3. 查看容器日志：`docker-compose logs`
4. 检查防火墙设置
5. 确保系统有足够的内存和磁盘空间

**技术支持**: 请查看项目Issues或提交新的Issue。