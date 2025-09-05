# PowerShell 测试脚本 - 适用于Windows 11
# 简单分布式缓存系统测试

# 设置控制台编码为UTF-8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "=== 简单分布式缓存系统测试 (Windows 11 PowerShell) ===" -ForegroundColor Green
Write-Host ""

# 检查curl是否可用
if (-not (Get-Command curl -ErrorAction SilentlyContinue)) {
    Write-Host "错误: curl命令不可用。请确保安装了curl或使用Windows 10/11内置的curl。" -ForegroundColor Red
    exit 1
}

# 等待服务启动
Write-Host "等待服务启动..." -ForegroundColor Yellow
Start-Sleep -Seconds 5

Write-Host "1. 测试健康检查" -ForegroundColor Cyan
Write-Host "服务器1健康状态:"
try {
    $response1 = Invoke-RestMethod -Uri "http://127.0.0.1:9527/health" -Method Get -TimeoutSec 10
    $response1 | ConvertTo-Json -Depth 10
} catch {
    Write-Host "服务器1连接失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "服务器2健康状态:"
try {
    $response2 = Invoke-RestMethod -Uri "http://127.0.0.1:9528/health" -Method Get -TimeoutSec 10
    $response2 | ConvertTo-Json -Depth 10
} catch {
    Write-Host "服务器2连接失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "服务器3健康状态:"
try {
    $response3 = Invoke-RestMethod -Uri "http://127.0.0.1:9529/health" -Method Get -TimeoutSec 10
    $response3 | ConvertTo-Json -Depth 10
} catch {
    Write-Host "服务器3连接失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "2. 测试写入操作" -ForegroundColor Cyan
Write-Host "向服务器1写入 myname"
try {
    $headers = @{"Content-Type" = "application/json; charset=utf-8"}
    $body = @{"myname" = "电子科技大学@2023"} | ConvertTo-Json -Depth 10
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9527/" -Method Post -Headers $headers -Body $body -TimeoutSec 10
    Write-Host "响应: $response"
} catch {
    Write-Host "写入失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "向服务器2写入 tasks"
try {
    $headers = @{"Content-Type" = "application/json; charset=utf-8"}
    $body = @{"tasks" = @("task 1", "task 2", "task 3")} | ConvertTo-Json -Depth 10
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9528/" -Method Post -Headers $headers -Body $body -TimeoutSec 10
    Write-Host "响应: $response"
} catch {
    Write-Host "写入失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "向服务器3写入 age"
try {
    $headers = @{"Content-Type" = "application/json; charset=utf-8"}
    $body = @{"age" = 123} | ConvertTo-Json -Depth 10
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9529/" -Method Post -Headers $headers -Body $body -TimeoutSec 10
    Write-Host "响应: $response"
} catch {
    Write-Host "写入失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "3. 测试读取操作（跨节点）" -ForegroundColor Cyan
Write-Host "从服务器2读取 myname:"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9528/myname" -Method Get -TimeoutSec 10
    $response | ConvertTo-Json -Depth 10
} catch {
    Write-Host "读取失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "从服务器1读取 tasks:"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9527/tasks" -Method Get -TimeoutSec 10
    $response | ConvertTo-Json -Depth 10
} catch {
    Write-Host "读取失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "从服务器1读取 age:"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9527/age" -Method Get -TimeoutSec 10
    $response | ConvertTo-Json -Depth 10
} catch {
    Write-Host "读取失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "4. 测试读取不存在的键" -ForegroundColor Cyan
Write-Host "从服务器1读取 notexistkey:"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9527/notexistkey" -Method Get -TimeoutSec 10
    $response | ConvertTo-Json -Depth 10
} catch {
    if ($_.Exception.Response.StatusCode -eq 404) {
        Write-Host "预期的404错误 - 键不存在" -ForegroundColor Green
    } else {
        Write-Host "意外错误: $_" -ForegroundColor Red
    }
}
Write-Host ""

Write-Host "5. 测试删除操作" -ForegroundColor Cyan
Write-Host "从服务器3删除 myname:"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9529/myname" -Method Delete -TimeoutSec 10
    Write-Host "删除结果: $response"
} catch {
    Write-Host "删除失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "再次从服务器1读取 myname (应该返回404):"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9527/myname" -Method Get -TimeoutSec 10
    Write-Host "意外成功读取: $($response | ConvertTo-Json -Depth 10)" -ForegroundColor Yellow
} catch {
    if ($_.Exception.Response.StatusCode -eq 404) {
        Write-Host "预期的404错误 - 键已删除" -ForegroundColor Green
    } else {
        Write-Host "意外错误: $_" -ForegroundColor Red
    }
}
Write-Host ""

Write-Host "再次删除已删除的键:"
try {
    $response = Invoke-RestMethod -Uri "http://127.0.0.1:9529/myname" -Method Delete -TimeoutSec 10
    Write-Host "删除结果: $response"
} catch {
    Write-Host "删除失败: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "=== 测试完成 ===" -ForegroundColor Green
Write-Host "按任意键继续..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
