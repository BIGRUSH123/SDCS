@echo off
chcp 65001 >nul
echo ========================================
echo         分布式缓存系统 (Windows)
echo ========================================
echo.

REM 检查Docker
echo 1. 检查系统依赖...
where docker >nul 2>&1
if %errorlevel% neq 0 (
    echo ❌ Docker未安装，请先安装Docker Desktop
    echo 下载地址: https://www.docker.com/products/docker-desktop/
    pause
    exit /b 1
)

docker info >nul 2>&1
if %errorlevel% neq 0 (
    echo ❌ Docker服务未运行，请启动Docker Desktop
    pause
    exit /b 1
)

REM 检查Docker Compose
docker compose version >nul 2>&1
if %errorlevel% neq 0 (
    docker-compose --version >nul 2>&1
    if %errorlevel% neq 0 (
        echo ❌ Docker Compose不可用
        pause
        exit /b 1
    ) else (
        set COMPOSE_CMD=docker-compose
    )
) else (
    set COMPOSE_CMD=docker compose
)

echo ✅ Docker和Docker Compose已安装

echo 2. 停止现有容器...
%COMPOSE_CMD% down >nul 2>&1

echo 3. 构建并启动分布式缓存系统...
%COMPOSE_CMD% up --build -d
if %errorlevel% neq 0 (
    echo ❌ 服务启动失败
    pause
    exit /b 1
)

echo 4. 等待所有服务完全启动...
timeout /t 30 /nobreak >nul

echo 5. 检查服务状态...
setlocal enabledelayedexpansion
for %%p in (9527 9528 9529) do (
    echo 检查节点 %%p...
    curl -s -f http://localhost:%%p/health >nul 2>&1
    if !errorlevel! equ 0 (
        echo ✅ 缓存节点 %%p 运行正常
    ) else (
        echo ⚠️  缓存节点 %%p 可能还在启动中
        echo 查看日志: %COMPOSE_CMD% logs cache-server-%%p
    )
)

echo ========================================
echo 🎉 分布式缓存系统启动完成！
echo.
echo 📊 服务信息：
echo    - 节点1: http://localhost:9527
echo    - 节点2: http://localhost:9528
echo    - 节点3: http://localhost:9529
echo.
echo 🧪 运行测试：
echo    基础测试: powershell -ExecutionPolicy Bypass -File test_simple.ps1
echo    查看日志: %COMPOSE_CMD% logs -f
echo    停止服务: %COMPOSE_CMD% down
echo ========================================
pause