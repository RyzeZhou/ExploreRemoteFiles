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
