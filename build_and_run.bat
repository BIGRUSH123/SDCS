@echo off
chcp 65001 >nul
echo === 简单分布式缓存系统 构建和运行脚本 (Windows 11) ===
echo.

REM 检查Docker是否安装
where docker >nul 2>&1
if %errorlevel% neq 0 (
    echo 错误: Docker未安装，请先安装Docker Desktop
    echo 下载地址: https://www.docker.com/products/docker-desktop/
    pause
    exit /b 1
)

REM 检查Docker是否运行
docker info >nul 2>&1
if %errorlevel% neq 0 (
    echo 错误: Docker服务未运行，请启动Docker Desktop
    pause
    exit /b 1
)

REM 检查Docker Compose是否可用
docker compose version >nul 2>&1
if %errorlevel% neq 0 (
    REM 尝试使用旧版本的docker-compose命令
    docker-compose --version >nul 2>&1
    if %errorlevel% neq 0 (
        echo 错误: Docker Compose未安装或不可用
        echo 请确保Docker Desktop已正确安装
        pause
        exit /b 1
    ) else (
        set COMPOSE_CMD=docker-compose
    )
) else (
    set COMPOSE_CMD=docker compose
)

echo 1. 清理之前的容器和镜像...
%COMPOSE_CMD% down --rmi all --volumes --remove-orphans 2>nul

echo 2. 构建和启动缓存服务...
%COMPOSE_CMD% up --build -d
if %errorlevel% neq 0 (
    echo 错误: 服务启动失败
    pause
    exit /b 1
)

echo 3. 等待服务启动...
timeout /t 10 /nobreak >nul

echo 4. 检查服务状态...
%COMPOSE_CMD% ps

echo 5. 等待所有服务完全启动...
timeout /t 5 /nobreak >nul

echo 6. 运行功能测试...
if exist "test.bat" (
    echo 运行批处理测试脚本...
    call test.bat
) else if exist "test.ps1" (
    echo 运行PowerShell测试脚本...
    powershell -ExecutionPolicy Bypass -File test.ps1
) else (
    echo 测试脚本不存在，手动测试示例：
    echo curl -X POST -H "Content-type: application/json" http://127.0.0.1:9527/ -d "{\"test\": \"hello\"}"
    echo curl http://127.0.0.1:9528/test
)

echo.
echo === 服务已启动完成 ===
echo 访问地址：
echo   - 节点1: http://127.0.0.1:9527
echo   - 节点2: http://127.0.0.1:9528
echo   - 节点3: http://127.0.0.1:9529
echo.
echo 停止服务命令: %COMPOSE_CMD% down
echo 查看日志命令: %COMPOSE_CMD% logs
echo.
pause
