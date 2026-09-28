# 发布说明 · ExploreRemoteFiles 0.1-Alpha

日期：2026-09-15 ｜ tag：`v0.1-Alpha` ｜ 分支：`feature/microsoft-explorer-core`
定位：**Alpha = 作者自用可依赖；不承诺他人机器可直接安装**（未做代码签名，卸载残留未系统验证）。

> ⚠ **本 tag 是预发布检查点，不是门槛意义上的 Alpha。**
> 真正的 Alpha 需完成递归设置权限、「远程操作队列」、在目录右键打开终端三件事，
> 之后才开始做 Windows 安装程序 —— 出口条件与版本线以
> [MILESTONES.md](MILESTONES.md) 为准，本文件下方"已知问题 #1"即为未达门槛之处。

> ✅ **状态更新（2026-09-20）：上面这三件事全部已完成，Windows 安装程序也已落地并多次实机安装。**
> 逐项进度见 [MILESTONES.md](MILESTONES.md) 的 A1–A4 进度块；
> 本文下方的能力清单是 **2026-09-15 那一刻的快照**，此后新增的能力
> （传输票据 `.erfdl`、目录/票据缓存的 SQLite 化、设置窗口四页、菜单「下载」即点即下、
> `erf:` 多标签精确直达、传输统一走常驻服务队列）以 **根目录 [README.md](../README.md)** 为准。
> 本文件下方"已知问题 #1"（递归权限未达门槛）**已不成立**。

> 🔧 **状态更新（2026-09-28）：0.1-Alpha 安装包已用含下列修复的代码重打**
> （`dist\Erf-0.1-Alpha-Setup.exe`，132.6 MB）。
> 9/27 那份包里的扩展 DLL 有一处 **PIDL 项结构错位**（提交 `24e9a90`）：右键菜单的
> 属性 / 新建文件 / 新建文件夹 / 刷新一点就报错 —— 原因是 `e5cad68` 给 PIDL 项结构插入
> `fAbsRoot` 时，`ContextMenu.cpp` 里另一份同布局的只读视图没有同步，`szName` 偏移
> 12→16，菜单读到的"文件名"其实是 `fAbsRoot` 那 4 个字节。
> 修复把该布局收敛为**唯一共享定义** `PidlItem.h`，并加了两条 `static_assert`
> 编译期哨兵 —— 以后再有人改布局会在编译期失败，而不是运行时读错名字。
> 详见记忆库 `ERF-2026-09-28-02`。

## 这一版是什么

在资源管理器左侧直接出现站点根，像本地文件夹一样浏览、编辑、传输远程 Linux 文件系统，
并且**保留 POSIX 语义**（权限位、属主、属组、符号链接）。
不映射成虚拟盘、不伪装 NTFS、不替代 Explorer。

实现为三层：C++ Shell 命名空间扩展（Explorer 进程内，**不做任何同步网络 I/O**）
→ 常驻 WPF 客户端（凭据、任务与进度窗口、命名管道桥接）
→ CLI 子进程（一次性网络动作）。
Provider：SFTP（SSH.NET）、FTP（FluentFTP）。

## 能力清单（均经实机验证）

| 域 | 能力 |
|---|---|
| 导航 | 面包屑 / 地址栏 / 后退 / 上级 / F5 / 站点根与深层目录进出 |
| 元信息 | Details 显示 `rwxr-xr-x` 式权限、Owner、Group、Modified、Type；dotfile 显隐 |
| 写操作 | 新建目录、新建空文件（`touch`，已存在即失败不覆盖）、重命名、删除（原生 IFileOperation 承接，真实进度在自研窗口）、权限修改、属主/属组修改 |
| 传输 | 远程→本地、本地→远程（复制/粘贴与拖放）、远程→远程服务端复制/移动 |
| 集成 | 右键菜单 + 命令栏按钮（canonical verb 桥接）、`erf://` 地址方案注册（带所有权检查，不覆盖他人 handler）、默认编辑器打开远端文件 |
| 性能 | 10 万文件目录下右键/删除/属性不再冻结 UI；复制 33 430 文件数量正确 |

