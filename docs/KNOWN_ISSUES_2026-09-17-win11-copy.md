# Win11「复制 / 拖拽到本地」失败：执行读取操作时发生磁盘错误（待修）

来源：用户 2026-09-17 在 **Windows 11** 上实测（本开发机是 Win10 22H2 19045，复现不了）。

## 现象

- 从远程目录**复制到本地**或**拖拽到本地**时报错：**“执行读取操作时发生磁盘错误”**
  —— 这是 Windows 复制引擎的报错（Win32 30 / ERROR_READ_FAULT 一类：读源失败），
  说明失败点在**我们提供给复制引擎的读取流**上，而不是网络协议层先报的错。
- Win10 上同样操作正常（同一份 DLL、同一个远端 SFTP 站点）。

## 代码位置（已定位）

- `src-cpp/ExplorerDataProviderFtp/RemoteDataObject.h:65` —— **`CRemoteStream : public IStream`**
  这就是复制引擎下载时读取的流。
- `src-cpp/ExplorerDataProviderFtp/ExplorerDataProvider.cpp:1310` —— `CFolderTransferSource : public ITransferSource`
  （下载走 `CreateViewObject(IID_ITransferSource)`）。

## 首要假设（按可能性排序）

1. **`CRemoteStream::Read` 的 EOF / 短读契约不对**：请求长度超出剩余数据时必须返回
   **`S_FALSE`**（并给出实际读到的字节数），返回 `E_FAIL`/`STG_E_*` 会被复制引擎报成
   “磁盘错误”。Win11 的复制引擎分块大小与调用节奏和 Win10 不同（更偏向大块读、
   也更容易请求到 EOF 之外），所以 **Win10 过、Win11 崩**，这是最像的解释。
2. `Stat()` 报的 `cbSize` 与实际可读长度不一致（引擎按 Stat 的长度读满，读到 EOF 报错）。
3. `Seek`/`Clone` 未按契约实现，Win11 的引擎尝试用 Seek 定长读取。
4. `GetStream` 的 `STREAM_*` 标志/`ITransferSource::GetTransferState` 没声明支持随机访问，
   引擎按“可随机访问”处理。

## 下一步（验尸顺序）

1. 让用户提供出错前后的 `%LOCALAPPDATA%\ExplorerRemoteFs\logs\remotefs-debug.log`（含 `[XFER]` 行）
   —— 现有 ProbeLog 已记录 `CreateViewObject ITransferSource`，需要补上 `Read` 的
   请求长度 / 返回值 / EOF 判定日志。
2. 补日志后复现一次：确认是“短读返回错误”还是“读超界”。
3. 按 COM 契约把 `CRemoteStream` 修硬：短读/EOF 一律 `S_FALSE`，绝不因 EOF 返回失败；
   `Stat` 与实际长度一致；`Seek` 支持 `STREAM_SEEK_SET/CUR`（不支持时明确 `E_NOTIMPL` 而不是返回 0）；
   顺带核对 `CLocalStream`（上传方向）是否有同样的短读问题，避免 Win11 上换个方向再踩一次。
4. 修完在 Win11 上回归：单大文件（>100 MB）、多小文件（几千个）、拖拽与右键复制两种入口、上传方向。

## 相关：卸载黑屏（**已于 2026-09-18 修复** → 见 `KNOWN_ISSUES_2026-09-18-setup-explorer-black-screen.md`）

用户现象：**桌面变黑一直没恢复，卸载窗口也不见了** → 卸载器走到我们那一步（杀掉 explorer）之后
进程消失了，没完成卸载。开发机上无法用交互式方式复现（本会话启动的 GUI 进程窗口不可见/搜不到，
连不含 `[Code]` 的最小安装包也一样卡住），但**静默卸载是通的**（`src-setup/inno-test.ps1` 51 项全过）。

当时的猜测（"杀掉 explorer 这一步本身有问题"）方向是对的，但**没找到真正的机关**：
`taskkill /IM explorer.exe /F /T` 里的 **`/T` 会把安装/卸载程序自己一起杀掉** ——
双击时它就在 explorer 的进程树里（`explorer.exe → Erf-…-Setup.exe → Erf-…-Setup.tmp`）。
所以不是"卸载器卡住了"，而是"卸载器死了"。
2026-09-18 已实证并修好：安装与卸载都不再终止资源管理器，旧 DLL 改名 `.old` 绕开占用。

## 2026-09-17 追加：Win11 日志 + 代码走查后的结论（根因链已定位到具体函数）

用户提供的 Win11 日志特征：
- `[XFER] CreateViewObject ITransferSource site='WSL' folder='/home/zhou/AI_work' hr=0` **连续出现约 10 次**
  （10678578 → 10680562）——复制引擎在失败后反复重建传输源。
- `[DATAOBJ] GetData enter fmt=0xC13F` / `GetData contents idx=0 'test.txt'` —— 数据对象与
  `CFSTR_FILECONTENTS` 的流**已经交出去了**（失败文件是个小文件 `test.txt`）。
- 之后**没有任何读取路径的日志**（我们只在 CreateViewObject 打点）。

