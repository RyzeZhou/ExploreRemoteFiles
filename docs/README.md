# `docs/` 索引（本目录**只在本地**，不进 git；但工作区是共享盘，另一台机器能看到）

> 时间紧就看**加粗**的三份。

## 交接 / 现场记录（最常看）

| 文件 | 内容 |
|---|---|
| **`CHANGE_HANDOVER_2026-09-20.md`** | **变更交接**：Win10 侧自 `ab50578` 以来的 31 个提交改了什么、哪些与 Win11 侧工作重叠、要重编/重拉什么（不含多标签页问题的重述） |
| **`KNOWN_ISSUES_2026-09-19-win11-shell.md`** | **Win11 Shell 现场证据全集**（属性页锁窗/按钮失效、`erf:` 协议与注册分流、地址栏输入路径根因、Explorer 栈溢出崩溃专项、票据 .erfdl、SQLite 缓存、决策框返工）——最长最全 |
| `KNOWN_ISSUES_2026-09-17-win11-copy.md` | Win11 复制数据通路专项（`CFSTR_FILECONTENTS` vs `ITransferSource`、直传流） |
| `KNOWN_ISSUES_2026-09-18-setup-explorer-black-screen.md` | 安装后 Explorer 黑屏 |
| `KNOWN_ISSUES_2026-09-14.md`、`KNOWN_ISSUES_2026-08-30.md` | 更早的已知问题 |

## 规格 / 技术实现

| 文件 | 内容 |
|---|---|
| `TECHNICAL_IMPLEMENTATION.md` | 桥接协议（TAB 分隔、空字段铁律）、架构底线 |
| `SHELL_NAMESPACE_SPEC.md` | Shell 命名空间规格（PIDL、列、属性） |
| `ERF_PROTOCOL_PLAN.md` | `erf:` 协议设计 |
| `ERF_RESIDENT_SERVICE_ARCHITECTURE.md` | 常驻服务架构 |
| `FEATURES_AND_DISPLAY_SPEC.md` | 功能与显示规格 |
| `MILESTONES.md`、`CURRENT_PROGRESS_2026-09-12.md` | 里程碑与进度快照 |
| `PROJECT_IDENTITY.md`、`PIVOT_WIN11_STRATEGY.md` | 项目定位与 Win11 战略 |
| `RELEASE_v0.1-Alpha.md` | 发布说明 |
| `GUI_CLIENT_AND_CREDENTIALS.md` | 客户端与凭据 |

## 专项分析（历史，需要时再翻）

`BREADCRUMB_ISSUE_ANALYSIS.md`、`NAVIGATION_INVESTIGATION_2026-08-24.md`、
`DOUBLECLICK_INVESTIGATION_2026-09-06.md`、`UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md`、
`OPEN_IN_TERMINAL_FEASIBILITY.md`、`TERMINAL_AND_OPERATIONS_2026-09-16.md`、
`TERMINAL_FEATURE_STATUS_2026-09-16.md`、`SHELL_EXTENSION_LESSONS.md`、
`FTP_CLIENT_RESEARCH_AND_PLAN.md`、`EXPLORER_LAYER_ENHANCEMENTS.md`、
`EXPLORATION_PLAN.md`、`IMPLEMENTATION_GUIDE.md`、`DEBUG_STATUS_2026-08-23.md`、`RESEARCH_LOG.md`

## 其它（工作区根）

| 位置 | 内容 |
|---|---|
| `..\..\AGENTS.md` | **工作区规则**（笔录纪律、`tmp\` 约定、相对路径、PowerShell 坑）——接手前必读 |
| `..\..\笔录\explore-remote-files.md` | 用户消息全记录（前缀 `ERF-`，按时间追加） |
| `..\tmp\explore-remote-files\harness\` | 一次性测试工具（票据端到端、剪贴板、UI Automation 点弹窗、查库） |
| `..\src-cpp\ExplorerDataProviderFtp\third_party\sqlite\` | 内置 SQLite（目录缓存 + 票据库用） |

## 最新状态（2026-09-20 夜，Win11 侧）

| 事项 | 状态 |
|---|---|
| **Win11 多标签精确直达** | ✅ **已修复并实测**：`App.xaml.cs` 用 UIA `SelectionPattern` 读活动标签名 → 匹配 ShellWindows 条目 `LocationName` 定位当前标签。实测同一窗口 4 个标签（`R`/`B`/`main`/`此电脑`）能精确命中活动标签（旧逻辑全并列、只取枚举最小者）。随后优化为**延迟调用 + FindFirst**（仅同分并列时才读 UIA），见 `KNOWN_ISSUES_2026-09-19-win11-shell.md` 末节 |
| **协议通道为什么比原生命名空间慢** | `erf:` 走 URL 协议 = 另起进程 → IPC → COM 枚举 → 跨进程 UIA → `Navigate2`（5 跳）；而 `易远传/WSL/R`、`::{CLSID}\…` 是 Explorer 自己进程内解析（1 跳）。`xxx:` 在 Win11 一律判 scheme，无法降级 —— 这是通道固有代价 |
| **全面重编译** | 待做：Win10 VM 跑 `scriptsuild-release.ps1` → `src-setupuild-inno.ps1`（首次多编 `third_party\sqlite\sqlite3.c`；DLL 476KB → 1.52MB）。装了新 DLL 才能验证**传输新逻辑**与 **Excel 崩溃**（`dll_unloaded + 0xc0000005`，疑似已由 `3ef54d4`/`1b923b3` 的线程 pin 修复覆盖） |
| **远程 main** | 本地分支已改名 `main`（HEAD `4dd7301`）；远程 `origin/main` 与本地分叉（远程有 4 个提交：README 修订 + 2 张截图）。**远程待重建**，README 与本机截图已备份到 `tmpemote-main-backup\`（截图与本地逐字节一致） |
| **README** | 已按真实状态更新：补传输票据 `.erfdl`、SQLite 缓存、设置页四页、即点即下、多标签直达；修正"上传/下载未并入队列"这条**已过时**的限制 |

> 笔录（`..\..\笔录\explore-remote-files.md`，前缀 `ERF-`）已记到 `-61`，一条不漏。
> 笔录工具 `笔录ecord.py` 已重写：ID **自动派生**（像 Word 自动编号），要改结构用
> **插入 / 删除 / 交换**（不混用、可批量），重编号会自动同步正文里的引用。