## 本版关键修复（2026-09-14/15）

1. **UI 线程冻结根因**：`CRemoteDataObject::GetData` 在判定格式**之前**就递归展开整棵树，
   于是连 `CF_HDROP` 这种我们不支持的格式探测都会付一次全树同步拉取
   （冷 10 万文件目录 = 9 188 907 字节 / 6875 ms）。现改为**先验格式**，
   且冷目录在 shell 探测期间**拒绝**给出虚拟文件格式而不是交出半成品树。
   过程与 5 个被否证假设：[UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md](UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md)
2. **复制报 0x80004001**：把剪贴板复合对象里的 inner 数据对象摘掉后，
   `FileGroupDescriptorW/FileContents` 消失，复制引擎退到 `ITransferSource::OpenItem`（我们实现为 E_NOTIMPL）。
   已恢复挂载并加了哨兵日志。
3. **复制静默截断（数据丢失）**：摊平上限 5000 → 200 000，**触顶改为明确拒绝而非交出部分树**；
   并补了 `cFileName[MAX_PATH]` 表达不了深相对路径时把**空名字**交给 Explorer 的第二个静默损坏。
4. **安装会抹掉用户桌面图标设置**：`New-Item -Force` 作用在
   `HideDesktopIcons\NewStartPanel`（Windows 拥有的键）上等于重建键，
   连带销毁同键的 `{20D04FE0-…}=0`（"显示计算机"开关）。A/B 实测证实，已修。
5. **死注册清理**：删除指向旧项目 `ExplorerDataProvider.dll` 的两把遗留 CLSID
   （其一 `(Default)` 还是 "FolderView SDK Sample Control"），删前导出备份，删后复验只剩活注册。

## 已知问题（本版未修，含根因）

| # | 问题 | 性质 |
|---|---|---|
| 1 | **递归设置权限只改目录、不改文件**（`SetPermissionsRecursive` 缺叶子分支；FTP 版同形待核对） | 功能错误，修法已定，并入「远程操作队列」 |
| 2 | `getr` **逐文件串行**下载，且一处异常 `break` 整树中断 | 性能 + 正确性，ERF 一期靶心 |
| 3 | 深相对路径（>259 字符）经剪贴板协议无法表达 → 现为明确拒绝 | 协议天花板，需自有传输 |
| 4 | `ParseDisplayName` 冷路径仍同步拉列表（为 shell 身份回环保留） | 最后一条 UI 线程网络点 |
| 5 | 冷的大目录首次浏览会有短暂空白后填充（换取删除/右键瞬时） | 有意取舍 |
| 6 | 12 个历史 `.obj` 曾被跟踪（本版已从索引移除，不影响历史体积） | 仓库卫生 |

详见 [KNOWN_ISSUES_2026-09-14.md](KNOWN_ISSUES_2026-09-14.md)。

## 如何验收

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1
powershell -ExecutionPolicy Bypass -File dist\ExplorerRemoteFs-win-x64\install.ps1   # 会重启 Explorer
```

1. 左侧出现站点 → 进入 → Details 有权限/属主/属组；
2. 右键 10 万文件目录：**菜单立即出现且可点**；工具栏删除：**不弹原生进度条**，自研窗口显示真实进度且可取消；
3. 复制一个 >5000 文件的目录到本地，数落盘数量应与源一致；
4. 递归改权限**预期失败**（已知问题 1）——这是本版承认的缺口，不要当成回归；
5. 安装后检查桌面"计算机"图标仍在（已知问题 4 的反证）。

## 下一版（0.2）的判断线

- 递归 chmod 修好并接入**「远程操作队列」**（队列六条契约见 KNOWN_ISSUES §3）；
- SFTP 上"多而小文件"实测提速达标（基准三数见 ERF_PROTOCOL_PLAN §8.6）。

相关：[../README.md](../README.md) ｜ [PROJECT_IDENTITY.md](PROJECT_IDENTITY.md) ｜ [ERF_PROTOCOL_PLAN.md](ERF_PROTOCOL_PLAN.md)
