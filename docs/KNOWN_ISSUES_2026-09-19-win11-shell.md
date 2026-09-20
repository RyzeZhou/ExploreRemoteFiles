# Win11 Shell：属性页不锁 Explorer、`erf:` 精确当前标签页（2026-09-19）

> 状态：属性页与注册分流代码/安装均已完成；**`erf:` 精确当前标签页卡在 Win11 设计上**（无公开接口）。
>
> 📌 **Win10 侧自 `ab50578` 以来的改动清单**：`docs\CHANGE_HANDOVER_2026-09-20.md`
> —— 只说明"我改了什么、哪些与你的工作重叠、要重编什么"，不重述你写的问题描述。

## 现场证据与根因

Win11 的 `rfs-shell-watch.log` 记录了属性窗口存在期间 Explorer 的 `CabinetWClass`
从 `enabled 1 -> 0`，关闭属性窗口后才恢复为 `0 -> 1`。同一轮
`remotefs-debug.log` 的窗口链来自 `PermDlgProc`，不是 Explorer 的标准属性表页面。

根因有两条：

1. 选中文件的 `MENU_PROPERTIES` 在 Explorer UI 线程同步 `ReadRemoteMeta`，随后调用
   `DialogBoxParamW(..., ci->hwnd, ...)`。该 API 会禁用 `ci->hwnd` 所属的 Explorer；Win11
   标签宿主把这个 owner-modal 链扩散到其他 Explorer 窗口。
2. `erf:` 由外部 `RemoteFsClient.exe --open-erf` 接管后，原代码枚举 `ShellWindows` 并按 HWND
   选择浏览器。Win11 的两个标签条目都报告同一个 `CabinetWClass` HWND；日志中二者得分同为 4，
   所以 `>` 的并列规则固定选择枚举较早的标签。这不是可用更换打分规则修复的身份信息缺失。

## 修复

- `ShowRemotePropertiesSheetModeless` 成为文件和目录属性共用入口：只读本地元数据缓存、冷缓存由
  `MetaWarmThread` 回填，并创建 `IDD_PERMPAGE` 的 `PSH_MODELESS` 标准 `PropertySheetW`（parent 为
  `NULL`）。因此自定义菜单路径也统一为白底、有“文件属性”标签的原生属性表；选中文件路径不再同步访问远端，
  也不再调用 `DialogBoxParamW` 或无标签的 `IDD_PERMBOX`。
- 新增 `CErfProtocolCommand`，并以 `erf\shell\open\command\DelegateExecute` 注册为 in-process
  `IExecuteCommand`。它从 `IObjectWithSite` 获取**发起地址栏请求的** `IShellBrowser`，解析目标 PIDL
  后调用 `BrowseObject(..., SBSP_SAMEBROWSER | SBSP_ABSOLUTE)`；不再以共享 HWND 猜测标签。
- 非 Explorer 调用没有 Shell site 时才回退到常驻客户端，保留外部 `erf:` 打开的兼容性。远程访问、
  缓存、队列和传输仍由常驻服务承担；DLL 只负责当前标签页的 Shell 导航语义。
- 诊断日志只记录 URI 长度、HRESULT、site 是否存在、目标长度；不记录完整 URI 或密码。

## Win11 回归步骤

先在具备 Visual Studio Desktop C++ workload 的环境构建并安装：

```powershell
Set-Location Z:\tools\explore-remote-files
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-cpp\ExplorerDataProviderFtp\compile.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-setup\build-inno.ps1
```

安装新包后，重启 Explorer 或注销/重新登录一次（in-process DLL 不会被 `SHChangeNotify` 卸载）。

1. 在同一 Explorer 窗口开两个不同标签，切到第二个标签，在地址栏输入可控目录的
   `erf:<site>:/<path>`。预期：第二个标签原地变成目标目录，第一个标签不变；日志有
   `[ERF-DELEGATE] SetSite present=1` 与 `BrowseObject same-browser hr=0x00000000`。
2. 对一个缓存命中和一个缓存未命中的远程小文件分别点“属性”。预期：所有 Explorer 窗口始终可点击；
   日志有 `properties standard-sheet modeless ... owner=NULL`，不会再出现该菜单路径的 `DialogBoxParamW` 或 `PermDlg init`。
