@echo off
chcp 65001

REM 设置进入正常模式

echo 设置进入正常模式，系统将重新启动以生效。

echo 如需再次进入测试模式，请执行set-test-model.bat

bcdedit /set testsigning off

echo 设置完成，准备重启
pause

shutdown /r /t 0
