# Win11 Shell：属性页不锁 Explorer、`erf:` 精确当前标签页（2026-09-19）

> 状态：代码与安装注册已完成；等待带 VS C++ 工具链的 Win11 实机回归。

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