3. 可选执行安装器注册回归：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-setup\inno-test.ps1
```

它现在会断言 DelegateExecute CLSID、其 `InprocServer32` 和 `erf:` 命令注册；本机因缺少 C++ 工具链
未执行这项实机验证。

---

## 追加问题：modeless 属性表的「确定 / 取消 / 关闭」全部失效（2026-09-19）

现场：`7bcac55` 之后外观已经对了 —— 白底、带「文件属性」标签、不再禁用其他 Explorer 窗口；
但**三个按钮点了都没有反应**，属性页关不掉，只能关掉 Explorer 主窗口，属性页才随之消失。

根因是 `PSH_MODELESS` 的固有行为，与 Win10 / Win11 无关：

1. 属性表的按钮属于属性表 **frame**，不属于我们提供的页面，所以页面过程 `PermPageProc`
   收不到这些 `WM_COMMAND`；
2. 模态属性表由它自己的内部模态循环处理按钮并结束窗口，而 `PSH_MODELESS` 下**那个循环不存在**，
   frame 收下点击后不销毁窗口；
3. 于是属性表窗口一直留着；它又是 Explorer 的 owned window（`psh.hwndParent = NULL` 时系统取
   活动窗口作 owner），所以关掉 Explorer 时它才跟着消失 —— 与实测观察完全一致。

修复：子类化 sheet frame（`SetWindowSubclass` + `DefSubclassProc`）接管按钮 ——

- `WM_COMMAND` 且 `IDOK / IDCANCEL / IDCLOSE`：**先** `DefSubclassProc` 让 frame 自己处理
  （「确定」必须由 frame 把 `PSN_APPLY` 通知发给页面，`chmod` 才会写回），**再** `DestroyWindow`；
- `WM_CLOSE`、`WM_SYSCOMMAND(SC_CLOSE)`：直接 `DestroyWindow`（右上角 X 与 Alt+F4）；
- 即使系统在某条路径上也自己销毁了窗口，`IsWindow` 检查让这次销毁成为幂等操作。

### 诊断日志（`remotefs-debug.log`）

- `[DIAG] sheet frame button cmd=<id> -> close modeless sheet hwnd=...`
- `[DIAG] sheet frame WM_CLOSE -> close modeless sheet hwnd=...`
- `[DIAG] sheet frame SC_CLOSE -> close modeless sheet hwnd=...`
- `[DIAG] sheet frame subclass failed err=...`（挂子类失败时留证据）

点按钮后若**没有**出现第一行，说明命令根本没到 frame，那是另一层问题（消息泵 / 窗口层级），
需要新的取证 —— 这条日志本身就是分界线。

### 本轮未做（留给后续）

- 属性页「确定」路径里的同步 `RunCli(chmod)` 仍跑在 Explorer 的 UI 线程上，远程慢时会卡住属性页；
  递归改权限已交常驻服务，非递归这一支也可以照此办理。
- `RemoteFsShell` 目录属性仍有 `DialogBoxParamW(IDD_PERMBOX)` 的模态路径（较低优先级）。

### 回归步骤

编译必须在有 VS Desktop C++ workload 的机器上（Win10 虚拟机），产物再装到 Win11：

```powershell
Set-Location D:	ools\explore-remote-files
git log -1 --oneline
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-cpp\ExplorerDataProviderFtp\compile.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scriptsuild-release.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-setupuild-inno.ps1
Get-Item .\dist\Erf-0.1-Alpha-Setup.exe
```

把 `dist\Erf-0.1-Alpha-Setup.exe` 复制到 Win11 安装，然后重启 Explorer（或注销再登录）。

1. 右键远程文件 → 属性：窗口应为白底带「文件属性」标签；点「取消」应立即关闭；
   点「确定」应先写回 `chmod` 再关闭；点右上角 X 也应关闭。
2. 三个动作都应在 `remotefs-debug.log` 里留下对应的 `sheet frame ...` 日志。
3. 属性页打开期间，其他 Explorer 窗口应始终可点击（不回归到「其他窗口变暗」）。

---

## 追加问题：地址栏输入 `erf:` 直接报「参数错误」（2026-09-19）

现场：`1941916` / `a0299c6` 引入 in-process `IExecuteCommand`（`DelegateExecute`）之后，
Win11 地址栏输入 `erf:<site>:/<path>` 直接弹「参数错误」；改动之前（走常驻客户端 `--open-erf`）
能直达，只是跳固定标签页。

根因：**注册表里 verb 的命令行模板被整串交给了 `SetParameters`。**

- `src-setup/erf.iss` 除了 `DelegateExecute = {A970407D-...}`，还写着
  `command 默认值 = '"...\RemoteFsClient.exe" --open-erf "%1"'`；
- Shell 调 `IExecuteCommand::SetParameters` 时把带模板的那一串（或整条命令行）交进来，
  **不是**纯 URI；
- 而 `IsErfAddress` 只接受「整串以 `erf:` 开头」→ 校验失败 → `Execute()` 返回
  `E_INVALIDARG`(0x80070057)，它的中文消息正是「参数错误」。

（官方样例 `RegisterExecuteCommandVerb` 只写 `DelegateExecute`、不写 command 默认值；我们保留它
是为了非 Explorer 调用与 DelegateExecute 创建失败时的回退，代价就是参数解析必须容错。）

修复（`src-cpp/ExplorerDataProviderFtp/ErfProtocolCommand.cpp`）：

1. 新增 `ExtractErfAddress`：在参数串里定位 `erf:`（必须位于串首或空格 / 制表符 / 引号之后，
   避免把 `--open-erf` 当成协议名），再从那里取到串尾、去掉尾部空白与引号 ——
   纯 URI、参数部分、整条命令行三种形态都能认出来；
2. `IsErfAddress` 改用上面的结果；另外 `UrlUnescapeW` 失败不再直接判死（保留原文继续校验）、
   `erf:site:`（无路径）按站点根 `/` 处理 —— 都只为少抛「参数错误」；
3. `StartResidentFallback` 改传提取出的干净 URI（原先传整串参数，会让 CLI 收到
   `--open-erf "--open-erf ..."`）；
4. `BuildTargetPidl` 给 `ParseDisplayName` 一份可写副本，不再 `const_cast` 到 `std::wstring`
   的内部缓冲区（那是 UB）；
5. 日志拆开：`BuildTargetPidl hr=...` 与 `BrowseObject same-browser hr=...` 分开记录；
   拒绝时记 `parameters-length` 与 `has-erf-colon`（**只记长度，不记内容**，不泄露路径/凭据）。

### 验证

本机没有 C++ 工具链，无法编译。把两个纯逻辑函数**逐行等价移植到 Python** 后跑了 17 个用例
（`tmp\explore-remote-files\erf-address-logic-test.py`）：**旧逻辑在「我们的注册」的两种输入下
全部拒绝**（复现根因），新逻辑全部符合期望。C++ 侧另做了去注释/字符串后的结构配平检查
（花括号 41/41、圆括号 172/172）。

### 回归步骤

Win10 虚拟机编译 → 装到 Win11 → 同一窗口开两个标签、切到第二个标签，地址栏输入
`erf:<site>:/<path>`：

- 预期**只在当前（第二个）标签**原地跳转；`remotefs-debug.log` 出现
  `[ERF-DELEGATE] SetSite present=1`、`BuildTargetPidl hr=0x00000000`、
  `BrowseObject same-browser hr=0x00000000`；
- 若出现 `no shell browser ... resident fallback`，说明 Shell site 没拿到，会退回常驻客户端
  （能直达但跳固定标签）—— 那属于另一层问题，按该日志继续取证；
- 不应再出现「参数错误」。

---

## 决策：erf: 注册按 Windows 版本分流（2026-09-19）

实测把两个平台的行为钉死了：

| | Win10（经典地址栏 `ToolbarWindow32`） | Win11（XAML/WinUI 3 地址栏） |
|---|---|---|
| 命令行通道 `shell\open\command` | URI 经 `%1` 到达常驻客户端 ✅ | 同样到达 ✅ |
| `DelegateExecute`（COM 通道） | — | 只调 `SetSite` + `Execute`，**从不调 `SetParameters`** ❌ |

Win11 上两条路**各拿一半**：命令行通道拿得到地址但认不出发起标签；COM 通道认得出标签但拿不到地址
（把 `command` 默认值改成 `"%1"` 也一样，实测两次都是 `parameters-length=0`）。
所以加了 `DelegateExecute` 之后反而**比不加更糟**：从「能直达但跳固定标签」退化成「直接报参数错误」。

另外实测：删掉 `URL Protocol` 值后，Win11 弹「打开此 "erf" 链接的应用」——说明它仍按 URL 协议处理，
**不会**退化成 Shell 命名空间路径解析（原本希望走这条路，因为我们的 `ParseDisplayName` 本来就接受
`site:/unix/path`）。

### 因此安装脚本按版本分流（`src-setup/erf.iss`）

- `IsWindows11OrGreater()`：`10.0` 且 build `>= 22000`；
- **Win10 分支**：命令行通道，与历史一致（已验证可用）；
- **Win11 分支**：**暂时也走命令行通道**，并显式删除可能残留的 `DelegateExecute`；
  等调研出 Win11 正确的 in-process 通道后再改这一支；
- 协议处理器的 CLSID（`InprocServer32`）保留注册，将来启用 Win11 途径时不必改安装结构；
  注释里保留一条经验：**将来写回 `DelegateExecute` 时，`command` 默认值不能简化成 `"%1"`**
  （创建失败时 Shell 会退回来执行它，导致对同一 URI 自我递归）；
- 安装日志新增 `ERF: windows-version=0x... win11-or-greater=...`，便于确认走了哪一支；
- `src-setup/inno-test.ps1` 的断言改为「当前不注册 `DelegateExecute`」。

### 待调研（交给外部检索）

Win11 的 XAML 地址栏在 URL 协议路径上如何传 URI；有没有新的 in-process 接口能**同时**拿到 URI 与
发起标签的 `IShellBrowser`；如果确实没有，官方推荐的替代架构是什么（命名空间解析 / `IExplorerCommand` /
其他）。调研提示词见会话记录。

### 临时恢复命令（不必重新编译）

```powershell
Remove-ItemProperty -LiteralPath "HKCU:\Software\Classes\erf\shell\open\command" -Name 'DelegateExecute'
Stop-Process -Name explorer -Force
```

---

## 补充结论：Win10 为什么"能实现"、Win11 改了什么（2026-09-19）

### Win10 是完整解，因为它没有原生多标签

Win10 的 Explorer 一个窗口就是一个位置，所以命令行通道（`shell\open\command` + `%1` → 常驻客户端 →
`IShellWindows` 找窗口 → 导航）**不存在"跳错标签"的可能** —— 找窗口等价于找位置。
Win11 才有原生多标签（一个 `CabinetWClass` 托管多个标签），这才逼出 in-process 的 `DelegateExecute` 路线。

### Win11 不是删了某条路，而是多了一条半成品分支

| 注册形态 | Win11 走哪条 | 拿到 URI | 拿到标签 |
|---|---|---|---|
| 只有 `shell\open\command` | 经典命令行（`%1` → CreateProcess） | ✅ | ❌（多标签） |
| 加上 `DelegateExecute` | COM 激活：建 activation site → `SetSite` → `Execute` | ❌ **`SetParameters` 不被调用** | ✅ |

即：Win11 的 XAML 地址栏在"有 DelegateExecute"时**优先走 COM 分支，而这条分支不构造传统 verb 参数**。
这与全部实测吻合（加之前能直达、加之后报参数错误、`SetSite`/`Execute` 有日志而 `SetParameters` 一行没有）。

### 已排除的路线（实测）

- **KB5052093**（build 26100.3323 修过"地址栏输入 URL 可能无法导航"）：目标机是 25H2 26200，已包含，排除。
- **让 `erf:` 走 Shell 命名空间解析**：删掉整个 `erf` 键后仍弹「选择打开此 erf 链接的程序」——
  `xxx:` 形态在 Win11 里被优先判定为 scheme，不会退化成命名空间解析。**不成立**。
- **读地址栏文本反查发起标签**：标准 UIA `ValuePattern` 在 Win11 XAML 地址栏上取不到文本
  （只枚举出 `System.ItemNameDisplay` / `Private` / `TextBox` 等空值元素）。需改用 `TextPattern`
  或 UWPSpy 再确认；暂不可用。

### 问题重新定位：真正的痛点是"命名空间内输入路径"

用户实测：易远传内部的**前进 / 后退 / 上层 / 面包屑导航全部正常**，**只有"在地址栏输入路径"不行**。
那是我们自己的 `CFolderViewImplFolder::ParseDisplayName` 的入口，属于**可修范围**，与 URL 协议无关。
待确认：输入的确切形式（`erf:site:/path`、`site:/path`、`/path`），以及日志里有没有 `[PARSE] enter level=...`。

---

## 根因（地址栏输入路径）：`PathFindNextComponent` 只认反斜杠（2026-09-19）

`::{C816CE0E-728C-4FC9-98E5-D0B35B384597}\WSL:/home/zhou/AI_work` 报「找不到这个路径，请检查拼写」——
它**确实进入了命名空间解析流程**（错误文案与「选择打开 wsl 链接的程序」完全不同），失败在解析的第一步：

Shell 把 `::{CLSID}\<name>` 拆开后交给命名空间扩展的 `pszName` 会带**前导反斜杠**
（`\WSL:/home/zhou/AI_work`），而 `CFolderViewImplFolder::ParseDisplayName` 用
`PathFindNextComponent` 切第一个组件，**它只认反斜杠**：`component` 因此被切成一个空的 `"\"`，
`PathRemoveBackslash` 后变成空串，接着 `FtpSiteFind("")` 必然失败 → `ERROR_FILE_NOT_FOUND`
→ Shell 报「找不到这个路径」。

