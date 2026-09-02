ExplorerRemoteFs — FTP/SFTP 资源管理器集成（NSE）
====================================================

把 FTP/SFTP 站点变成 Windows 资源管理器导航栏的一级入口：
浏览、WinSCP 级右键菜单、权限/chown 写回、属性对话框、地址栏 site:/path 直达。

目录结构
--------
  ExplorerDataProviderFtp.dll    Shell 扩展（COM，x64）
  cli\                           CLI 桥（Explorer 调用它做所有远程操作，含 .NET 8 runtime）
  client\                        GUI 站点管理器（RemoteFsClient.exe，含 .NET 8 runtime）
  install.ps1                    安装（当前用户，无需管理员）
  uninstall.ps1                  卸载（当前用户）

安装
----
1. 解压整个文件夹（路径随意，但不要放在临时目录后删除）。
2. 右键 install.ps1 -> "使用 PowerShell 运行"
   （或：powershell -ExecutionPolicy Bypass -File install.ps1）
3. 安装脚本会：复制文件到 %LOCALAPPDATA%\ExplorerRemoteFs、
   注册 Shell 扩展（导航栏顶级 "FTP" 入口）、自动重启 Explorer。

添加站点
--------
方式 A（推荐）：运行 client\RemoteFsClient.exe（GUI 站点管理器），
   添加站点（名称/协议/主机/端口/用户/密码——密码存 Windows 凭据管理器，
   不落盘明文）。
方式 B（手动）：编辑 %APPDATA%\ExplorerRemoteFs\connections.json：
   [ { "Name": "WSL-SFTP", "Type": "sftp", "Host": "192.168.200.1",
       "Port": 2223, "Username": "zhou", "StartPath": "/" } ]
   密码留空则从凭据管理器读（ExplorerRemoteFs/<站点名>）。

使用
----
- 左侧导航栏出现 "FTP"（与"此电脑"平级）-> 展开 -> 站点 -> 远程目录。
- 地址栏：WSL-SFTP:/home/user（可输入直达）。
- 目录文件右键：Open / Edit(改完自动回传) / Download / 剪贴板复制 /
  Duplicate / Move to / Rename / Delete / 自定义命令 / Remote properties(权限+chown)。
- 站点项右键：系统默认菜单 + 属性(连接信息页)；空白处右键：New site...。
- 自定义命令：%APPDATA%\ExplorerRemoteFs\custom-commands.json。

卸载
----
右键 uninstall.ps1 -> "使用 PowerShell 运行"
  （加 -RemoveConfig 会同时删除站点配置；凭据管理器里的密码需自行清理：
   Control Panel -> Credential Manager -> Windows Credentials ->
   删除 ExplorerRemoteFs/* 条目。）

已知限制
--------
- Win10：Ribbon 通过官方 opt-in 显示；Win11：显示命令栏（系统行为）。
- FTP 协议无数字 uid/gid（UID/GID 列空）；chown 仅 SFTP 支持。
- 跨用户 chown 受 Linux 权限限制（需 root，WinSCP 同样）。
- 远程文件属性走右键"Remote properties"（Ribbon 内置属性按钮已重定向到同款页）。
- Win11：第三方右键项在“显示更多选项”中；删除使用“Delete from server”，属性使用“Remote properties”。

开发/日志
--------
诊断日志：C:\temp\remotefs-debug.log（ProbeLog）。

WinSCP 绿色版（便携版）支持
--------------------------
- "测试连接"与自定义命令(type=script)需要 WinSCP.com。查找顺序：
  1) GUI 站点管理器点"WinSCP 设置"按钮，手动选择绿色版解压目录里的 WinSCP.com
     （路径存入注册表 HKCU\Software\ExplorerRemoteFs\WinScpPath）；
  2) 常见安装路径（Program Files）；
  3) PATH。
- 找不到时提示信息会说明如何指定，不再只报"请先安装"。

Win11: installation only pins the navigation pane; it does not modify Desktop\NameSpace or system desktop-icon settings.

Explorer 扩展翻译（额外语言）
----------------------------
- 简体中文（zh-CN）和英语（en-US）内置在 DLL 中，YAML 不会覆盖这两种语言。
- 安装后，模板位于 %APPDATA%\ExplorerRemoteFs\explorer-translations.yaml；同目录的 explorer-translations.example.yaml 提供完整的中英对照；升级不会覆盖已编辑文件。
- 要添加语言，在 YAML 中增加语言代码段，例如：
    ja-JP:
      column.name: "名前"
      menu.open: "開く"
      property.permissions: "アクセス許可"
  保存后在服务程序“设置 -> Explorer 扩展显示语言”选择 ja-JP，并重启 Explorer。
- YAML 未提供的键会回退到英语；文件顶部注释列出了当前全部可翻译键。