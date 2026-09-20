# 里程碑与出口条件

> 目的：把"什么叫做完了"写死，避免用"差不多能用了"当标准。
> 制定于 2026-09-15（`v0.1-Alpha` 推送之后）。

## 版本语义的更正（重要）

今天推的 `v0.1-Alpha` 是**检查点**，不是真正的 Alpha：它的已知问题里有一条
"递归设置权限只对目录生效"，而递归改权限恰恰是 Alpha 的出口条件之一。
因此本文件的定义优先，两条路选一：

```powershell
# 方案 A（推荐，历史诚实）：把今天的 tag 改名为预发布，Alpha 留给三件事做完时
git tag -d v0.1-Alpha ; git push origin :refs/tags/v0.1-Alpha
gh release delete v0.1-Alpha --cleanup-tag -y
git tag -a v0.0.9-pre-alpha b2a1909 -m "预发布检查点：UI 线程不再阻塞、复制不再丢文件"
git push origin v0.0.9-pre-alpha

# 方案 B：保留 v0.1-Alpha，但 Release 说明里必须写明"递归权限未达门槛"
```

未替你做这个改动——已发布的 tag 与 Release 属于对外事实，改名/删除要你点头。

---

## Alpha 出口条件（四项全满足才算 Alpha）

### A1 递归设置权限（含文件）

> **进度（2026-09-15）：代码与实测已完成**——两个 Provider 的叶子分支已修（含"目录自身失败也要
> 继续下探"），接口返回 `ChmodRecursiveResult{Dirs, Files, Failures}`，CLI 以退出码 3 报 `PARTIAL`。
> 实测 `chmodr /chmodtest 750` → `dirs=2 files=3 failed=0`，改后**文件与目录**均为 `-rwxr-x---`/`drwxr-x---`。
> **唯一未完成项：把它接进 A2 的队列窗口。**
>
> **进度（2026-09-20）：已接进队列，A1 完成。** 递归改权限走常驻服务
> （`FtpMeta.h:715`「递归修改权限：走常驻服务」），与删除共用队列窗口，失败明细汇总；
> README 已把它列为可用能力（「递归改权限（`chmod -R`，文件与目录都改）」）。

- `SetPermissionsRecursive` 补上叶子分支：**目录与文件都改**；符号链接**不跟随**
  （不通过它改目标权限）。FTP 与 SFTP 两个 Provider 同形，都要核对。
- 回报"改了 N 个目录 / M 个文件 / K 项失败"的清单，**有失败即为部分完成，不得报成功**
  （同一类问题已在 `kMaxItems` 截断上犯过一次，见
  [UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md](UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md) §6）。
- 走「远程操作队列」（A2），而不是就地阻塞对话框。
- **验收**：对含文件的目录递归设 `750`，远端 `ls -l` 里**文件**位必须变；
  拔掉网线/删一个不可写文件时，队列末尾显示失败明细而非"完成"。

### A2 远程操作队列（`RemoteOperationQueueWindow`）

> **进度（2026-09-16）：用户可见的要求已完成。**
> `DeleteProgressWindow` 已升格为 **`RemoteOperationQueueWindow`**：一个站点一个窗口
> （标题「操作队列：&lt;站点名&gt;」），删除与递归改权限以**条目**并存、每条独立取消、
> 全部结束后汇总「成功/失败/取消」并带失败明细，另加任务栏进度（`ITaskbarList3`）。
> 关窗 ≠ 取消：有条目在跑时拒绝关闭，必须显式取消。
> 证据：`RemoteFsClient.exe --queue-selftest`（17 条断言全过）、
> `research/tools/bridge-probe/probe.ps1`（同站点并发 `CHMOD -R 700` + `DELETE -R` 均 `OK`；
> 远端实测目录与文件全部 700、被删的树确实不存在）。
> 设计与本轮新踩的两个坑（WPF TwoWay 绑定只读属性把常驻进程带崩；自检必须 Show 窗口，
> 否则 DataTemplate 绑定根本没附加）见
> [TERMINAL_AND_OPERATIONS_2026-09-16.md](TERMINAL_AND_OPERATIONS_2026-09-16.md) §11。
>
> **仍未做**（下表里属于"与传输队列统一"的部分）：上传/下载纳入同一窗口、
> 进度改为**字节 + 条目数双维度**、把 `TransferTaskService` 按队列语义收敛。
>
> **进度（2026-09-20）：主体已完成，A2 达成。**
> - **传输已统一走常驻服务队列**：桥接新增 `FETCH` / `FETCHDIR` / `PUT` / `FETCHSTREAM`
>   （直传流：Service → 命名管道 → `IStream`，不再落 `%TEMP%`），删除 / 递归改权限 / 传输
>   共用队列窗口，各有进度、当前项、错误汇总与**取消**。
> - **暂停语义已定并实现**（`TransferTaskService.CanPause` / `TogglePause`）：**自家路径**
>   （菜单「下载」、复制到本地、上传）可真暂停；**由资源管理器复制对话框驱动的任务**禁用暂停
>   —— 流由对话框持有，停住会让整个复制对话框卡死（实测结论）。
> - 源端展开改为**后台预热**：复制前先递归列目录到缓存，Explorer UI 线程不再同步 LIST
>   （此前一次展开 26535 项、阻塞 2.6 s）。
> - 附带产出：**传输票据 `.erfdl`**（Ctrl+C 产出下载票 → 放到目标目录双击下载，天然支持
>   "先定位置再下载"与真暂停/续传）；菜单「下载」改**即点即下**（落默认下载目录，同名自动加 `" (2)"`）。