**修复**：切分前跳过前导的 `\` 与 `/`；并补一行 `[PARSE] component=... tail=...` 日志。
Python 等价移植验证（`tmp\explore-remote-files\parse-name-logic-test.py`）：修复前带前导分隔符的输入
切出空组件、修复后正确得到 `site=WSL`，且**普通子项解析行为不变**。

### 地址栏可用的输入形式（实测）

| 输入 | 结果 |
|---|---|
| `::{C816CE0E-728C-4FC9-98E5-D0B35B384597}\WSL:/path` | **走命名空间解析** —— 修复后应能直达，且天然落在当前标签 |
| `WSL:/path`（不带 `erf:`） | 被当作 scheme `WSL` → 弹「选择打开 wsl 链接的程序」 ✗ |
| 在易远传**内部**输入 `WSL:/path` | 同上，仍被当 scheme ✗ |
| `erf:WSL:/path` | 被当作 scheme `erf` → 走协议 handler ✗ |

结论：**`xxx:` 形态在 Win11 里一律先判 scheme**，所以"地址栏输入路径"只能用 `::{CLSID}\...` 形式
（Explorer 原生支持，**不需要任何协议注册**），或者走左侧导航树。

### UIA / UWPSpy 结论：读地址栏文本反查标签不可行

`ValuePattern` 与 `TextPattern` 在 Win11 的 XAML 地址栏上**都取不到文本**（`Value`/`Text` 全空）；
UWPSpy 树里 `Microsoft.UI.Xaml.Controls.TextBox`（Name=「地址栏」）**整个窗口只有一份**，
不是每个标签一份。因此"按地址栏文本反查发起标签"这条路线**不成立**。

---

## 回退说明（2026-09-19 晚）

当天连续提交了 7 个改动，其中唯一触及 C++ 核心逻辑的 `a3904de`（`ParseDisplayName` 跳过前导分隔符）
与随后出现的 Explorer 崩溃（`0xc000041d`、偏移固定 `0x4e3b7`）落在**同一个 DLL** 里。为了不在一个
"既崩溃又阻塞复制"的构建上做二分，决定**回退到昨晚的已知可用基线 `a0299c6`**，只重新应用真正需要的：

| 保留 | 原因 |
|---|---|
| `fccebee` 属性页按钮失效修复 | 用户明确需要 |
| `1849326` + `b6e6504` erf: 注册分版本（Win11 不注册 `DelegateExecute`） | 否则 `erf:` 地址栏报参数错误；纯脚本改动，与崩溃无关 |

| 丢弃 | 原因 |
|---|---|
| `a3904de` `ParseDisplayName` 前导分隔符 | 实测 Shell 传进来的名字**没有前导分隔符**（`[PARSE] enter level=0 name='wsl:/...'`），地址栏直达并不依赖它 |
| `7f7891b` / `30cc8a9` / `8f88d1a` | `erf:` 协议的参数容错与诊断日志，暂不需要 |

以上全部改动保留在 `backup/today-before-rollback` 分支与 stash 中，没有丢失。

> **注意**：本文档上方「根因（地址栏输入路径）：`PathFindNextComponent` 只认反斜杠」一节描述的是
> `a3904de`，该修复已随本次回退撤销。那个结论本身仍然成立（前导分隔符确实会切出空组件），
> 但它**不是**当时症状的原因 —— 当时 `ParseDisplayName` 根本没被调用。

### 地址栏直达不受回退影响

`易远传/WSL/R` 与 `::{CLSID}\WSL/R` 走的是 Shell 命名空间解析（`ParseDisplayName` 收到的名字
不带前导分隔符），与 `a3904de` 无关，回退后应照常可用。

---

## 崩溃专项：Explorer 0xC00000FD 栈溢出 + 目录不刷新 + 下载阻塞（2026-09-20）

状态：已修复并提交（3569ec2），本机 Win10 真机验证通过；待用户手工回归 mkdir/删除/下载三个 UI 动作。

### 1. 诊断结论：explorer 里跑的就是当前构建，不是旧 DLL

- `scripts/clean-state.ps1` 有两个自带 bug：缺 BOM（PS 5.1 按 GBK 解析直接报 ParserError）
  与硬编码安装目录 `D:/Program/ExplorerRemoteFs`（实际在 `%LOCALAPPDATA%/ExplorerRemoteFs`）。
  两处已修：补 BOM、安装目录从四个 CLSID 的 InprocServer32 反推。
- 注册表反推 + 逐字节比对：安装目录 DLL 与仓库构建产物除链接时间戳/校验和外完全一致
  （470528 字节，.text 逐字节相同）。“跑旧代码”假设不成立。
- 本机事件日志：Win10 explorer.exe，fault 模块即该 DLL；本机偏移 **0x4d737**
  （任务里记的 0x4e3b7 是另一份构建的偏移）。五次崩溃偏移完全相同。

### 2. 崩溃调用栈 + 根因

- DLL 内置 VEH 在 `%TEMP%/rfs-explorer-crash.dmp` 留了 dump；自研 minidump 解析结果：
  真实异常 **`0xC00000FD`（栈溢出）**，fault 地址 = DLL 基址 + 0x4d737 = CRT `__chkstk`；
  WER 报的 0xc000041d 只是外层回调包装。崩溃线程栈回溯：
  `CMenu::InvokeCommand（删除）` → `DeleteSelectionWithNativeFileOperation` →
  `QueryInterface` → `EnumObjects` → `Initialize` → `vector<ITEMDATA>`/`operator new` → `__chkstk`。
- 等价构建（同源码同选项、只加 /MAP，.text 与崩溃 DLL 逐字节一致）的 MAP 定位：
  `CFolderViewImplEnumIDList::Initialize` 序言 `mov eax,0x72250; call __chkstk`，
  即 **FTPSITE sites[256]（约 466KB）** 的栈帧；返回地址 +0x10 与 dump 完全吻合。
  /Od 下函数帧覆盖全部分支，所以每次 Initialize 调用都预留半兆栈，
  删除经 IFileOperation 的深调用链下来必爆。`RunFtpList` 另有一个 157KB 的死代码栈帧（已无调用）。
- 症状 2/3（mkdir 不刷新、进新目录无文件）：`RefreshLocalFast` 调 `FtpPrefetchQuiet` 时不带
  notify PIDL，而在途去重把随后 `EnumObjects` 带来的 notify 直接丢弃 —— 预取完成无人通知，
  视图卡死在“正在载入…”占位条目。另有长期风险：在途键若因异常泄漏，之后永远返回占位条目。
- 症状 5（复制/下载阻塞）：菜单的下载/打开/编辑/复制到剪贴板在 Explorer UI 线程同步 `RunCli(get)`。
- 附带：另有一条 `ExplorerDataProviderFtp.dll_unloaded` 致 svchost 崩溃的记录 —— 模块引用计数缺口。

### 3. 修复（提交 3569ec2）与验证

- `Initialize` 的站点数组改堆上 `vector<FTPSITE>`；删除死代码 `RunFtpList`。
- 预取：在途键挂起迟到 notify、完成时全发；线程体 try/catch + 必调 `FtpPrefetchEnd`；失败也通知（视图重枚举重试）。
- 下载三件套阻塞 GET 搬进工作线程（存盘对话框仍在 UI 线程，worker 消息框用 NULL owner）。
- 止血：`ParseDisplayName/EnumObjects/GetDisplayNameOf/GetAttributesOf/BindToObject/`
  `CreateViewObject/SetNameOf/GetUIObjectOf` 加 function-try-block（catch(...) → E_FAIL），编译切 `/EHa`；
  枚举器与视图回调补 `DllAddRef/DllRelease`。
- `compile.ps1`：加 `/DEBUG /OPT:REF/ICF /MAP`（此前只有 `/PDB:`，链接器根本不生成符号；
  注意 /DEBUG 会改 /OPT 默认，必须显式写回 REF/ICF 否则代码布局改变）。安装包只收 DLL，符号不外泄。
- 验证：
  - 自研 `tmp/explore-remote-files/harness/ErfHarness2.exe`（进程内加载 DLL、小栈 + 700KB 欠栈跑完整枚举）：
    旧 DLL 必现 `0xC00000FD@0x4D737`（复现 dump 与线上逐字节同构），新 DLL 通过（8 个子项）。
  - 反汇编确认新 DLL 的 `Initialize` 不再调用 `__chkstk`；其余探测点最大 81KB（`QueryContextMenu`，正常量级）。
  - `build-release.ps1` → `build-inno.ps1`（Erf-0.1-Alpha-Setup.exe，131MB）→ 静默安装
    （sha256 与随包一致）→ 重启 Explorer，真机浏览/右键/取文件无崩溃，`[ENUM] warm-cache` 与 prefetch 回填正常。
- 未覆盖、需手工回归：新建文件夹后刷新、双击进入新目录、删除、菜单下载大文件（均需点 UI）。
- 已知残留（非回归）：大数据对象展开仍在 UI 线程同步做（`CRemoteDataObject::ExpandIfNeeded` 的 eager 路径），
  超大目录复制时窗口仍会假死几秒；架构级改造（流式 FileGroupDescriptor）留给后续。

### 4. 取证工具链教训（给下次）

- PowerShell 传参会被吃 `$` 与反斜杠：诊断命令一律写成 `tmp/` 下的 `.ps1` 文件再 `-File` 调用。
- 本机无 CDB/WinDbg，但 VEH + 自研 minidump 解析 + /MAP 等价构建足以定位到函数与帧大小。
- `/DEBUG` 会改变代码生成：要做地址映射必须用“同选项只加 /MAP”的构建，并用 `.text` 逐字节比对确认等价。
- `CreateThread` 的小栈请求可能被忽略（实测给了 1MB 预留）：回归测试不要信参数，要读 TEB/压到足够深。

---

## 实测反馈第二轮：服务 GUI 复制冻结 + B/b 大小写混淆（2026-09-20）

状态：已修复并提交（1edb5ab），已安装到本机；B/b 端到端验证通过，服务 GUI 节流待用户复制时确认。

### 1. 服务 GUI 在复制期间冻结（暂停/取消点不了）

- 传输本身在后台线程，但 SFTP 按块进度回调经 `BeginInvoke` 以 Normal 优先级打进 UI 线程；
  WPF 里 Normal 高于 Input，快传时 dispatcher 被进度更新打满，按钮点击被饿死。
- 修复（`TransferTaskService.cs`）：进度推送按任务合并为 150ms 一次（CLI 管道 P 帧与
  managed 任务两条路都收敛）；完成/失败/取消走原路径不受影响；任务结束清合并记录。
- 验证：随安装包更新服务并重启；复跑溢出回归通过；暂停/取消手感需用户复制大文件时确认。

### 2. B/b 同名不同大小写目录浏览混淆

- 现场：服务端建 B/b、删 B 都成功；但进 b 显示 B 的旧列表，里面文件打不开也删不掉。
- 根因（native 磁盘缓存，双缺陷）：文件名哈希对 `site|folder` 全串 `towlower`，B/b 落到
  同一个缓存文件；且文件头只有 magic/version/count、无身份字段，加载从不校验归属。
  内存缓存与服务端（全 Ordinal 比较）本来都是大小写敏感的，只有这一层折叠。
- 修复（`FtpMeta.h`）：哈希去掉折叠；文件头升级 v2 携带 site+folder，加载强校验，
  拒收 v1/冒名/截断文件并删除（存储是 temp+move 原子，不会误删半成品）。
  旧折叠文件在首次加载时自愈删除；小写纯路径有一次重拉成本。
- 验证：
  - `tmp/explore-remote-files/harness/ErfCaseTest.exe` 10/10：哈希区分、双向隔离、冒名拒收、旧版清理。
  - CLI 建 `test/erf-case-B`、`test/erf-case-b` 各加标记文件，测试桩完整下钻：
    首屏冷占位、12 秒后暖列表各为 marker-B/marker-b，零串扰；随后 CLI 清理测试目录。
  - 注意：CLI 经旁路建目录不经过 shell 缓存失效，验证前清了一次磁盘快照目录（属测试方法，
    真实 shell 内 mkdir 走补丁+失效管线，不受影响）。

---

## 实测反馈第三轮：取消崩溃 + 卸载竞态 + 源端冻结（2026-09-20 夜，待明天）

用户实测结论：大小写问题已解决；复制冻结转移到发起端（远程目录窗口）；
服务 GUI 不卡死但暂停/取消仍不可点，且点取消后服务程序崩溃。

### 1. svchost 内 dll_unloaded 崩溃（已定位，未修完）

- 00:51 新事件：`svchost.exe_cbdhsvc`，`ExplorerDataProviderFtp.dll_unloaded` +
  `0xc0000005`，模块时间戳 `0x6aaebba6` 与当前安装版一致 —— 卸载竞态在新版依然存在。
- 机理：剪贴板历史服务（cbdhsvc）检查剪贴板上的我们的数据对象时把 DLL 载入 svchost；
  `DllCanUnloadNow` 只数 COM 对象引用，后台线程（预取/刷新/通知/下载/编辑监视等 14 处）
  不 pin 模块；末对象释放后线程仍在跑即崩。3569ec2 的对象级引用计数堵不住这一层。
- 已提交 3ef54d4（WIP）：只做了 `FtpMeta.h` 三处（Runner/Refresh/Prefetch），编译通过；
  ContextMenu、RemoteDataObject、PropSheetProbe 的 pin 明天继续。

### 2. 点取消服务崩溃（待明天，初判方向）

- 现象：队列窗口点取消后 RemoteFsClient 进程崩溃。节流改动本身只加锁+字典读写，
  不太像直接肇事；怀疑 `Cancel → Kill(CLI)/取消回调` 与随后到达的 E 帧/完成回调竞态，
  或队列窗口取消回调内的空引用。明天先拿崩溃 dump（服务进程 dump）+ rfs-tasks.log 对时间线。

### 3. 源端 Explorer 冻结（已知残留，待架构级修）

- 即 `CRemoteDataObject::ExpandIfNeeded` 的 eager 全量展开占 UI 线程（复制源窗口假死几秒，
  但能恢复、不崩溃）。流式 FileGroupDescriptor 改造留给后续，不在这轮动。

### 明天调试计划

1. 补完剩余线程 pin，编译安装；
2. 复现取消崩溃并抓服务进程 dump，定位后修；
3. 用户确认：B/b 导航、暂停/取消手感、大文件菜单下载。

---

## 实测反馈第四轮：点取消崩溃根因 + 暂停闸门 + 线程 pin 补完（2026-09-20 白天）

状态：已修复并提交 `1b923b3`，本机已重新构建、打包并安装；`RemoteFsClient` 已以新构建重启
（暂停/取消修复已生效），`explorer.exe` 需重启/注销后线程 pin 才生效。回归脚本三条全过。

### 1. 点「取消」导致服务进程崩溃：根因（有事件日志实锤）

Windows 事件日志（Application，`RemoteFsClient.exe`）两次记录：

```
Application: RemoteFsClient.exe ... Exception code: 0xe0434352
.NET Runtime: The process was terminated due to an unhandled exception.
Exception Info: System.OperationCanceledException: The operation was canceled.
   at System.Threading.CancellationToken.ThrowOperationCanceledException()
   at Renci.SshNet.SftpClient.ThreadPoolProgress`1.<>c.<System.IProgress<T>.Report>b__2_0(Object state)
   at System.Threading.QueueUserWorkItemCallback.Execute()
   at System.Threading.ThreadPoolWorkQueue.Dispatch()
```

