# 变更交接：Win10 侧自 `ab50578` 以来的改动（2026-09-20）

> **交接对象**：Win11 侧智能体。
> **本文范围**：`ab50578`（你上次「回滚到 a0299c6 基线，只保留属性页修复」）→ `2cb75ed`，
> 共 **31 个提交**（`3fef322` … `2cb75ed`，09-19 23:44 → 09-20 21:28）。
> **本文只说明"我这边动了什么"**：哪些文件/机制变了、哪些与你的工作重叠、你需要重编/重拉什么。
> **多标签页导航问题不在本文范围** —— 那份问题描述由你写、也由你解决；我不改你的结论、不给你派任务。
>
> 📌 **本文是历史快照（范围截止 `2cb75ed`）。** Win11 侧接手后的进展（UIA 多标签精确直达、
> 全面重编译待办、远程 main 重建）见 `docs\README.md` 的「最新状态」一节。

---

## 0. 先看这个：与你工作重叠的三件事

| 你的区域 | 我动了吗 | 说明 |
|---|---|---|
| `src-cpp/…/ErfProtocolCommand.cpp`（`erf:` in-process 处理器） | **一行没动** | 该文件在本范围 0 提交 |
| `ParseDisplayName` / `CaptureTabAnchor` / `TryNavigateForegroundExplorer` | **一行没动** | 按符号查 `git log -S`，本范围 0 提交 |
| `src-setup/erf.iss` 的 **`erf:` 协议注册与版本分流** | **一行没动** | 我只**新增**了 `.erfdl` 关联（11 行）；分支逻辑、`DelegateExecute` 的删除、`InprocServer32` 全部原样 |
| `src-cpp/…/PropSheetProbe.h`（属性表探针） | ⚠️ **动了 7 行** | `1b923b3` 给采样线程加了模块引用计数（`DllAddRef/DllRelease`），因为该线程会跨越 COM 对象存活期，卸载后会回调崩溃。**纯追加，逻辑未改** |
| `src-client/…/App.xaml.cs` | ⚠️ **动了 1 个提交（+59 行）** | 只加了 `--open-ticket`（票据）分支；`--open-erf` 那条链路的代码未改 |

另外有**两个新耦合**会影响你编译与调试，详见 §3。

---

## 1. 变更清单（按主题）

### A. 崩溃与稳定性（09-20 凌晨，本范围最早的 4 个提交）

| 提交 | 内容 |
|---|---|
| `3fef322` | `scripts/clean-state.ps1`：ERF 状态诊断 / 干净卸载辅助（后来我修了它两个自带 bug：缺 BOM、硬编码安装目录） |
| `3569ec2` | **Explorer 爆栈崩溃**：`CFolderViewImplEnumIDList::Initialize` 在栈上放了 466KB 帧，在 `CMenu::InvokeCommand → DeleteSelectionWithNativeFileOperation` 的深栈上直接 `0xC00000FD`；改为堆分配 + 复用小栈线程。同时修**目录不刷新**与**下载阻塞** |
| `1edb5ab` | 大小写敏感缓存（`B` 与 `b` 不再互相覆盖）；修服务 GUI 里复制的冻结 |
| `3ef54d4` + `1b923b3` | **工作线程 pin 模块**：DLL 里自建线程（菜单、属性表采样、数据对象预热）都加 `DllAddRef/DllRelease`，避免卸载后回调崩溃（`PropSheetProbe.h` 那 7 行在此） |

### B. 传输队列 / 取消 / 暂停

| 提交 | 内容 |
|---|---|
| `1b923b3` | 传输取消/暂停可用（`SafeProgress<T>`：SSH.NET 的同步 `DownloadFile` 在**线程池**上回调，在里面抛 `OperationCanceledException` 会直接杀服务 → 改为安全包装） |
| `8a34292` | 上传可暂停（暂停闸门挂在**输入流**上，不阻塞会话消息线程）+ 传输阻塞取证探针 |
| `e2011e0` | 菜单/属性页里的**写操作**（`RunCli`）全部移出 Explorer UI 线程 |
| `1a67145` | 传输统一走常驻服务队列；枚举路径去掉同步 LIST（那段是死代码） |

### C. 复制数据通路（本范围最大的一块，18 个提交）

**结论（都已实测）**：

- 真实 Explorer 的复制只能走 `CFSTR_FILECONTENTS`；`ITransferSource` 只用于删除/移动/元数据；
- `IDataObjectAsyncCapability` 是死路（Shell 从不查询，复合 `SHCreateDataObject` 不转发该接口）；
- 正解是**直传流**：Service → 命名管道 → DLL 的 `IStream`（不再落 `%TEMP%` 中转）；
  FTP 后端因为 `DownloadStream` 需要可 seek，退回"服务侧临时文件再喂流"（SFTP 是真直传）；