代码走查（`RemoteDataObject.h`）：
- 报错文案「执行读取操作时发生磁盘错误」= **`STG_E_READFAULT`**，而整个文件里只有两处会返回它：
  `CRemoteStream::Read`（第 99/101 行）与 `CLocalStream::Read`。
- `Read()` 的第一句就是 `if (!Ensure()) return STG_E_READFAULT;` ——
  **真正失败的是 `Ensure()`（懒下载 + 打开临时文件），却被我们报成了"磁盘读取错误"**，把真实原因盖住了。
- `Ensure()` 走 `GetTempPathW`，配合文件头注释可确认下载模型：
  「**Folder fetch: ONE 'cli getr' process downloads the whole tree into a temp tree …
  is killed (if still running) and the temp tree is deleted**」（第 198–213 行），
  并且有「Holds a ref on the owning fetch so the temp tree stays alive while the …」（第 344 行）。

**根因假设（按可能性）**
1. **fetch 生命周期 / 并发**：Win11 复制引擎并行度更高（日志里 10 次重建传输源），
   多个 stream 同时依赖同一个「整树下载」临时目录；一旦某条路径没有正确持有 fetch 引用、
   或后一次 fetch 把前一次的临时树删掉/进程杀掉，正在读的 stream 就拿不到文件
   → `Ensure()` false → `STG_E_READFAULT`。**这是最贴合"Win10 过、Win11 崩"的解释。**
2. 临时目录**单槽复用**（全局"当前 fetch"）：并发的第二个文件请求把第一个挤掉。
3. `Ensure()` 里 CLI 进程启动失败/超时（Win11 上传下都）被静默吞成 false。

**下一步（按序）**
1. 先补日志（这是眼下最缺的）：`Ensure()` 里记 fetch 开始/结束、CLI 退出码、临时树路径与
   `PathFileExists(目标)`；`Read()` 记首次调用、`Ensure()` 返回值、请求长度/实读长度。
2. `Ensure()` 的错误分类：文件不存在 / 下载失败 / 打开句柄失败 → 分别返回
   `STG_E_FILENOTFOUND` / `STG_E_READFAULT` / `STG_E_ACCESSDENIED`，别再一律盖成 READFAULT。
3. 审计 fetch 引用计数与临时树删除时机：**每个 fetch 独占临时目录**，删除前确认没有 stream 持有；
   禁止"全局单槽 fetch"。
4. 修完在 Win11 回归：单大文件（>100MB）、一批小文件、拖拽与右键复制两个入口、上传方向。

## 2026-09-17 诊断构建已出（`dist\Erf-0.1-Alpha-Setup.exe`，12:1x 重编）

改动只在 `RemoteDataObject.h`（行为不变，纯加日志）：
- `CRemoteStream::Read`：`Ensure()` 失败时记 `[DL] Read FAILED at Ensure ...`（这正是
  STG_E_READFAULT 的真正出口）、`ReadFile` 失败记真实 `GetLastError`、首次读成功记请求/实读长度。
- `CRemoteStream::Ensure`：下载前后各一条 —— `[DL] Ensure start ...`（站点/远程/期望大小/临时路径）、
  `[DL] Ensure FAILED: download failed ... exists=%d`、`[DL] Ensure FAILED: cannot open local ... err=%lu`、
  `[DL] Ensure ok left size_on_disk=%lld expected=%llu`。
- 判定方法：日志里出现 `[DL]` 即说明跑的是诊断构建。

