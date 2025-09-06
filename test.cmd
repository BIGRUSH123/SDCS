@echo off
chcp 65001 >nul
echo ========================================
echo         分布式缓存系统测试 (Windows)
echo ========================================
echo.

REM 等待服务启动
echo 等待服务启动...
timeout /t 5 /nobreak >nul

echo 1. 测试健康检查
echo 服务器1健康状态:
curl -s http://127.0.0.1:9527/health
echo.
echo 服务器2健康状态:
curl -s http://127.0.0.1:9528/health
echo.
echo 服务器3健康状态:
curl -s http://127.0.0.1:9529/health
echo.

echo 2. 测试写入操作
echo 向服务器1写入 myname
curl -s -X POST -H "Content-Type: application/json" -d "{\"myname\": \"电子科技大学@2023\"}" http://127.0.0.1:9527/
echo.
echo 向服务器2写入 tasks
curl -s -X POST -H "Content-Type: application/json" -d "{\"tasks\": [\"task 1\", \"task 2\", \"task 3\"]}" http://127.0.0.1:9528/
echo.
echo 向服务器3写入 age
curl -s -X POST -H "Content-Type: application/json" -d "{\"age\": 123}" http://127.0.0.1:9529/
echo.

echo 3. 测试读取操作
echo 从服务器2读取 myname:
curl -s http://127.0.0.1:9528/myname
echo.
echo 从服务器1读取 tasks:
curl -s http://127.0.0.1:9527/tasks
echo.
echo 从服务器1读取 age:
curl -s http://127.0.0.1:9527/age
echo.

echo 4. 测试读取不存在的键
echo 从服务器1读取 notexistkey:
curl -s http://127.0.0.1:9527/notexistkey
echo.

echo 5. 测试删除操作
echo 从服务器3删除 myname:
curl -s -X DELETE http://127.0.0.1:9529/myname
echo.
echo 再次读取已删除的键:
curl -s http://127.0.0.1:9527/myname
echo.
echo 再次删除已删除的键 (应该返回0):
curl -s -X DELETE http://127.0.0.1:9529/myname
echo.

echo ========================================
echo 测试完成
echo ========================================
pause