这是 SSH.NET 2026.0.0 的既定行为（已对照其源码）：

- `SftpClient.DownloadFile(path, stream, callback)`（同步重载）会 `new ThreadPoolProgress<...>(...)`
  包装回调，`ThreadPoolProgress.Report` 把回调 **丢到线程池**执行，并且把 token 写死为
  `CancellationToken.None`——它本身没有任何取消能力。
- 我们原先把 `token.ThrowIfCancellationRequested()` 放进那个回调里"靠抛异常中断下载"。结果：
  取消时线程池上的回调抛 OCE = **线程池未处理异常 = 进程直接死**；而且下载循环根本不看回调，
  这个抛出**中断不了下载**，只会炸进程。

同时 `ThreadPoolProgress` 也让回调执行在线程池上、顺序不定——任何回调里的异常都是致命的。

**修复**（`SftpFileSystem.Download`）：改用
`_client.DownloadFileAsync(remotePath, fs, IProgress<DownloadFileProgressReport>, token)`。
`InternalDownloadFile` 在下载循环里**直接** `downloadProgress.Report(...)`，并让 `ReadAsync` /
`WriteAsync` 观察同一个 token；取消时 OCE 由 `GetAwaiter().GetResult()` 在**我们的线程**上抛出，
被上层 `catch (OperationCanceledException) when (cancellation.IsCancellationRequested)` 按用户取消处理。
回调经新增的 `SafeProgress<T>`（`Providers/SafeProgress.cs`）包装，**永不抛出**。`Upload` 的
同步回调也做了同样包装（同步 `UploadFile` 同样走 `ThreadPoolProgress`）。

### 2. 传输队列「暂停」对托管下载是空操作

`FETCH` / `FETCHDIR` 由常驻服务自己执行，没有 CLI 进程，因此 `JobReporter` 也不会创建 named gate；
`TransferTaskService.TogglePause` 去 `OpenExisting` 那个名字必然失败、异常被吞 —— 点「暂停」没有任何反应。

**修复**：服务自己为每个托管任务持有 `ManualResetEventSlim`（`_managedGates`，signaled = running）：

- `TogglePause`：托管任务直接 Set/Reset 自己的闸门；CLI 任务仍走原 named gate 路径；
- `WaitWhilePaused(task, token)`：由下载线程在**进度回调里**调用并阻塞（这就是"暂停"），
  每 100 ms 醒来一次检查取消，取消时立即返回、不抛异常；
- `FetchAsync` / `FetchDirAsync` 的进度回调（含目录任务逐个文件的回调）都接上它。

取消回调也一并收严：`Cancel(task)` 先 `MarkCancelRequested()`，再把真正的
`cancellation.Cancel()` 交给线程池执行（`Cancel()` 会**在调用线程上**内联跑注册的回调，
不能让它卡在 UI 线程），并且 `catch { }` 兜底，绝不让它逃进 WPF dispatcher。

### 3. 后台线程 pin（模块引用计数）

`DllCanUnloadNow` 只数 COM 对象；凡代码里自行 `CreateThread` 的线程都必须在模块上各持一份引用，
否则末对象释放、DLL 卸载后线程仍在跑 → `svchost(cbdhsvc)` 里的 `dll_unloaded + 0xc0000005`。
本轮补齐（`FtpMeta.h` 三处在 `3ef54d4`）：

| 文件 | 创建点 |
|---|---|
| `ContextMenu.cpp` | DeleteRemoteThreadProc、MetaWarmThread、EditWatch、DownloadJobProc、DownloadBatchProc、ClipJobProc、ChmodRemoteThreadProc、TerminalFocusThread（PasteJobProc 原已 pin） |
| `PropSheetProbe.h` | SamplerProc（并在头里前置声明 DllAddRef/DllRelease） |
| `RemoteDataObject.h` | `CFolderFetch` 构造/析构各一份（它自己持有 fetch 线程） |