部署与收日志脚本：`src-setup/deploy-diag.ps1`（不带参数＝静默升级到默认目录；`-Collect`＝把最近 400 条
`[DL]/[XFER]/[DATAOBJ]/[NAV]` 导到 `dist\diag\`）。

## 2026-09-17 结论：不是 Win11 的下载代码 bug，是"安装状态不一致（旧 DLL 没被替换）"

诊断构建在 Win11 上复现成功（全链路日志）：
```
[DATAOBJ] GetData contents idx=0 'test.txt'
[DL] Ensure start site='WSL' remote='/home/zhou/AI_work/test.txt' size=41 -> %TEMP%\rfs-dataobj\35264_1_test.txt
[DATAOBJ] fetch '...' -> '...' exit=0
[DL] Ensure ok local='...' size_on_disk=41 expected=41
[DL] first Read ok cb=41 got=41
```
用户观察到的关键线索：**上次安装"没看到 explorer 被终止"** → 旧 DLL 根本没被替换（被 explorer 映射着，
Inno 的替换静默失败/被跳过），于是**旧 DLL 配新 CLI/客户端**：`cli get` 失败 → `Ensure()` 返回 false →
被一律报成 `STG_E_READFAULT`（"执行读取操作时发生磁盘错误"）。最新一次安装确实终止并重启了 explorer
（黑屏）→ DLL 真换了 → 复制/拖拽、进队列全部正常。

**已落地的改动**
1. `Ensure()` 下载失败先重试一次（300ms）再放弃，并把 `_done` 放回 false，让复制引擎的重试能真的重试；
   新增 `[DL] retrying download` 日志。→ 提交 `b012d2d`
2. `src-setup/deploy-diag.ps1` 装完**比对安装目录 DLL 与随包 DLL 的 SHA256**，不一致直接报错并提示
   "关掉/重启资源管理器后再装"。→ 这次提交
3. 诊断埋点（`[DL]` 系列）保留，作为以后这类问题的第一现场证据。

**还没做（下一批）**
- DLL ↔ CLI 的**协议版本握手**：现在两者版本不一致时只会得到一个裸的退出码，应该显式报"版本不匹配"。
- 安装器侧：替换 DLL 前先确认 explorer 真的没了（现在依赖 taskkill + 800ms sleep）；若替换后哈希不一致，
  安装结束时应主动报错而不是静默成功。
- `[SAMPLE] CreateViewObject level=4` 与 `[DIAG] GetUIObjectOf` 刷屏要限流，否则真信号被埋。

## 2026-09-19：Win11 大文件复制取消后被误作失败并自动重试（已修复，待 Win11 回归）

### Win11 日志证据

`dist\logs\remotefs-debug.log` 记录了同一条 `CFSTR_FILECONTENTS` 请求：

- `[24821546] [DATAOBJ] GetData contents idx=0 'MHCpan_1B_A1.length_08.out.zst'`；
- 同一时间 `CRemoteStream::Ensure` 请求大小为 `4821300010` 字节，临时目标为
  `%TEMP%\rfs-dataobj\38536_1_MHCpan_1B_A1.length_08.out.zst`；
- 约 14.7 秒后，桥接下载返回失败，随即出现 `[DL] retrying download (attempt 2)`。

同时间段 `dist\logs\rfs-tasks.log` 只有第一条 managed 下载的 `status=fail` 和随后的同文件第二条
`BEGIN managed`，没有 `CANCEL` / `status=cancel`。因此第二次下载不是新的用户操作，而是取消未被传到
服务、且 Shell 把泛化失败纳入重试的结果。

### 根因

1. `CRemoteStream::_done` 的含义是“已开始 Ensure”，不是“文件已经下载完成”。`Release()` 却用
   `!_done && _pos > 0` 判断是否发送 `CANCEL`。`CFSTR_FILECONTENTS` 会先整文件预取到临时目录，
   此时通常还没读出第一个字节（`_pos == 0`），而 `_done` 早已置位；Explorer 取消释放流时不会发送
   `CANCEL`。
2. 服务端原先只通过 `TransferTaskService.CancelBatch` 扫描已经显示在 WPF 队列里的任务。`FETCH` 已创建、
   但 UI 行尚未投递时，取消会漏掉真实后台 job。
3. Shell 与服务分别用 `"FAIL: cancelled"` / `Contains("cancelled")` 这类文本识别取消；任何丢失、改写或
   普通失败都会落入重试路径，无法保证取消是终态。

### 修复

- `FtpBridgeFetchState` 明确区分 `Done` / `Failed` / `Cancelled`；`FETCHSTATUS` 的 `CANCELLED` 直接映射为
  枚举，`CRemoteStream` 返回 `HRESULT_FROM_WIN32(ERROR_CANCELLED)`，不会再重试。
- `Release()` 以“FETCH 是否已开始且本地文件是否已就绪”判断取消，而非 `_done` 或已读字节数；发送
  `CANCEL <batchId>` 后记录请求与确认。
- `RemoteBridgeService` 直接取消匹配 batch 的 `FetchJob.Cts`，再同步队列 UI；即使 UI 行尚未创建，后续
  开始的下载也会在连接前被取消。`FETCHSTATUS` 以 typed job state 返回最终 `CANCELLED`，不再解析异常文本。
- SFTP/FTP 下载入口在连接前检查 token；服务在单文件取消或失败时再次删除临时目标，保留现有“不留半截
  缓存”的策略。
- 新诊断字段：job ID、batch ID、取消请求/确认及最终状态（日志时间戳）、请求大小、已传输/总字节、临时
  路径、可用磁盘空间、脱敏后的异常链与清理结果。`password` / `pwd` / `passphrase` 键值会被替换为 `***`。

### 验证状态与 Win11 回归

已完成：静态调用链审计，确认目标路径没有保留字符串式取消判定，且 `CANCELLED` 不会进入重试分支。
本开发环境缺少 .NET SDK 和 MSVC x64 Desktop C++ 工具，`dotnet build` 与 `compile.ps1` 无法执行；未进行真实
远程大文件下载。

在具备工具链的 Win11 安装机，先构建并部署，再仅用可控小文件验证：

```powershell
Set-Location Z:\tools\explore-remote-files
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\dist\ExplorerRemoteFs-win-x64\install.ps1
```

复制一个可控的小文件到本地后立即在 Explorer 复制窗口或「传输队列」点击取消。预期：

1. `remotefs-debug.log` 出现 `[DL] cancel requested`、`[XFER] cancel` 与 `terminal=cancelled`；
2. `rfs-tasks.log` 以同一 job ID 记录 `cancel requested`、`fetch cancelled` 和 `state=Cancelled`；
3. 无 `[DL] retrying download`，无第二个同路径 `BEGIN managed`，临时文件已删除；
4. Explorer 立即恢复可操作，无需杀进程。