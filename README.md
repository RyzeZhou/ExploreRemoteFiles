# ExploreRemoteFiles (ERF)

> **把 WinSCP 的远程浏览、远程语义、远程打开/编辑与高效传输能力，无缝嵌进 Windows 资源管理器。**
> 更技术化的定义：**A native Windows Shell namespace for SFTP / FTP, backed by a cross-filesystem transfer engine.**

一句话：**不替代 Explorer，而是扩展 Explorer。**

版本：**0.1-Alpha** ｜ 当前主线分支：`feature/microsoft-explorer-core`

## 为什么叫 ExploreRemoteFiles（不叫 ExplorerRemoteFiles）

1. 避免 `Explor**er**Remote` 两个连续 `R` 的拼写与读音黏连；
2. **`Explore` 既指 Explorer，也是动词"探索"**——远程只是第一条腿，
   后续要往 Explorer 的**本地增强能力**扩展（见路线）；
3. 缩写 **ERF** 由此成为产品前缀：地址 `erf://<site>:/path`、协议 `ERF-Proto`、
   服务端 `ERF-Server`、插件 `ERF-Shell`。详见 [docs/PROJECT_IDENTITY.md](docs/PROJECT_IDENTITY.md)。

## 核心理念

- **统一的是操作体验，不统一文件系统语义**：本地显示 NTFS 语义（Type/Size/Date modified），
  远程显示 POSIX 语义（Permissions/Owner/Group/Modified），并以服务端声明为准。
- **Explorer 只表达"用户想做什么"，引擎决定"数据怎样最快完成"**：浏览与传输解耦。
- **架构不变量**：Explorer 进程（插件 DLL）内**永不做同步网络 I/O**；网络与长任务在常驻客户端与 CLI 子进程里。
- **远程文件是一等公民**：Ctrl+C / Ctrl+V / F2 / Delete / Alt+Enter / 双击 / 拖放 都要可用。

## 现在的形态

```text
资源管理器（Explorer.exe）
  └─ ERF-Shell：C++ Shell Namespace 扩展        src-cpp/ExplorerDataProviderFtp/
       IShellFolder2 + 紧凑身份 PIDL + ITransferSource + IContextMenu + Details 列
       只读内存缓存 / 命名管道；冷数据一律后台预取，绝不在 UI 线程等待
       ▼
常驻客户端 RemoteFsClient（WPF）                src-client/
       凭据管理 · 命名管道桥接 · 任务与进度窗口（即将升格为「远程操作队列」）
       ▼
CLI 子进程 ExplorerRemoteFs.Cli                 src/ExplorerRemoteFs.Cli/
       一次性网络动作：list / get / put / getr / chmod / touch / rm …
       ▼
Provider 层                                     src/ExplorerRemoteFs/Providers/
       SftpFileSystem（SSH.NET） · FtpFileSystem（FluentFTP）
```

## 0.1-Alpha 已实测通过

浏览与导航（面包屑 / 地址栏 / 后退 / 上级 / F5）· POSIX 权限与属主显示及修改 ·
新建目录 · 重命名 · 复制（远程↔本地、远程→远程）· 拖放上传下载 ·
右键菜单与命令栏按钮 · 删除由自研窗口承担真实进度与取消 ·
10 万文件大目录下右键/删除/属性不再冻结 UI · 复制 33 430 个文件数量正确。

已知缺陷（**含根因与修法**）见 [docs/KNOWN_ISSUES_2026-09-14.md](docs/KNOWN_ISSUES_2026-09-14.md)：
递归设置权限只改目录不改文件（并入「远程操作队列」一起做）、
深相对路径经剪贴板协议无法表达（已改为明确拒绝而非静默出错）、
`getr` 逐文件串行且一处失败即整树中断。

## 构建与安装

```powershell
# 需要 VS2022（C++ 工具集 + Windows SDK）与 .NET 8 SDK
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1
powershell -ExecutionPolicy Bypass -File dist\ExplorerRemoteFs-win-x64\install.ps1
# 卸载：dist\ExplorerRemoteFs-win-x64\uninstall.ps1
```

站点凭据通过常驻客户端 GUI 添加，或写 `%APPDATA%\ExplorerRemoteFs\connections.json`。
调试日志：`C:\temp\remotefs-debug.log`（4 MB 轮转，行首 tick 为 `GetTickCount`）。

## 路线

| 期 | 内容 | 文档 |
|---|---|---|
| 近期 | 自研进度窗口升格为**「远程操作队列」**（删除 / 递归改权限 / 传输共用一个队列与一组契约），顺带修递归 chmod | `docs/KNOWN_ISSUES_2026-09-14.md` §1 §3 |
| 近期 | **ERF 协议第一步**：在 SFTP 上解决"多而小文件"传得慢与传不全（并行会话 / `ssh exec` 打包流 + manifest + 校验） | `docs/ERF_PROTOCOL_PLAN.md` |
| 已评估 | 站点/目录右键「在 Windows 终端中打开」：认证**交给终端里的 ssh**，产品不碰凭据 | `docs/OPEN_IN_TERMINAL_FEASIBILITY.md` |
| 之后 | **扩展 Explorer 的本地能力**（`Explore` 作动词的第二条腿）：批量重命名、校验、差异比对等复用同一套队列 UI 与契约 | 待定 |
| 长期 | ERF-Server 代理模式起步 → 独立服务端（ext4/NTFS/对象存储），语义声明与独占能力落地 | `docs/ERF_PROTOCOL_PLAN.md` §7 |

## 成功判据

```text
1. Win+E → 左侧进入站点 → 直接看到 /var/www
2. Details 显示 rwxr-xr-x / owner / group，且能改（含递归）
3. Ctrl+C/V、F2、Delete、拖放、右键菜单全部可用，且 Explorer 不冻结
4. 大目录与小文件海：传得完，也传得快
5. 双击远端配置文件 → 本地默认编辑器打开，保存自动回传
6. 任何"部分完成"都必须被看见，禁止报成功
```

## 文档索引

| 文档 | 内容 |
|---|---|
| [PROJECT_IDENTITY.md](docs/PROJECT_IDENTITY.md) | 名称、品牌分层、版本号规范、`erf://` 前缀来源 |
| [RELEASE_v0.1-Alpha.md](docs/RELEASE_v0.1-Alpha.md) | 0.1-Alpha 发布说明与验收方法 |
| [SHELL_NAMESPACE_SPEC.md](docs/SHELL_NAMESPACE_SPEC.md) · [PIVOT_WIN11_STRATEGY.md](docs/PIVOT_WIN11_STRATEGY.md) | Shell 层规格与选型转向 |
| [UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md](docs/UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md) | UI 线程不变量的由来：冻结根因与 5 个被否证假设 |
| [KNOWN_ISSUES_2026-09-14.md](docs/KNOWN_ISSUES_2026-09-14.md) | 当前已知问题（根因、WinSCP 对照、队列契约） |
| [ERF_PROTOCOL_PLAN.md](docs/ERF_PROTOCOL_PLAN.md) | 协议蓝图与第一步可执行拆解 |
| [ERF_RESIDENT_SERVICE_ARCHITECTURE.md](docs/ERF_RESIDENT_SERVICE_ARCHITECTURE.md) | 常驻服务与进程边界 |
| [OPEN_IN_TERMINAL_FEASIBILITY.md](docs/OPEN_IN_TERMINAL_FEASIBILITY.md) | 「在 Windows 终端中打开」实测结论 |

---

*项目起始 2026-08-20（原名 Explorer RemoteFS，2026-09-15 定名 ExploreRemoteFiles）·
原始探索概要归档于 `reference/`。*
