@echo off
chcp 65001

REM 设置进入测试模式

echo 设置进入测试模式，系统将重新启动以生效。

echo 如需退出测试模式，请执行set-normal-model.bat

bcdedit /set testsigning on

echo 设置完成，准备重启
pause

shutdown /r /t 0