把现在的 `DeleteProgressWindow` 升格为通用队列；删除、递归改权限、属主变更、
上传下载、（未来）ERF 打包流共用一个窗口、一套契约：

| 契约 | 要求 |
|---|---|
| 条目 | `操作类型 / 目标 / 已完成 / 总数 / 状态 / 失败原因`，目录与文件同列 |
| 进度 | **字节 + 条目数双维度**（大文件与小文件海都要看得出在动） |
| 取消 | ≤ 1 s 生效，清理 `*.rfs-part` 等中间态 |
| 失败 | 单项失败**继续队列**，末尾汇总；有失败即 `PARTIAL`，**禁止报成功** |
| 线程 | Explorer 进程内永不做网络 I/O（既有不变量） |
| 原生窗口 | 传输/删除不出现 Explorer 原生进度条（已由空枚举 + 自研窗口达成） |

实现落点：`TransferTaskService`（440 行，已有 download/upload/delete 三种方向标签）是现成雏形，
按队列语义收敛，而不是新造一套。
- **验收**：同时挂三个操作（删除大目录 + 递归改权限 + 上传）→ 队列三项并存、
  各自进度独立、逐个取消互不牵连、全部完成后失败项可复查。

### A3 在目录右键打开终端

> **进度（2026-09-20：A3 保持完成状态）**：菜单项、站点级终端、SSH 绑定校验均可用；
> 实现见 `ContextMenu.cpp`（`wt.exe ... ssh.exe -p <port> <user>@<host> -t "cd '<远端目录>' && exec $SHELL -l"`）。
>
> **进度（2026-09-16 晚：三条路线全部打通）**：WT 走 **fragment profile + `wt -p`**、
> VS Code 走**请求文件 + 心跳**（复用已连窗口原地开终端，不再每次弹信任）、PowerShell 直接起 shim；
> 另有站点级终端 + SSH 绑定校验、递归改权限异步化、列顺序自定义。
> 全部协议、13+6 条踩坑、未完成清单与 8 条验收用例见
> [TERMINAL_AND_OPERATIONS_2026-09-16.md](TERMINAL_AND_OPERATIONS_2026-09-16.md)（本文档下文为当时的中间状态记录）。
>
> **当时的进度（2026-09-16 早）：代码已就位，WT 的启动方式未打通。** 菜单项、终端由服务程序设置决定、
> 三种终端的代码路径、SSH 密钥复用、VS Code 的 `~/.ssh/config` 受管块都已实现（`8af90d0`）；
> 但 **Windows Terminal 会把它命令行里以 `-` 开头的 token 当自己的选项吃掉**，于是报
> `0x80070002`"启动…时找不到文件"。出路（引导写 WT profile + `wt -p` 启动 + 目标目录走 sidecar 文件）、
> level 1 空白处菜单的插入点与"错位会触发别的动作"的风险、验收用例、测试痕迹清理清单，
> 全部记在 **[TERMINAL_FEATURE_STATUS_2026-09-16.md](TERMINAL_FEATURE_STATUS_2026-09-16.md)**。
> 另外：早先记的"`wt` + 整条 commandline 这条已验证路径"**已被推翻**（见该文档 §3）。

