# Windows 11 环境使用指南

本文档专门为Windows 11用户提供分布式缓存系统的安装、配置和测试指南。

## 🚀 快速开始

### 前置要求

1. **Windows 11** 操作系统
2. **Docker Desktop for Windows** (推荐最新版本)
   - 下载地址：https://www.docker.com/products/docker-desktop/
3. **Windows终端** (可选，但推荐)
   - 从Microsoft Store安装Windows Terminal

### 🔧 环境准备

#### 1. 安装Docker Desktop

1. 下载并安装Docker Desktop for Windows
2. 启动Docker Desktop
3. 确保Docker Desktop正在运行（系统托盘中有Docker图标）

#### 2. 验证Docker安装

打开命令提示符或PowerShell，运行：

```cmd
# 检查Docker版本
docker --version

# 检查Docker Compose版本
docker compose version
# 或
docker-compose --version
```

## 📋 可用的测试脚本

我们为Windows 11用户提供了多种测试脚本：

### 1. 批处理脚本 (.bat)

- **`build_and_run.bat`** - 构建和启动服务
- **`test.bat`** - 运行完整测试套件

#### 使用方法：
```cmd
# 构建和启动服务
build_and_run.bat

# 或者分步执行
docker compose up --build -d
test.bat
```

### 2. PowerShell脚本 (.ps1)

- **`build_and_run.ps1`** - 构建和启动服务（PowerShell版本）
- **`test.ps1`** - 运行完整测试套件（PowerShell版本）

#### 使用方法：
```powershell
# 可能需要设置执行策略
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# 构建和启动服务
.\build_and_run.ps1

# 或者分步执行
docker compose up --build -d
.\test.ps1
```

## 🧪 测试功能

所有测试脚本都会执行以下测试：

1. **健康检查测试** - 验证所有节点是否正常运行
2. **写入操作测试** - 向不同节点写入数据
3. **跨节点读取测试** - 验证数据路由和分布式存储
4. **删除操作测试** - 测试数据删除功能
5. **错误处理测试** - 测试404和其他错误情况

### 测试数据示例

脚本会自动测试以下数据：

```json
{"myname": "电子科技大学@2023"}
{"tasks": ["task 1", "task 2", "task 3"]}
{"age": 123}
```

## 🔍 手动测试

如果你想手动测试API，可以使用以下命令：

### 使用 curl（Windows 11内置）

```cmd
# 写入数据
curl -X POST -H "Content-Type: application/json" http://127.0.0.1:9527/ -d "{\"key\": \"value\"}"

# 读取数据
curl http://127.0.0.1:9528/key

# 删除数据
curl -X DELETE http://127.0.0.1:9529/key

# 健康检查
curl http://127.0.0.1:9527/health
```

### 使用 PowerShell

```powershell
# 写入数据
$headers = @{"Content-Type" = "application/json"}
$body = @{"key" = "value"} | ConvertTo-Json
Invoke-RestMethod -Uri "http://127.0.0.1:9527/" -Method Post -Headers $headers -Body $body

# 读取数据
Invoke-RestMethod -Uri "http://127.0.0.1:9528/key" -Method Get

# 删除数据
Invoke-RestMethod -Uri "http://127.0.0.1:9529/key" -Method Delete

# 健康检查
Invoke-RestMethod -Uri "http://127.0.0.1:9527/health" -Method Get
```

## 🐛 常见问题与解决方案

### 1. Docker相关问题

**问题**: "Docker未安装"错误
**解决方案**: 
- 确保Docker Desktop已安装并运行
- 重启Docker Desktop
- 检查Windows功能中是否启用了Hyper-V和容器功能

**问题**: "Docker服务未运行"
**解决方案**: 
- 启动Docker Desktop应用程序
- 等待Docker完全启动（系统托盘图标变为正常状态）

### 2. 端口占用问题

**问题**: 端口9527、9528、9529被占用
**解决方案**: 
```cmd
# 查看端口占用
netstat -ano | findstr :9527

# 停止现有服务
docker compose down
```

### 3. PowerShell执行策略问题

**问题**: "无法加载文件，因为在此系统上禁止运行脚本"
**解决方案**: 
```powershell
# 临时允许脚本执行
Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process

# 或永久设置（仅当前用户）
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

### 4. 编码问题

**问题**: 中文字符显示乱码
**解决方案**: 
- 确保使用UTF-8编码
- 在cmd中运行 `chcp 65001`
- 使用Windows Terminal而不是传统命令提示符

## 📊 性能监控

### 查看容器状态
```cmd
# 查看运行中的容器
docker compose ps

# 查看容器日志
docker compose logs

# 查看特定容器日志
docker compose logs cache-server-1
```

### 资源使用情况
```cmd
# 查看容器资源使用
docker stats
```

## 🔧 开发者选项

### 本地编译（如果需要）

如果你有Visual Studio或其他C++编译器：

```cmd
# 使用Visual Studio Developer Command Prompt
cl /EHsc main.cpp /I. /Fe:cache_server.exe

# 运行单个节点
cache_server.exe 9527
```

### 调试模式

```cmd
# 以前台模式运行（可以看到详细日志）
docker compose up --build

# 进入容器进行调试
docker compose exec cache-server-1 /bin/bash
```

## 🚪 停止服务

```cmd
# 停止所有服务
docker compose down

# 停止服务并删除镜像
docker compose down --rmi all

# 停止服务并清理所有数据
docker compose down --volumes --remove-orphans
```

## 📞 技术支持

如果遇到问题，请检查：

1. Docker Desktop是否正常运行
2. 防火墙是否阻止了端口访问
3. 系统是否有足够的内存和磁盘空间
4. Windows版本是否兼容（推荐Windows 11）

---

**注意**: 本系统仅用于学习和测试目的，不建议在生产环境中使用。