- 暂停语义：**复制对话框驱动的任务禁用暂停**（只能等或取消），**真暂停留给自家路径**（菜单下载 / 复制到 / 上传）。

| 提交 | 内容 |
|---|---|
| `64ade59` `08ffcf9` `2ab8f0f` | 探针量化源端展开、复制前后台预热、文件夹复制补回顶层目录名 |
| `fc2d323` | A 方案：`IDataObjectAsyncCapability`（**证伪**） |
| `03d19fc` `ffa0430` `341d14c` `874b3d7` `97d7a87` | B 方案探索：`ITransferSource` → `IShellItemResources`（未公开协议）→ 做成注册表档位 `ShellResourceMode` |
| `a59c56b` | 回到虚拟文件格式（唯一数据通路）+ `CRemoteStream` 改"边下边读" |
| `4286013` `8caad86` `7858ad6` | 共享打开冲突、等待期间抽消息、**直传流**（管道） |
| `3075a0b` `7be2417` `5290c18` | FTP 回退、引用计数判断"Shell 已释放流"、暂停改限速（后被取消暂停取代） |
| `f56084e` `147fa97` | 自家「复制到本地文件夹」先下 `<目标>.rfs-part` 再原子改名；**妥协方案**：复制类禁用暂停、菜单「下载」改为"即点即下"（落 `DownloadDir`，同名自动 `" (2)"`）；管道截断保护 |

### D. 传输票据 `.erfdl`（Ctrl+C 产出"任务单"）

| 提交 | 内容 |
|---|---|
| `0a6978d` | 服务侧核心：jobId 登记（持久化）+ 桥接操作 `MKTICKET` / `OPEN-TICKET` + `--open-ticket` 入口 |
| `e6dda45` | DLL 侧：开关打开时 Ctrl+C 产出的虚拟文件 = **一张票据**（72 字节，`SHCreateMemStream` 直接给内存流）；安装器注册 `.erfdl` |
| `f7a1d4f` | 设置页新增「票据模式」勾选框 + 「重新关联 .erfdl」按钮 |
| `2cb75ed` | 决策框返工：按钮文字 = 选项本身（迁移到新目录 / 在新目录重新下载 / 覆盖它 / 保留两者 / 取消） |

**票据的隐私设计**：票据里**只有** `magic/version/jobId`，**不写任何路径**；
文件清单、来源、原目标只存在服务侧数据库；jobId 查不到即拒绝。

### E. 缓存数据库化（顺带）

| 提交 | 内容 |
|---|---|
| `e6dda45` | 目录缓存 + 票据记录改用 **SQLite**：`<MetadataCachePath>\erf-cache.db`（WAL），**扩展 DLL 与常驻服务共用一个库**；`dir_cache(site,path,tick,items BLOB)` 取代"一个目录一个 `.bin`"；`tickets`/`ticket_items` 取代 `tickets.json` |
| `b5113fb` | 首次打开库时一次性清掉旧版遗留的 1069 个 `.bin` |

### F. 服务 UI

| 提交 | 内容 |
|---|---|
| `f7a1d4f` | 设置页改**左导航 + 右内容**（通用 / 缓存与编辑器 / 下载 / 文件关联）；新增「默认下载目录」「重新关联 .erfdl」 |
| `147fa97` | 队列暂停按钮按 `CanPause` 灰显 |

---

## 2. 文件级地图（本范围改了什么，以及是不是你的区域）