可行性与红线已实测完毕，见 [OPEN_IN_TERMINAL_FEASIBILITY.md](OPEN_IN_TERMINAL_FEASIBILITY.md)。要点：
认证**交给终端里的 ssh**（密码在终端交互输入），产品不碰凭据；
`wt --appendCommandLine` 在 1.24.11911 实测不生效，用"`wt` + 整条 commandline"这条已验证路径。
- **验收**：站点右键 → WT 里落到 `StartPath`；深层目录右键 → 落到该目录；
  目录名含 `'`、`$()`、空格、中文时**不得执行任何远端命令**（转义用例进测试表）；
  FTP 站点不出现该菜单项。

### A4 Windows 安装程序（三件事完成后开始）

> **进度（2026-09-20）：已完成并多次实机安装。** 按下面的三个岔路全部落地：
> **Inno Setup**（非 MSIX）、**per-user 默认**（可自定义目录；本机实测装在 `D:\Program\ExplorerRemoteFs`）、
> **登录时自启**（`HKCU\...\Run`，非 Windows 服务）。
> 安装包 `Erf-0.1-Alpha-Setup.exe`（约 131 MB，单文件自包含，含 DLL 哈希自校验）；
> 向导可选安装目录 / 桌面快捷方式 / 自启动，并有「安装完成后重启资源管理器」附加任务。
> 安装/升级/卸载**不终止资源管理器**（DLL 用"先改名再写新文件"的方式替换）；
> `src-setup/inno-test.ps1` 提供"装到临时目录 → 断言 → 卸载"自检。
> `.erfdl` 关联与 `erf:` 协议注册都在安装器里，且 `erf:` 注册按 Windows 版本分流。

Alpha 之后第一件事就是它，因为"能安装"决定别人能不能真的用上。

**必须先定的三个岔路（做错会返工）**

1. **安装包形态**
   - ❌ **不要用 MSIX**：MSIX **不支持传统 Shell 扩展 / 供解包进程（Explorer.exe）加载的
     in-process COM 服务器**。我们的核心就是一个被 Explorer 加载的 DLL，
     这条路在架构上封死，别在选型阶段被"现代打包格式"带走。
   - ✅ **推荐 Inno Setup 起步**：脚本化、可选安装目录、写注册表、
     注册/注销 COM、卸载干净，工程量最小，最快拿到"别人能装"的状态。
   - ⏩ 将来若需要企业部署/GPO/静默分发，再上 **WiX（MSI + Burn bundle）**；
     Inno 的脚本资产不会白费，语义是同一套。
2. **per-user 还是 per-machine**
   - 现状是 **per-user**：装 `%LOCALAPPDATA%\ExplorerRemoteFs`、写 `HKCU`、不要求管理员。
   - 安装到指定目录（如 `C:\Program Files`）意味着 **per-machine**：需要管理员、
     COM 注册进 `HKLM`、且**常驻程序不能只靠 `HKCU` 配置**（多用户各自凭据要隔离）。
   - 决策建议：**两者都支持，默认 per-user**——个人用户不碰 UAC，企业按需 per-machine。
     注意两者混装会留下互相看不见的注册，安装器必须检测并明确提示。
3. **"服务程序自启动"不要做成 Windows Service**
   - Windows 服务跑在 **Session 0**，**弹不出交互窗口**——而 A2 的远程操作队列正是要弹窗口显示进度。
   - 正确做法：**用户登录时启动**，二选一
     - `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`（最简单，随登录起）
     - 任务计划程序（`schtasks` / XML 触发器 `LogonTrigger`）——可配"仅交互式会话"、
       失败重启、单实例，更贴近"服务"的体验
   - 常驻程序自身必须有：命名互斥单实例、崩溃自恢复（由 Run/计划任务在下次登录或看门狗拉起）、
     命名管道 ACL 只允许当前用户。

**安装器功能清单（Alpha → 0.2）**

- [ ] 选择安装目录；检测旧版本并**先卸载旧版再装**（我们踩过两版 DLL 并存：
      旧 `ExplorerDataProvider.dll` 与新 `ExplorerDataProviderFtp.dll` 同时被 Explorer 加载，
      死注册还留在 `HKCU` —— 见 KNOWN_ISSUES 与清理记录）