### 4. 回归（新增，可复跑）

`tmp/explore-remote-files/harness/`：

- `erf-sftp-server.py`：可用的本地 SFTP 测试服务器（`research/ftpserver/run_sftp.py` 只能列举，
  其 `open` 实现是坏的，无法下载）。
- `ErfCancelTest/`：真实 `SftpFileSystem` + 真实 SSH.NET 跑三条 ——
  ① 传输中取消：4 MB 处取消 → 抛 OCE、半截文件删除、**进程存活**；
  ② 暂停/恢复：4 MB 处暂停，1.2 s 内进度字节不变，恢复后完成到 67,108,864 B；
  ③ 暂停中取消：33–48 ms 内结束。

**证明回归有效**：把 `Download` 临时换回旧实现，同一测试立刻以
`Exception code: 0xe0434352` 崩溃，栈与线上事件日志逐帧一致。

### 5. 仍未做：源端 Explorer 冻结（架构级）

`CRemoteDataObject::ExpandIfNeeded` 在调用 `GetData(CFSTR_FILEDESCRIPTORW)` 的线程上**同步**展开
整棵远程树（`ExpandInto` 对每个未缓存目录发一次桥接 LIST）。Shell 的虚拟文件剪贴板协议要求
描述符一次性完整返回，所以不能"先返回后补"。真正的解法是让这段展开不占 UI 线程：
实现 `IDataObjectAsyncCapability`（旧名 `IAsyncOperation`，IID `{3D8B0590-F691-11d2-8EA9-006097DF5BD4}`）
让 Shell 在后台线程取描述符，或改走自研复制引擎（`ITransferSource`）。
本轮**未改**——它对"可用性"的影响是"假死几秒后能恢复"，而改动本身风险较高，需要实机 UI 验证，
待与用户确认路线。

---

## 实测反馈第五轮：粘贴端 Explorer 冻结 = UI 线程同步展开整棵树（2026-09-20 续）

状态：已修复并提交 `08ffcf9`，本机已重建/重装/重启（服务 9:54:15、explorer 9:54:16）。

### 1. 先确认：暂停/取消这次是好的

`rfs-tasks.log`：

```
09:46:45.263 PAUSE managed id=32d7b302... paused=True
09:46:57.301 PAUSE managed id=32d7b302... paused=False
09:47:00.797 PAUSE managed id=32d7b302... paused=True
09:47:01.911 fetch cancelled ... cleaned=True bytes=4811840520/4821300010
09:47:01.911 END managed id=32d7b302... status=cancel
09:47:03.401 BATCH END done=0 failed=0 cancelled=1
```

暂停、恢复、再暂停、取消都按预期走了，且服务进程存活 —— 上一轮的目标达成。

### 2. 取证：粘贴端窗口为什么假死

探针输出在 **`%LOCALAPPDATA%\ExplorerRemoteFs\logs\remotefs-debug.log`**
（不是 `%TEMP%`；`ProbeLog` 的位置见 `ProbeLog.h` 顶部注释）：

```
[3952046] [DATAOBJ] GetData enter fmt=0xC0DF suppressed=0 tid=9632
[3952046] [DATAOBJ] cold '/home/zhou/AI_work/test/big-1'
[3952109] [DATAOBJ] expand COLD cacheOnly=1 tid=9632 dirs=1 coldDirs=1 items=1 elapsedMs=63
[3954859] [DATAOBJ] expand done cacheOnly=0 tid=9632 tops=1 dirs=1 coldDirs=0 items=26536 elapsedMs=2750
[3954859] [DATAOBJ] GetData descriptors n=26536
```

同一时刻 `rfs-shell-watch.log`：`CHANGED hung 0 -> 1 ... cls='CabinetWClass' title='test'`
（09:48:34→38，以及 09:48:52→58）。

**结论**：Shell 在**发起/粘贴窗口的 UI 线程**上查询 `CFSTR_FILEDESCRIPTORW`；我们必须在
返回前枚举整棵树，冷目录每多一个就多一次同步桥接 LIST。这个测试目录有 26535 项、无子目录，
**一次 LIST 就占住 UI 线程 2750 ms** —— 与窗口 `hung` 的时间点完全吻合。
（`fmt=0xC0DF` = `CFSTR_FILEDESCRIPTORW`。）

另外 `[DATAOBJ] fetch wait failed 'big-1\f-93503.txt'` 说明：用户在队列里取消后，
`GetData(CFSTR_FILECONTENTS)` 拿不到文件，原来回 `STG_E_READFAULT`，资源管理器就报了
「移动文件或文件夹时出错」。

### 3. 修复（`08ffcf9`）

- `FtpMeta.h` 新增 `FtpPrefetchTreeQuiet(site, folder, maxDirs=512, maxEntries=300000)`：
  后台递归把一棵子树逐个列进内存+磁盘缓存。共用 `FtpPrefetchBegin/End` 去重表（键
  `tree|site|folder`），单个目录失败即跳过，线程自己 `DllAddRef/DllRelease`。
- `CRemoteDataObject::Prewarm()`：数据对象一建立（Ctrl+C / 拖拽开始 / 菜单探测）就在后台
  预热所选中文件夹的子树；`ExplorerDataProvider::GetUIObjectOf(IDataObject)` 在 Add 循环后调用。
  这样"复制 → 粘贴"之间用户思考的那几秒就把列表拉完，粘贴时展开命中缓存。
- `GetData(CFSTR_FILECONTENTS)`：当 `CFolderFetch::Cancelled()` 为真时返回
  `HRESULT_FROM_WIN32(ERROR_CANCELLED)`，资源管理器报"已取消"。

### 4. 残余与验证

- **残余**：若"复制后立刻粘贴"、且该目录从未被浏览过，第一次展开仍要等一次 LIST（预热尚未跑完）。
  这是 Shell 剪贴板虚拟文件协议要求"描述符一次性完整返回"的固有代价；要做到彻底不阻塞，
  需要改成异步数据对象或自研复制引擎，属后续工作。
- **验证**：重放一次"复制远程大目录 → 粘贴"，日志里应能看到
  `[WARM] tree site='...' root='...' dirs=... entries=...`（预热）与
  `[DATAOBJ] expand done ... elapsedMs=`（展开耗时）。命中预热时 `elapsedMs` 应降到几十毫秒量级。

---

## UI 线程同步 I/O 盘点 + A 批：菜单/属性页写操作全部后台（2026-09-20 续）

用户定调：**所有传输、权限修改、删除都必须在后台进行**。据此把整条链路按
"会不会在 Explorer UI 线程上做同步 I/O"审了一遍，并先完成 A 批。

### 1. 盘点（改动前）

**A. 菜单/属性页写操作 —— UI 线程直接 `RunCli`**（`CreateProcessW` + `WaitForSingleObject`，
默认 30 s，且每次新进程 + 新连接）：

| 位置 | 操作 |
|---|---|
| `ContextMenu.cpp` 属性页「确定」/`PSN_APPLY` | 非递归 `chmod`（两份实现） |
| `ContextMenu.cpp` `PermApplyChown` | `chown` |
| `ContextMenu.cpp` `NewFolderRemote` | `mkdir` |
| `ContextMenu.cpp` `DoRename` | `rename` |
| `ContextMenu.cpp` `ServerMove` | 逐项 `rename` |
| `ContextMenu.cpp` `CopyToRemoteFolder` | `dup` |
| `ContextMenu.cpp` `ServerCopy` | 复制到其他站点/本地：同步 LIST + `get` + `put` + `CopyFile` |

**B. Shell 枚举/解析的同步 LIST**：`ExplorerDataProvider.cpp:2671`
（`EnumIDList::Initialize`，删除预扫描/复制的 STORAGE 枚举，冷目录）、
`ExplorerDataProvider.cpp:765`（`ParseDisplayName` 命中不了最近快照时）。

**C. 数据对象（复制）**：`GetData(CFSTR_FILEDESCRIPTORW)` 同步展开（已由 `Prewarm` 缓解）；
`GetData(CFSTR_FILECONTENTS)`→`WaitForFile`；`CRemoteStream::Ensure()` 阻塞到整个文件下完。

**已经在后台的（对照）**：删除（`StartDeleteRemote` 线程 + 服务 `DELETE`）、
递归 `chmod`（`StartChmodRecursiveAsync` + 服务 `CHMOD`）、下载/打开/编辑/剪贴板
（`DownloadJobProc`/`DownloadBatchProc`/`ClipJobProc`/`EditWatch` 线程）、`FETCH`/`FETCHDIR`（服务托管）。

### 2. A 批改造（提交 `e2011e0`，`ContextMenu.cpp` +242/-54）

- 新增通用 **`BgCliJob`**：`enum BgAfter { BG_NONE, BG_REFRESH, BG_PATCH_ADD, BG_PATCH_RENAME }` +
  `std::vector<BgCliStep> steps`。UI 线程只投递参数副本；工作线程顺序执行 `RunCli`；成功后
  新建目录走 `FtpCachePatchAdd`+`RefreshLocalFast`、重命名走 `FtpCachePatchRename`+`RefreshLocalFast`、
  其余走 `AfterRemoteMutation`；失败用 NULL owner 弹窗；线程自 `DllAddRef/DllRelease`；
  多条目任务逐项尝试、最后如实报失败。