```
                                            增删行   你的区域?
src-cpp/ExplorerDataProviderFtp/
    ErfProtocolCommand.cpp                     0     ✅ 是（未动）
    PropSheetProbe.h                          +7     ⚠️ 是（仅模块 pin）
    ExplorerDataProvider.cpp                +251     ⚠️ 部分是（GetUIObjectOf/枚举；ParseDisplayName 未动）
    RemoteDataObject.h                      +675     ❌ 否（数据对象整体重写：虚拟文件/直传流/票据）
    FtpMeta.h                               +627     ❌ 否（桥接协议 + SQLite 缓存 + 票据）
    ContextMenu.cpp                         +630     ❌ 否（菜单动作移出 UI 线程 + 后台 worker）
    compile.ps1                              +18     ⚠️ 编译方式（见 §3）
    third_party/sqlite/sqlite3.{c,h}      +276k     ❌ 否（新增内置 SQLite）
src-client/RemoteFsClient/
    App.xaml.cs                              +59     ⚠️ 是（仅 --open-ticket）
    Services/RemoteBridgeService.cs         +312     ❌ 否
    Services/TransferTaskService.cs          +99     ❌ 否
    Services/TransferTicketService.cs       +432     ❌ 否（新文件）
    Services/AppSettings.cs                  +16     ❌ 否
    AppSettingsWindow.xaml(.cs)             +264     ❌ 否
    TicketChoiceWindow.xaml(.cs)             +60     ❌ 否（新文件）
    RemoteFsClient.csproj                     +2     ⚠️ 新依赖 Microsoft.Data.Sqlite
src/ExplorerRemoteFs/Providers/            +210     ❌ 否（下载/上传能力 + SafeProgress + PausableReadStream）
src-setup/erf.iss                           +11     ⚠️ 仅新增 .erfdl 关联
scripts/clean-state.ps1                    +132     ❌ 否
```

---

## 3. 三个需要你注意的"新耦合"

1. **编译**：`src-cpp/ExplorerDataProviderFtp/compile.ps1` 现在除 7 个 `.cpp` 外，
   还要**按 C** 编译 `third_party\sqlite\sqlite3.c`（`/MD` 必须与其它文件一致），并把 `sqlite3.obj` 一起链接。
   少了它链接会失败；sqlite3.c 首次编译要多花 1~2 分钟。
2. **DLL 体积 / 依赖**：`ExplorerDataProviderFtp.dll` 从 **525 KB → 1.52 MB**（内置 SQLite）。
   客户端侧新依赖 `Microsoft.Data.Sqlite` + 原生 `e_sqlite3.dll`（自包含发布会自动带上，
   `src-setup/erf.iss` 用 `recursesubdirs` 打包整目录，无需改安装脚本）。
3. **`GetUIObjectOf(IID_IDataObject)`** 里多了一个**票据模式分支**（开关 `UseTransferTicket`，**默认关**）。
   它只影响"选中项 → 数据对象"的内容，不影响导航；但会让 `remotefs-debug.log` 多出
   `[TICKET]` / `[DATAOBJ] ticket …` 行，你排查时注意别被干扰。

---

## 4. 本轮新增的注册表开关 / 关联（你调试时可能撞到）

| 位置 | 缺省 | 用途 |
|---|---|---|
| `HKCU\Software\ExplorerRemoteFs\UseTransferTicket` (DWORD) | `0` | Ctrl+C 是否产出 `.erfdl` 票据（**每次现读**，改完即刻生效） |
| `HKCU\Software\ExplorerRemoteFs\DownloadDir` (SZ) | `%USERPROFILE%\Downloads` | 菜单「下载」的落地目录（即点即下） |
| `HKCU\Software\Classes\.erfdl` → `ERF.TransferTicket` | — | 票据关联（安装器注册；设置页有"重新关联"按钮） |
| `HKCU\Software\ExplorerRemoteFs\MetadataCachePath` | `%LOCALAPPDATA%\ExplorerRemoteFs\MetadataCache` | **现在这个目录里放的是 `erf-cache.db`**（不再是每目录一个 `.bin`） |
| （更早就有）`UseVirtualFileFormats` / `ShellResourceMode` | `1` / `0` | 复制数据通路的实验档位，保留可回退 |

---

## 5. 怎么拿到这些改动 / 怎么退回

```powershell
# 拿最新（共享盘上同一个仓库）
git log --oneline ab50578..HEAD          # 就是本文 §1 那 31 条
git checkout feature/microsoft-explorer-core

# 构建 + 安装（C++ 必须在有 VS C++ 工具链的机器上；本机是 Win10 虚拟机）
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-cpp\ExplorerDataProviderFtp\compile.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-setup\build-inno.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\src-setup\install-default.ps1
Stop-Process -Name explorer -Force; Start-Sleep 4; Start-Process "$env:WINDIR\explorer.exe"

# 退回你上次的基线（本范围全部改动一次性放掉）
git checkout ab50578      # 或 git revert 指定提交
```

---

## 6. 我这轮**没做**的事（边界，免得你去别处找）

- 没有改 `erf:` 的注册形态（Win11 仍是**命令行通道 + 显式删除 `DelegateExecute`**）；
- 没有改 `ParseDisplayName`、也没有改标签页定位/选窗口那一套；
- 没有改属性表本身的逻辑（只加了采样线程的模块 pin）；
- 没有动 Win11 侧的任何系统设置/注册表（这些都在你那边）。