- [ ] 注册/注销 COM 与命名空间（沿用现有 `install.ps1` / `uninstall.ps1` 语义，
      尤其保留 `erf://` 的**所有权检查**：已被他人占用就中止，不覆盖）
- [ ] 升级/卸载前**重启 Explorer**（DLL 被占用是硬事实；给用户可点的按钮而非静默 kill）
- [ ] 卸载干净：移除注册表项、`erf://`（仅当 owner 是我们时）、计划任务/Run 项；
      **凭据默认保留并询问**（删掉用户密码是极差体验）
- [ ] 日志与缓存目录改为 `%LOCALAPPDATA%\ExploreRemoteFiles\logs`——
      现在硬写 `C:\temp\remotefs-debug.log`，不能带进安装包
- [ ] 卸载残留自检脚本（复算我们写过的键），并把"不要对不属于我们的键用 `-Force`/整体重写"
      这条已付出的教训写进安装器注释
- [ ] 版本号单一来源（DLL/EXE/安装包/`erf://` 注册标记一致），并在队列窗口显示版本

**签名与现实预期**

- 未签名 → SmartScreen 警告与杀软误报是**必然**，不是 bug。发布页要写清如何"仍要运行"。
- 值得评估的低成本路径：**Azure Trusted Signing**（对个人/小团队签发，成本远低于传统 OV 证书）；
  具体资质、可用区域与费用需**先核实再承诺**。
- 不做静默遥测；崩溃回到本地日志文件。

**A4 进展（2026-09-17）：用 Inno Setup 出包，A4 主体完成**

- 选型定案：**Inno Setup 7**（`src-setup/erf.iss` + `build-inno.ps1`），产物是单文件自包含的
  `dist\Erf-0.1-Alpha-Setup.exe`（131 MB，payload 356 MB）。经典向导
  （欢迎 → 选择安装位置 → 任务 → 准备安装 → 安装 → 完成）+ "应用和功能"里的卸载项 +
  升级识别 + 静默安装（`/VERYSILENT /DIR /LOG`）+ 签名接入，全部是 Inno 现成能力。
  MSIX 依旧排除（不支持 in-process Shell 扩展）；per-user 免 UAC（`PrivilegesRequired=lowest`）。
- 手搓的那版（`ErfSetup.cpp` + 自绘属性表向导）已经删除——能跑、也过了 35 项断言，
  但维护成本明显高于 Inno，代码留在 git 历史（`daed431` 附近）。
- 已完成：可选安装目录、静默安装/卸载、ARP 项与登录自启、`erf://` 归属检查、
  卸载保留站点配置与凭据、装/卸前关 `AutoRestartShell` 的 explorer 重启舞蹈、日志搬到
  `%LOCALAPPDATA%\ExplorerRemoteFs\logs`、装完即启动常驻服务（静默路径也启动）。
- 可重复验证：`src-setup/inno-test.ps1` —— `/VERYSILENT` 装到临时目录 → **43 项断言** →
  `unins000.exe /VERYSILENT` 卸载 → 再断言一遍；**2026-09-17 全部通过**。
- 本轮最值钱的一课：**32 位模式注册表重定向**（`ArchitecturesInstallIn64BitMode`）——
  不加这一行，`HKCU\Software\Classes\CLSID\...` 会写进 `Wow6432Node`，64 位资源管理器
  完全看不到我们的 in-proc 服务器，而安装/卸载过程"一切成功"。
- 未完成：代码签名方案（未签名 = SmartScreen 警告 + 杀软误报）、旧版注册的自动检测提示、
  面向用户的安装说明（截图/FAQ）。
- 细节见 `TERMINAL_AND_OPERATIONS_2026-09-16.md` §15（含坑清单与验证方式）。

---

## 之后的版本线

| 版本 | 出口条件 |
|---|---|
| **Alpha** | A1 + A2 + A3 全部满足（安装包可暂用脚本安装） |
| **0.2** | A4 安装包可用（per-user，Inno，可升级可卸载干净）+ 签名方案定案 |
| **0.3** | ERF 协议第一步达标：SFTP 上"多而小文件"实测提速并**传全**（基准三数见 [ERF_PROTOCOL_PLAN.md](ERF_PROTOCOL_PLAN.md) §8.6） |
| **Beta** | per-machine 安装 + ERF-Server 代理模式跑通语义声明 |
| **1.0** | 协议冻结（`erf-*@projecterf.dev` 与 `settings.json`/CLI 契约不再破坏性变更） |
