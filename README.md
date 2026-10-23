### 开发参考
https://learn.microsoft.com/zh-cn/windows-hardware/drivers/gettingstarted/provision-a-target-computer
大致的安装步骤如下：
1. 安装 C:\Program Files (x86)\Windows Kits\10\Remote\x64\WDK Test Target Setup x64-x64_en-us.msi
2. 如果目标计算机正在运行 Windows Server，请查找刚刚由 WDK 测试目标安装程序 MSI 创建的 DriverTest 文件夹。 （示例：c：\DriverTest）。
   选择并按住 DriverTest 文件夹（或右键单击），然后选择 “属性”。 在“安全”选项卡上，向“经过身份验证的用户组”授予“修改”权限。

### 已知问题
1. 如果windows启用网络共享（非高级共享），会导致目录无法访问，引发驱动加载失败，这个问题暂时只能通过不启用网络共享来解决。

### 特别说明
1. windows 10 上内置的UMDF是1.4版本的，windows 11以上才内置2.0以上版本。

### 版本变更日志
#### 1.0.26.102301
1. 增加单独的显示器上下文，每个显示器采用单独的线程。
2. 修改配置文件的加载方式，现在从option.txt加载配置。
3. 调整了项目目录结构。
4. 正式命名为HdrVDD。
5. GetIddCx新增管线测试支持。
6. 新增IOHelper、Utils，并简化了edid读取逻辑到IOHelper。
7. 完善和优化了虚拟显示器的相关安装脚本。
8. 内置了nefconc.exe、nefconw.exe到脚本目录。
9. 增加版本控制文件version.h
10. 增加rc文件，用于dll的详细信息支持。
