# PowerShell 构建和运行脚本 - 适用于Windows 11
# 简单分布式缓存系统构建和运行

# 设置控制台编码为UTF-8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "=== 简单分布式缓存系统 构建和运行脚本 (Windows 11 PowerShell) ===" -ForegroundColor Green
Write-Host ""

# 检查Docker是否安装
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "错误: Docker未安装，请先安装Docker Desktop" -ForegroundColor Red
    Write-Host "下载地址: https://www.docker.com/products/docker-desktop/" -ForegroundColor Yellow
    Write-Host "按任意键退出..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    exit 1
}

# 检查Docker是否运行
try {
    docker info | Out-Null
} catch {
    Write-Host "错误: Docker服务未运行，请启动Docker Desktop" -ForegroundColor Red
    Write-Host "按任意键退出..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    exit 1
}

# 检查Docker Compose是否可用
$composeCmd = $null
if (Get-Command "docker" -ErrorAction SilentlyContinue) {
    try {
        docker compose version | Out-Null
        $composeCmd = "docker compose"
        Write-Host "使用 Docker Compose V2" -ForegroundColor Green
    } catch {
        # 尝试使用旧版本的docker-compose命令
        if (Get-Command "docker-compose" -ErrorAction SilentlyContinue) {
            try {
                docker-compose --version | Out-Null
                $composeCmd = "docker-compose"
                Write-Host "使用 Docker Compose V1" -ForegroundColor Yellow
            } catch {
                Write-Host "错误: Docker Compose未安装或不可用" -ForegroundColor Red
                Write-Host "请确保Docker Desktop已正确安装" -ForegroundColor Yellow
                Write-Host "按任意键退出..."
                $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
                exit 1
            }
        } else {
            Write-Host "错误: Docker Compose未安装或不可用" -ForegroundColor Red
            Write-Host "请确保Docker Desktop已正确安装" -ForegroundColor Yellow
            Write-Host "按任意键退出..."
            $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
            exit 1
        }
    }
}

Write-Host "1. 清理之前的容器和镜像..." -ForegroundColor Cyan
try {
    Invoke-Expression "$composeCmd down --rmi all --volumes --remove-orphans" 2>$null
} catch {
    Write-Host "清理过程中出现警告（可忽略）" -ForegroundColor Yellow
}

Write-Host "2. 构建和启动缓存服务..." -ForegroundColor Cyan
try {
    Invoke-Expression "$composeCmd up --build -d"
    if ($LASTEXITCODE -ne 0) {
        throw "服务启动失败"
    }
} catch {
    Write-Host "错误: 服务启动失败" -ForegroundColor Red
    Write-Host "按任意键退出..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    exit 1
}

Write-Host "3. 等待服务启动..." -ForegroundColor Cyan
Start-Sleep -Seconds 10

Write-Host "4. 检查服务状态..." -ForegroundColor Cyan
Invoke-Expression "$composeCmd ps"

Write-Host "5. 等待所有服务完全启动..." -ForegroundColor Cyan
Start-Sleep -Seconds 5

Write-Host "6. 运行功能测试..." -ForegroundColor Cyan
if (Test-Path "test.ps1") {
    Write-Host "运行PowerShell测试脚本..." -ForegroundColor Green
    & ".\test.ps1"
} elseif (Test-Path "test.bat") {
    Write-Host "运行批处理测试脚本..." -ForegroundColor Green
    & ".\test.bat"
} else {
    Write-Host "测试脚本不存在，手动测试示例：" -ForegroundColor Yellow
    Write-Host "Invoke-RestMethod -Uri 'http://127.0.0.1:9527/' -Method Post -Headers @{'Content-Type'='application/json'} -Body '{\"test\": \"hello\"}'" -ForegroundColor White
    Write-Host "Invoke-RestMethod -Uri 'http://127.0.0.1:9528/test' -Method Get" -ForegroundColor White
}

Write-Host ""
Write-Host "=== 服务已启动完成 ===" -ForegroundColor Green
Write-Host "访问地址：" -ForegroundColor Yellow
Write-Host "  - 节点1: http://127.0.0.1:9527" -ForegroundColor White
Write-Host "  - 节点2: http://127.0.0.1:9528" -ForegroundColor White
Write-Host "  - 节点3: http://127.0.0.1:9529" -ForegroundColor White
Write-Host ""
Write-Host "常用命令：" -ForegroundColor Yellow
Write-Host "  停止服务: $composeCmd down" -ForegroundColor White
Write-Host "  查看日志: $composeCmd logs" -ForegroundColor White
Write-Host "  查看状态: $composeCmd ps" -ForegroundColor White
Write-Host ""
Write-Host "按任意键退出..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