- `ServerCopy` 用专用 `BgCopyCtx`/`BgCopyThreadProc`（跨站点/复制到本地要
  `ReadRemoteMeta` + `get`/`put`/`CopyFile`，整体搬到工作线程）。
- **核对**：改完后 `grep RunCli(` 的所有调用点都只在工作线程上
  （`BgCliJobProc`/`EditWatch`/`DownloadJobProc`/`DownloadBatchProc`/`ClipJobProc`/`PasteJobProc`/
  `ChmodRemoteThreadProc` 回退/`BgCopyThreadProc`）。
- 已重建/重装/重启（服务与 explorer 均 10:16:43）。

### 3. 行为变化（需要体感确认）

新建文件夹 / 重命名 / 移动 / 复制 / 改权限现在是"命令先返回、稍后生效"；
失败提示由后台线程以 NULL owner 弹出；成功后的视图刷新仍走 `AfterRemoteMutation` 管线。

### 4. 后续批次

- **B 批**：`EnumIDList::Initialize`（STORAGE 枚举）与 `ParseDisplayName` 的同步 LIST 改
  "缓存优先 + 后台预热 + 通知"。
- **C 批**：跨站点/本地复制改走常驻服务传输队列（`FETCH` + 上传/跨站点桥接），DLL 里不再跑 CLI 传输。
- **D 批**：`IStream` 同步下载（`CRemoteStream::Ensure`）要彻底不阻塞需自研复制引擎
  （`ITransferSource` 已有雏形）。

---

## B+C 批：枚举路径去同步 LIST + 传输统一走常驻服务（2026-09-20 续）

提交 `1a67145`（7 文件 +161/−29），已重建/重装/重启（服务与 explorer 均 10:31:52）。

### B. 枚举 / 解析路径

- **核实结论**：`CFolderViewImplEnumIDList::Initialize` 的网络 `FtpListCachedAll` 对远程目录
  **不可达**。`EnumObjects` 对 `m_nLevel >= 1` 一律**播种**枚举器：命中快照用快照、
  冷目录用"正在载入…"占位条目 + `FtpPrefetchQuiet`，两支都 `return`。原来那段
  "cold + transfer/storage 同步列举"的注释与代码是死代码 —— 已改写为明确说明，并加
  `[ENUM] WARN unseeded sync-list reached ...` 防回归日志（真被走到就是在调用线程上同步拉网络）。
- `Initialize` 的未播种分支也加了 `[ENUM] Initialize unseeded network LIST ...` 日志。
- `ParseDisplayName` 身份解析正常从 `m_recentItems` 快照命中（零 I/O）；落到同步 LIST 时记
  `[PARSE] slow LIST ... elapsedMs=`，用于确认残余规模。
- **结论**：删除预扫描/地址栏解析的 UI 阻塞在当前代码里已不存在（属死代码/极少路径）。

### C. 传输统一走常驻服务

原来菜单的下载/上传仍用 `RunCli(get/put)`（每次新进程 + 新连接）。现在：

- 服务桥接新增 **`PUT`**（`RemoteBridgeService`）：与 `FETCH` 完全同形 —— 立刻回
  `STARTED <jobId>`，上传在后台跑，进**传输队列**（进度、暂停/取消、`FETCHSTATUS`、`CANCEL` 复用）。
- `IRemoteFileSystem.Upload` 增加 `CancellationToken`：
  - `SftpFileSystem.Upload` 改走 `UploadFileAsync(...)`（同步重载同样经 `ThreadPoolProgress`
    在线程池执行回调，抛异常会终止宿主进程 —— 与 `Download` 同一个坑）；
  - `FtpFileSystem.Upload` 在 FluentFTP 进度回调里检查 token。
  - ⚠ **上传不做暂停闸门阻塞**：SSH.NET 的上传进度回调跑在**会话消息线程**上，阻塞会卡死整条连接；
    上传只支持取消（代码注释已写明）。
- DLL 侧新增 `FtpBridgePut`，并把仍用 `RunCli` 的**传输**全部改走桥接：
  `DownloadJobProc` / `DownloadBatchProc` / `ClipJobProc` / `BgCopyThreadProc`(下载段) → `FtpBridgeFetch`；
  `PasteJobProc` / `BgCopyThreadProc`(上传段) / `UploadEditedFile` → `FtpBridgePut`。
  每次用户级传输生成唯一 `batchId`（`MakeTransferBatchId`），队列分组与取消不再互相连坐。
- 保留 `RunCli` 的只剩**元数据操作**（`touch`/`dup`/`rename`/`mkdir`/`chmod`/`chown`/`chmodr`），
  且都在工作线程上（A 批成果）。

### 回归

`tmp/explore-remote-files/harness/ErfCancelTest`（配 `erf-sftp-server.py`）四条全过：
下载取消、暂停/恢复、暂停中取消、**新增的上传取消**（4.2 MB 处 29 ms 结束，进程存活）。

### 剩余（D 批）

`CRemoteStream::Ensure()` 会阻塞到整个文件下完（`FtpBridgeFetch` 等终态），由
`IStream::Read/Seek/CopyTo` 调起；要彻底不阻塞需自研复制引擎（`ITransferSource` 已有雏形）。

---

## 实测反馈（A/B/C 之后）：写操作全过、下载达标；上传暂停与 Ctrl+C 复制（2026-09-20 续）

用户逐条实测：

| 项 | 结果 |
|---|---|
| A 批写操作（新建/重命名/移动/副本/改权限/所有者/删除） | ✅ 全部正常 |
| 右键下载 | ✅ 不阻塞、暂停/取消随时可用 |
| Ctrl+C 粘贴上传到远程 | ⚠ 不阻塞、取消正常，**暂停无效** |
| 右键复制到本地文件夹 | ⚠ 同上（不阻塞、暂停无效、取消正常） |
| Ctrl+C 从远程复制到本地 | ❌ **阻塞源端（远程目录）Explorer 窗口**，暂停无效；取消正常 |

### 1. 上传暂停：修好（提交 `8a34292`）

上一版为安全刻意没接暂停闸门 —— SSH.NET 的**上传**进度回调跑在**会话消息线程**上
（`InternalUploadFile` 在 FXP_WRITE 响应回调里 `uploadProgress.Report`），在那里阻塞会把
整条连接卡死。这次改挂在**输入流**上：

- 新增 `Providers/PausableReadStream.cs`：每次 `Read/ReadAsync` 前调用一次 `waitWhilePaused()`。
  上传循环在**它自己的线程**上 `await input.ReadAsync(...)`，阻塞只暂停这次上传。
- `IRemoteFileSystem.Upload` 增加 `Action? waitWhilePaused = null`；
  `SftpFileSystem.Upload` 用它包住 `FileStream`；`FtpFileSystem.Upload` 在 FluentFTP 进度回调
  （传输线程）里调用它；`RemoteBridgeService.PutAsync` 传入 `WaitWhilePaused`。
- 回归：`ErfCancelTest` 新增"上传暂停/恢复"，**五条全过**（上传在 6.0 MB 处停住，恢复后完成）。

### 2. Ctrl+C 复制阻塞源端窗口：已加取证探针，待日志确认

`RemoteDataObject.h`：

- `Ensure` 起始/完成日志加 `tid` 与 `elapsedMs`；
- `GetData contents` 日志加 `tid`；
- `CFolderFetch::WaitForFile` 超过 500 ms 记 `[DATAOBJ] WaitForFile slow tid=... elapsedMs=...`。

**初判**：Ctrl+C 时 Shell 在**源端窗口的 UI 线程**上取 `CFSTR_FILEDESCRIPTORW`。若选中的是
**文件夹**且子树冷，`ExpandIfNeeded` 会在该线程上同步 LIST，于是源窗口假死（单文件不需要 LIST，
所以不受影响）。`Prewarm` 只在数据对象建立时才开始后台预热，赶不上紧随其后的这次查询。

这是 Explorer 剪贴板虚拟文件协议（`CFSTR_FILEDESCRIPTORW` 必须一次性完整返回）的固有代价，
解法在 Shell 集成侧：让 Explorer 走我们已实现的 `ITransferSource`，或实现异步数据对象
（`IDataObjectAsyncCapability`）。**换传输引擎（SSH.NET/FluentFTP/WinSCPnet）都无济于事** ——
引擎只负责搬字节，这一步是 Explorer 定的接口。

---

## Ctrl+C 复制日志取证：展开 2.6 s 阻塞源线程 + 文件夹复制落盘布局 bug（2026-09-20 续）

提交 `2ab8f0f`，已重建/重装/重启（服务与 explorer 均 12:15:24）。

用户实测：Ctrl+C 复制**单文件与文件夹都会**阻塞源端 Explorer 窗口，并弹出「计算复制时间」
的 Win7 样式窗口。日志（`remotefs-debug.log`）取到：

```
[12494656] [DATAOBJ] GetData enter fmt=0xC0DF suppressed=1 tid=7480
[12494656] [DATAOBJ] cold '/home/zhou/AI_work/test/small-5'
[12494734] [DATAOBJ] expand COLD cacheOnly=1 tid=7480 dirs=1 coldDirs=1 items=1 elapsedMs=78
[12494734] [DATAOBJ] cold while probing -> refuse formats tops=1
[12495218] [WARM] tree site='WSL-SFTP' root='/home/zhou/AI_work/test/small-5' dirs=1 entries=4469
[12497437] [DATAOBJ] expand done cacheOnly=0 tid=7480 tops=1 dirs=1 coldDirs=0 items=4470 elapsedMs=2656
[12497437] [DATAOBJ] GetData descriptors n=4470
[12497531] [XFER] fetch started op='FETCHDIR' remote='.../small-5' job=...
[12511218] [XFER] fetch terminal=done op='FETCHDIR' elapsed=13687
[12511218] [DATAOBJ] fetchdir(service) dir='.../small-5' -> '...rfs-copy-1116-3' state=1
[12511687] [DATAOBJ] fetch wait failed 'small-5\f-7723.txt'
```

