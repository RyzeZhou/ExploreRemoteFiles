# Explorer RemoteFS

> **把 WinSCP 的远程文件浏览、远程语义、远程打开/编辑和高效传输能力，无缝嵌入 Windows Explorer。**

更技术化的定义：

> **A native Windows Shell namespace for WSL, SFTP and FTP, backed by a high-performance cross-filesystem transfer engine.**

一句话：**不替代 Explorer，而是扩展 Explorer。** 用户继续使用 Windows 自带资源管理器，在左侧导航中直接访问 Windows 本地、WSL / WSL2、FTP/FTPS、SFTP/SSH 远程服务器，并保留传统高信息密度、键鼠优先的操作习惯。

---

## 目标 / 非目标

| | 说明 |
|---|---|
| ✅ 目标 | Explorer 内原生浏览远程 Linux 文件系统（SFTP / FTP / WSL），保留 POSIX 语义（rwx / owner / group / symlink），提供权限修改、Working Copy 远程编辑、WinSCP 级传输 |
| ❌ 非目标 | 开发新的文件管理器（不做 Files 类前端）；不把 FTP/SFTP 映射成虚拟盘（不做 WebDAV 盘、不伪装 NTFS）；不重写完整 SFTP/FTP 协议栈（第一阶段以 WinSCP 为 Transfer Backend） |

## 核心产品理念

- **统一的是操作体验，不统一文件系统语义**：Windows 目录显示 NTFS 语义（Type/Size/Date modified），远程目录显示 POSIX 语义（Permissions/Owner/Group/Modified）。
- **Explorer 只表达"用户想做什么"，Transfer Engine 决定"数据怎样最快完成"**：浏览与传输严格解耦。
- **远程文件是一等公民**：Ctrl+C / Ctrl+V / F2 / Delete / Alt+Enter / 双击 / 拖放全部可用，用户不应意识到自己在"使用 FTP 客户端"。

## 目标体验

```text
Explorer 左侧导航：

Desktop / Documents / Downloads / This PC
├─ WSL
│   ├─ Ubuntu
│   └─ Debian
└─ Servers
    ├─ prod
    ├─ staging
    └─ NAS
```

进入 `Servers > prod`，Details 视图切换为 Linux 语义：

```text
Name          Permissions   Owner      Group       Size       Modified
www           drwxr-xr-x    www-data   www-data               ...
deploy.sh     -rwxr-xr-x    deploy     dev         8 KB       ...
nginx.conf    -rw-r--r--    root       root        12 KB      ...
current       lrwxrwxrwx    root       root                   ...
```

## 目录结构

```text
explorer-remote-fs/
├── README.md                       # 项目章程（本文件）
├── docs/
│   ├── EXPLORATION_PLAN.md         # 技术探索计划（架构/风险/原型/任务清单，核心文档）
│   └── RESEARCH_LOG.md             # 探索日志与任务状态追踪（随进度更新）
├── src/
│   ├── ExplorerRemoteFs/           # Shell Namespace 插件（.NET 8 + SharpShell + comhost）
│   └── ExplorerRemoteFs.Cli/       # 冒烟测试 CLI（不依赖 Explorer 验证 Provider 层）
├── scripts/
│   ├── register.ps1                # 注册插件（管理员，需重启 Explorer）
│   ├── unregister.ps1              # 卸载插件（管理员）
│   └── add-connection.ps1          # 添加 FTP/SFTP 连接（无需管理员）
├── reference/
│   ├── explorer_remote_filesystem_integration_summary.md   # 原始探索概要（备查）
│   └── sharpshell-src/             # SharpShell 源码（API 参考）
└── research/                       # 探索产出：验证笔记、测试服务器、API 探测工具
```

## 快速开始（Prototype 1）

```bat
:: 1. 编译
dotnet build src/ExplorerRemoteFs/ExplorerRemoteFs.csproj

:: 2. 添加一个连接（示例；也可直接编辑 %APPDATA%\ExplorerRemoteFs\connections.json）
powershell -ExecutionPolicy Bypass -File scripts\add-connection.ps1 ^
    -Name prod -Type sftp -Host 1.2.3.4 -User deploy -Pass 'xxx'

:: 3. 用 CLI 验证连接（不需要 Explorer）
dotnet run --project src/ExplorerRemoteFs.Cli -- list prod /

:: 4. 注册插件（需要管理员；会写入注册表）
powershell -ExecutionPolicy Bypass -File scripts\register.ps1

:: 5. 重启 Explorer 后，左侧"此电脑"下出现 Servers → 点击展开连接
taskkill /f /im explorer.exe & start explorer.exe
```

## 技术栈（选型依据见 RESEARCH_LOG §3）

| 组件 | 方案 | 许可 | 职责 |
|---|---|---|---|
| Shell 集成层 | **SharpShell**（SharpNamespaceExtension） | MIT | IShellFolder2 桥接、导航挂载、自定义列 |
| SFTP 协议层 | **SSH.NET** 2025.1.0 | MIT | SFTP 连接、POSIX 元数据 |
| FTP/FTPS 协议层 | **FluentFTP** 53.x | MIT | FTP/FTPS 连接、能力降级 |
| 宿主 | .NET 8 + EnableComHosting（.comhost.dll） | — | 原生 COM 注册，规避旧 .NET 加载冲突 |

## 技术路线（详见 EXPLORATION_PLAN.md）

```text
✅ Prototype 1：SFTP/FTP Namespace（Provider 层已实测；Explorer 端待注册验证）
  → Prototype 2-5（Permissions / 双向传输 / Remote Open）
  → 第二阶段（WSL / Preview / 进程隔离 / 远端到远端）
  → 长期产品形态（Explorer 一等公民 + 专业 Transfer Engine）
```

## 参考项目（详见 EXPLORATION_PLAN.md §9）

| 项目 | 定位 | 对本项目的价值 |
|---|---|---|
| Swish | Explorer Shell Namespace + SFTP 浏览 | Namespace 前端参考（不作为产品底座） |
| WinSCP DragExt | Explorer 拖放截获 → WinSCP 传输 | **最重要的架构先例**：Explorer 操作可被 Shell Extension 截获并转交自有引擎 |
| WinSCP Core | 成熟 SFTP/FTP/SCP/WebDAV/S3 传输栈 | Transfer Backend 参考 |
| Windows FTP Folder | 历史 Shell Namespace 实现 | 证明"网络数据源作为 Explorer 原生位置"是 Shell 设计能力之一 |

## 成功判据（最低标准）

```text
1. Win+E → 点击 Servers > prod
2. Explorer 中直接看到 /var/www
3. Details 显示 rwx / owner / group
4. 右键 Properties 可以 chmod
5. 从 C:\ 拖入 10 GB 文件
6. 后台以接近 WinSCP 的速度上传
7. Explorer 不冻结
8. 双击远端配置文件 → 本地默认编辑器打开
9. 保存后自动同步回服务器
```

> 本项目的创新不是发明新协议，也不是重写 Explorer，而是**把已经成熟的能力（Explorer UI + Swish 概念 + WinSCP 引擎 + WSL）用正确的系统边界自然组合起来**。

---

*首次创建：2026-08-20 · 来源：桌面探索概要 `explorer_remote_filesystem_integration_summary.md`（已归档至 `reference/`）*
