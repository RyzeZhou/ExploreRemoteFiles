# ExploreRemoteFiles (ERF)

> 中文名 **易远传**（易 = Explorer 的 E，远 = Remote，传 = 传输；对应图标里的 E/R/F 三个字母）。

> **把 WinSCP 的远程浏览、远程语义、远程打开/编辑与高效传输能力，无缝嵌进 Windows 资源管理器。**
> 更技术化的定义：**A native Windows Shell namespace for SFTP / FTP, backed by a cross-filesystem transfer engine.**

一句话：**不替代 Explorer，而是扩展 Explorer。**

版本：**0.1-Alpha** ｜ 当前主线分支：`feature/microsoft-explorer-core`

## 为什么叫 ExploreRemoteFiles（不叫 ExplorerRemoteFiles）

1. 避免 `Explor**er**Remote` 两个连续 `R` 的拼写与读音黏连；
2. **`Explore` 既指 Explorer，也是动词"探索"**——远程只是第一条腿，
   后续要往 Explorer 的**本地增强能力**扩展（见路线）；
3. 缩写 **ERF** 由此成为产品前缀：地址 `erf://<site>:/path`、协议 `ERF-Proto`、
   服务端 `ERF-Server`、插件 `ERF-Shell`。详见 docs/PROJECT_IDENTITY.md。

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
       凭据管理 · 命名管道桥接 · 「远程操作队列」（删除 / 递归改权限 / 传输共用）
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

**Alpha 之后的主要缺口**：上传/下载尚未并入统一操作队列的进度与暂停体系；
深相对路径经剪贴板协议无法表达（已改为明确拒绝而非静默出错）；
SFTP 上「多而小文件」的传输性能与完整性是下一步（ERF 协议）。

## 构建与安装

```powershell
# 需要 VS2022（C++ 工具集 + Windows SDK）、.NET 8 SDK、Inno Setup 6/7
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1   # 编译扩展 DLL / CLI / 客户端
powershell -ExecutionPolicy Bypass -File src-setup\build-inno.ps1    # 出安装包 Erf-0.1-Alpha-Setup.exe
# 自检（装到临时目录 → 51 项断言 → 卸载 → 再断言一遍）：
powershell -ExecutionPolicy Bypass -File src-setup\inno-test.ps1
```

站点凭据通过常驻客户端 GUI 添加，或写 `%APPDATA%\ExplorerRemoteFs\connections.json`。
调试日志：`%LOCALAPPDATA%\ExplorerRemoteFs\logs\remotefs-debug.log`（4 MB 轮转）。

## 路线

> **出口条件与版本线以内部里程碑文档为准**（不随公开仓库发布）：
> 真正的 **Alpha** ＝ 递归设置权限 ＋「远程操作队列」 ＋ 在目录右键「打开终端」三件事全部完成；
> 之后**第一件事就是 Windows 安装程序**（可选安装目录、常驻程序随登录自启动、可干净升级与卸载）。

| 期 | 内容 | 文档 |
|---|---|---|
| Alpha 门槛 | 递归权限 + 队列 + 打开终端（含安装包的技术岔路：不用 MSIX、自启动不做成 Session 0 服务） | 内部文档 |
| 近期 | 自研进度窗口升格为**「远程操作队列」**（删除 / 递归改权限 / 传输共用一个队列与一组契约），顺带修递归 chmod | 内部文档 §1 §3 |
| 近期 | **ERF 协议第一步**：在 SFTP 上解决"多而小文件"传得慢与传不全（并行会话 / `ssh exec` 打包流 + manifest + 校验） | 内部文档 |
| 已评估 | 站点/目录右键「在 Windows 终端中打开」：认证**交给终端里的 ssh**，产品不碰凭据 | 内部文档 |
| 之后 | **扩展 Explorer 的本地能力**（`Explore` 作动词的第二条腿）：批量重命名、校验、差异比对等复用同一套队列 UI 与契约 | 待定 |
| 长期 | ERF-Server 代理模式起步 → 独立服务端（ext4/NTFS/对象存储），语义声明与独占能力落地 | 内部文档 §7 |

## 成功判据

```text
1. Win+E → 左侧进入站点 → 直接看到 /var/www
2. Details 显示 rwxr-xr-x / owner / group，且能改（含递归）
3. Ctrl+C/V、F2、Delete、拖放、右键菜单全部可用，且 Explorer 不冻结
4. 大目录与小文件海：传得完，也传得快
5. 双击远端配置文件 → 本地默认编辑器打开，保存自动回传
6. 任何"部分完成"都必须被看见，禁止报成功
```

## 下载与安装（0.1-Alpha）

到 [Releases](https://github.com/RyzeZhou/ExploreRemoteFiles/releases) 下载
`Erf-0.1-Alpha-Setup.exe`（单文件、自包含，**不需要预装 .NET**），双击按向导安装即可：
可选安装目录、创建桌面快捷方式、登录自启，只装当前用户、不弹 UAC。

- 安装/升级只在替换扩展 DLL 的那几秒终止资源管理器（桌面短暂黑屏 1–3 秒后自动恢复），
  向导会先弹一个讲清后果的确认框；卸载同理。全新安装不会终止资源管理器。
- 安装包**未做代码签名**：SmartScreen 会提示"未知发布者"，需要点"更多信息 → 仍要运行"；
  个别杀软也可能误报 —— 这是未签名软件的必然现象，不代表程序行为异常。
- 卸载：设置 → 应用 → 已安装的应用 → 易远传（**站点配置与凭据会保留**）。

## 关于开发文档

本仓库公开的是**源码、构建脚本与安装器脚本**。里程碑、调研记录、实验与排障笔记（`docs/`），
以及实验用探针与本地测试服务器（`research/`）**不随公开仓库发布**，它们只存在于作者的开发树里。

## 许可

[MIT](LICENSE)。

---

*项目起始 2026-08-20（原名 Explorer RemoteFS，2026-09-15 定名 ExploreRemoteFiles）·
原始探索概要归档于 `reference/`。*