### 1. 阻塞确认：`expand` 在源端调用线程上同步 LIST 2656 ms

Shell 在源端窗口线程（`tid=7480`）上取 `CFSTR_FILEDESCRIPTORW`；`ExpandIfNeeded` 走到
`cacheOnly=0`，对 4469 项的扁平目录发了一次同步网络 LIST，占住该线程 **2656 ms**。
同一次展开里「后台预热（`[WARM] tree`，约 0.5 s）」与「同步 LIST」各拉了一遍。

**初版缓解**：`FtpListCachedAll` 增加 `waitForWarm` —— 当 `tree|site|dir` 的预热在跑时，
最多等 5 s 让它把内存缓存填好再返回；预热线程自身调用不带该标志（否则会等自己）。
命中时打 `[CACHE] expand waited for warm ...`，超时打 `expand warm wait timed out`。

### 2. 顺带发现：文件夹复制**落盘布局错误**（复制实际失败）

CLI 的 `CmdGetR` 早有注释：descriptor 的 `relPath` 带顶层文件夹名（`small-5\f-7723.txt`），
所以本地树必须是 `<localRoot>\<folderName>\...`。搬到服务后 `FetchDirAsync` 把文件直接
放在 `localRoot`，于是 `GetData(CFSTR_FILECONTENTS)` 永远找不到文件
（`fetch wait failed`）—— 文件夹复制看似在跑（队列有进度），实际拿不到数据。
已改为下载到 `<localRoot>\<folderName>\`。

### 3. 仍未解决

`expand` 仍占住源端线程（只是变短）；「计算复制时间」是 Shell 自己的预复制统计窗口
（走我们的 `EnumObjects`/`GetAttributesOf`/`GetDisplayNameOf`）。彻底不阻塞需要
**异步数据对象**（`IDataObjectAsyncCapability`）或让 Explorer 走 `ITransferSource` ——
属 Shell 集成侧，换传输引擎无济于事。

---

## A 判死 + B 落地：复制改走 ITransferSource（2026-09-20 续）

提交 `03d19fc`，已重建/重装/重启（服务与 explorer 均 13:43:42）。

### A（IDataObjectAsyncCapability）实测判死

装上后日志里**一条 `[ASYNC]` 都没有** —— Shell 从未查询该接口。
`SHCreateDataObject(pidl, cidl, apidl, inner, ...)` 的复合数据对象没有把内层对象的额外接口
透出去，所以这条路在 Win11 上走不通（不是实现问题，是接口到不了 Shell 手上）。

### 真凶：`Ensure()` 在窗口线程上等整个文件

```
[12372500] [DL] Ensure start tid=7480 ... size=6048411574
[12383031] [DL] Ensure done  tid=7480 elapsedMs=10531     ← 6.0 GB 下了 10.5 s，全程占窗口线程
[12383062] [DL] first Read ok cb=262144 ...
[17582984] [DL] Ensure start tid=7016 ...
[17600890] [XFER] fetch terminal=cancelled elapsed=17859  ← 用户取消前等了 17.9 s
```

`GetData(CFSTR_FILECONTENTS)` → `IStream::Read` → `CRemoteStream::Ensure()` 会阻塞到**整个文件**
下完；暂停会让它永远等不到终态 —— 与"按暂停就一直卡、取消/完成才解除"完全吻合。
文件夹还有 `expand done ... tid=7480 elapsedMs=2656`（同步 LIST）。

### B：复制改走 `ITransferSource`（与删除同一条路）

`CFolderTransferSource::OpenItem` 的原有注释点明了机制：**只有剪贴板对象缺少
FileGroupDescriptorW/FileContents 时，复制引擎才会走到 `OpenItem`**。

- 实现 `OpenItem`：从 `IShellItem` 取名字/类型（复用 `FVITEMID`/`IsOursItem`），大小优先用父目录
  缓存、miss 则同步列一次（**OpenItem 在 Shell 的复制工作线程上，允许阻塞**），返回 `CRemoteStream`
  —— 内容仍由常驻服务 `FETCH` 提供（进度/暂停/取消都在服务侧）。
- 一个视图 = 一个 `batchId`（`ts-pid-tick-seq`）：一次复制的文件在队列里归一组，取消只取消这一批。
- `CRemoteDataObject` 默认**不再提供** `CFSTR_FILEDESCRIPTORW` / `CFSTR_FILECONTENTS`
  （`GetData`/`QueryGetData`/`EnumFormatEtc` 三处一致），逼 Explorer 走 `ITransferSource`。

### 回退开关

```
reg add "HKCU\Software\ExplorerRemoteFs" /v UseVirtualFileFormats /t REG_DWORD /d 1 /f
```

改完重启 Explorer 即恢复旧行为（无需重装）；默认（值不存在）= 0 = 走 B。

---

## B 报 0x80004002：复制引擎取流前先要 IShellItemResources（2026-09-20 续）

提交 `ffa0430`，已重建/重装/重启（服务与 explorer 均 13:50:03）。

B 装上后 Ctrl+C / 拖拽报 **0x80004002「不支持的接口」**，"复制到…"（我们自己的菜单命令）正常。
日志实锤 —— `OpenItem` 确实被调用了，而且在 Shell 的复制**工作线程**上：

```
[XFER] OpenItem tid=5684 site='WSL-SFTP' remote='....zst' size=5819494383 riid=FF5693BE flags=0x000002A8
[XFER] OpenItem QI failed riid=FF5693BE hr=0x80004002
```

SDK：`{ff5693be-2ce0-4d48-b5c5-40817d1acdb9}` = **`IShellItemResources`**（`ShObjIdl_core.h`）。
机制：复制引擎经 `ITransferSource::OpenItem` 取源项时**先**要 `IShellItemResources`
（属性/大小/时间/资源描述），**之后**才要 `IStream`。我们第一步就不支持，于是整个复制失败。

**修复**：

- 新增 `CRemoteItemResources : IShellItemResources`（在 `RemoteDataObject.h`）：
  `GetAttributes` / `GetSize` / `GetTimes` / `GetResourceDescription` 给出已知元数据；
  默认资源即文件内容流 —— `OpenResource(IID_IStream)` 返回 `CRemoteStream`；
  `EnumResources`/`SupportsResource` 表示没有额外资源。自带 IID，不依赖 SDK 的 IID 库。
- `CFolderTransferSource::OpenItem` 先分派 `IShellItemResources`（文件与目录都给），
  再分派 `IStream`；目录只给资源、不给内容流（递归仍由 `EnterFolder` + 逐项 OpenItem 完成）。
- 文件内容、进度、暂停/取消仍全部由常驻服务 `FETCH` 提供。

**若仍报错**：日志里的 `[XFER] OpenItem tid=... riid=...` 会直接给出 Shell 下一个需要的接口，
按同样方式补即可（这是纯增量、可回退的）。

---

## 直传流（去 %TEMP%）+ 限速暂停 + D（.rfs-part）(2026-09-20 晚)

提交：`8caad86`（抽消息/唤醒/自保）、`7858ad6`（直传流）、`3075a0b`（FTP 退路）、
`7be2417`（引用计数）、`5290c18`（限速暂停）、`f56084e`（D）。
已重建/重装/重启（服务与 explorer 均 18:51:24）。

### 1. 结论：复制数据只能走 `CFSTR_FILECONTENTS`

四种 `ShellResourceMode` 档位实测中，Shell 拿到 `IShellItemResources` 后一律 `Unadvise`，
**从不来要 `IStream`**。所以虚拟文件夹的复制数据**只能**走剪贴板虚拟文件格式；
`ITransferSource` 只被用于删除/移动/元数据。B 方案（关掉 FD 走 ITransferSource）对"复制数据"是死路，
已恢复 FD 为默认。

### 2. B：直传流（`FETCHSTREAM`），去掉 `%TEMP%` 中转

`Service(SSH.NET) → 命名管道 → DLL::IStream → Explorer → 目标文件`，目标文件由 Explorer 直接写：

- 协议：`FETCHSTREAM`/site/remote/batchId → `OK <jobId>` → 同一管道上就是原始字节（**头行逐字节读**，
  否则会把文件数据吞进缓冲区）；
- 服务把 `NamedPipeServerStream` **直接当 SSH.NET 的 output stream** → 写满即阻塞 = 天然背压；
- `Release` 关管道即取消**这一个** job（不再按 batch 取消）；
- **FTP 例外**：FluentFTP 的 `DownloadStream` 需要**可 seek** 的输出流，命名管道不可 seek →
  写一小段后返回 false。FTP 退回"服务侧临时文件（`%TEMP%\rfs-stream-*.part`）再喂流"，拷完即删；
  **Shell 侧仍然没有中转**。

### 3. 两个自己造成的 bug（已修）

- **队列被立即取消**：`_released` 标记在**任何** `n>0` 的 `Release` 上都会置位 —— 包括自己
  `Read`/`Seek` 里 `RefGuard` 的 AddRef/Release 配对。改为用**引用计数**判断（`_ref<=1` 才算 Shell 走了）。
- **FTP 直传流失败**：见上。

### 4. 暂停 = 限速（`5290c18`）

真暂停会让 Explorer 判定"源停滞"，从而给窗口设忙碌光标（用户实测"暂停时光标频繁转圈"）。
`WaitWhilePaused` 改为"每块之间最多等 300 ms 再放行这一块"（约 270 KB/s）：字节继续流动 →
没有忙碌光标、窗口不卡；恢复回全速、取消立即返回。

### 5. D：自家「复制到本地文件夹」不再双写（`f56084e`）

直接下到 **`<目标>.rfs-part`** → `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)` 原子改名；
覆盖确认提前到下载之前；失败删 `.rfs-part`。跨站点复制仍需本机中转（下载后再上传）。

### 6. 评估过的其他路线（结论：不做）

- **marker + ETW/USN/minifilter 反查目标路径**：能力上成立（`Microsoft-Windows-Kernel-File` 的
  `FileIo_Create/Write/Cleanup/Close` + `FileObject` + `OpenPath`），但**启用内核 ETW 会话、读 USN
  Journal 都需要管理员**（我们是普通用户托盘程序）、ETW 会丢事件、且 Explorer 会先报"复制完成"
  而目标位置先是个假文件。**它只是在绕开剪贴板协议。**
- **私有剪贴板格式 + FILECONTENTS 兜底**：机制正确，但只有**我们自己的消费端**认私有格式；
  原版 Explorer 的普通文件夹仍回退 FILECONTENTS → 仍拿不到目标。它是**自建宿主文件管理器（C）**
  的主协议，不能替代 B。要覆盖"任意目标"就得做目标端扩展（侵入性太大）。
- **自建宿主文件管理器（类 Q-Dir，本地 pane 用公开 API `CLSID_ExplorerBrowser`）**：
  目标已知、真暂停、无双写三个问题一次消失，且不需要管理员/ETW/驱动。属产品方向决策，
  建议先做"两 pane + remote→local 直落"的 PoC 再决定。

---

## 妥协方案：复制禁用暂停 + 下载"即点即下" + 断流截断保护（2026-09-20 夜）

提交 `147fa97`，已重建/重装/重启（服务与 explorer 均 19:01:58）。

### 1. 断流截断保护（关键正确性修复）

原实现把"管道关闭"一律当 **EOF**：服务中途消失（或网络断流）时，Explorer 会把**截断的文件
报成"复制成功"** —— 最坏的一类错（静默损坏）。现在：

- **只有确认任务成功才算 EOF**（或已收满预期字节 `_pos >= _size`）；
- 否则 `Read` 返回 `STG_E_READFAULT`；状态判定还会重试 10×50 ms 消除竞态。

网络中断的完整行为：服务侧异常被 `StreamFetchAsync` 捕获 → 任务 `fail`、**不自动重试**、
**服务不崩溃**；断点续传目前没有（失败后重拷从 0 开始）。

### 2. 复制类禁用暂停（用户拍板）

暂停会让 Shell 的复制停滞 → Explorer 显示忙碌光标；限速只是缓解。改为：

- `TransferTask.CanPause`（默认 true）+ `BeginManagedTask(..., canPause:)`；
- 直传流（复制对话框驱动）`canPause:false` → 队列 UI 暂停按钮
  `IsEnabled="{Binding CanPause}"` **变灰**；`TogglePause` 再兜一道；
- 只可**等待**或**取消**。

### 3. 真暂停还给自家传输路径

`WaitWhilePaused` 由"限速"改回**真暂停**（阻塞到恢复、取消即返回、非抛出），
只由菜单下载 / 复制到… / 上传调用 —— 消费者是我们自己的工作线程，不会出现忙碌光标。

### 4. 「下载」菜单"即点即下"

不再弹保存对话框，直接下到**默认下载目录**：

```
HKCU\Software\ExplorerRemoteFs\DownloadDir   (REG_SZ)
缺省 = %USERPROFILE%\Downloads
```

同名文件自动加 `" (2)"`、`" (3)"`…，**不覆盖**用户已有文件（`UniqueLocalPath`）。
后续可在服务程序"设置"里加栏位，免手改注册表。

---

## 2026-09-20（续）传输票据 .erfdl + 缓存数据库化

### 1. 票据：Ctrl+C 只产出"任务单"

- 开关 `HKCU\Software\ExplorerRemoteFs\UseTransferTicket`（DWORD，缺省 0）——**每次现读**，
  设置页勾选即时生效；设置页在「下载」栏。
- Ctrl+C 产出的虚拟文件 = **一张票据** `ERF_<站点名>_<jobId>.erfdl`，
  内容**只有** `{"magic":"ERFDL","version":1,"jobId":"..."}`（72 字节）。
- **文件清单、来源路径、原始目标目录**只存在服务侧数据库里 → 票据被拿走也看不出内容，
  被篡改也无用（jobId 查不到即拒绝）。代价：服务侧记录丢了票据就失效。
- 双击 = `RemoteFsClient.exe --open-ticket "%1"` → 下载到**票据当前所在目录**；
  目录变了才问「迁移 / 重新下载」，只在**同名 / 大小写同名冲突**时才问「覆盖 / 保留两者(2)」。
- 票据下载走**我们自己的队列任务**：**真暂停/取消**可用；单文件先下 `<目标>.rfs-part` 再原子改名。

### 2. 缓存/票据 → SQLite

- 一个库：`<MetadataCachePath>\erf-cache.db`（WAL），**扩展 DLL 与服务共用**
  （DLL 内置 SQLite amalgamation；服务用 `Microsoft.Data.Sqlite` + `e_sqlite3.dll`）。
- 表：`dir_cache(site,path,tick,items BLOB)`、`tickets`、`ticket_items`。
- 旧版"一个目录一个 `ExplorerRemoteFs-meta-*.bin`"在首次打开库时**一次性清掉**（1069 个）。
- 实测：`dir_cache=2`（DLL 写的）、`tickets=21`/`ticket_items=147`（服务写的）。

### 3. 测试工具（`tmp/explore-remote-files/harness/`，都是临时的）

| 脚本 | 干什么 |
|---|---|
| `ticket-e2e.ps1` | 直接对服务管道跑 MKTICKET→写票→OPEN-TICKET（不依赖 DLL） |
| `ErfTicketTest.cpp` | 进程内驱动 DLL 数据对象（**注意**：本进程里 `GetUIObjectOf` 会 E_FAIL，是环境假象，真实资源管理器正常） |
| `clip-test.ps1` / `clip-ticket.ps1` | 真窗口 Ctrl+C，读剪贴板里的票据 |
| `story-test.ps1` | 复制→粘贴→双击 全流程 |
| `db-dump.ps1` | 直接 P/Invoke `e_sqlite3.dll` 查库（表、行数、样本） |

### 4. 尚未实测

- 目录变更时的「迁移 / 重新下载」、命名冲突的「覆盖 / 保留两者」两条**弹窗分支**。

---

## 2026-09-20（再续）票据决策框返工

用户反馈：弹窗正文里出现**字面的 `**`**（Markdown 加粗），而且按钮是「是/否/取消」，
跟正文描述的「迁移 / 重新下载」对不上 —— 用户没法确定哪个按钮是哪个。

### 教训

**界面文本不是 Markdown。** `MessageBox` 与 WPF `TextBlock` 都只显示纯文本，
写进去的 `**加粗**` 会原样显示。以后凡是**用户能看到的字符串**一律不写 Markdown 标记；
要强调就用「」或换行排版。同类问题一并清掉了三处（下载页票据说明、文件关联页说明、ColumnHint）。

### 做法

`TicketChoiceWindow`：**按钮文字就是选项本身**，不再用通用按钮。

| 场景 | 按钮 |
|---|---|
| 目录已改变 | 迁移到新目录 / 在新目录重新下载 / 取消 |
| 已下载过 | 重新下载 / 取消 |
| 命名冲突 | 覆盖它 / 保留两者（新的加序号）/ 取消 |

默认按钮回车即选；`Topmost` + `Activate` 保证在后台服务里也能看见。

### 测试自动化的一点经验

- 用 PS 5.1 脚本按**中文标题**找窗口会踩编码坑（脚本文件编码、控制台代码页都会坏事）——
  改成**按窗口类**（`HwndWrapper[RemoteFsClient...`）匹配，稳。
- `WScript.Shell.SendKeys` 送不进被系统拒绝前置的窗口；
  改用 **UI Automation**（`AutomationElement` + `InvokePattern`）**按按钮序号**点击，
  既不受标题/编码影响，也能顺带把按钮文字打出来核对。

### 四条分支的验证结果

| 操作 | 结果 |
|---|---|
| 目录已改变 -> [0] 迁移到新目录 | A 里的文件被搬到 B（`migrate done moved=1`，A 变空） |
| 目录已改变 -> [1] 在新目录重新下载 | C 里出现新下载的文件，**B 保持原样**（没搬） |
| 已下载过 -> [0] 重新下载 -> 命名冲突 -> [1] 保留两者 | 生成 `test (2).txt`，原 `test.txt` 未动 |
| 命名冲突 -> [2] 取消 | `open-ticket result: FAIL: 用户取消`，目标目录未变 |
