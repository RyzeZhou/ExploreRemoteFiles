# 易远传 · ExploreRemoteFiles

> 把远程 Linux 主机（**SFTP / FTP**）挂进 Windows 资源管理器的导航窗格，
> 像本地文件夹一样浏览、改权限、复制粘贴、拖放传输 —— 并保留 POSIX 语义。

中文名 **易远传**：易 = Explorer 的 E，远 = Remote，传 = 传输；图标里的 **E / R / F** 三个字母代表
ExploreRemoteFiles，其中 **R** 上色表示远程站点连接状态、**F** 上色表示文件传输状态。

> ## ⚠️ 这是早期版本（Alpha），请谨慎使用
>
> - **没有经过足够测试**：目前只在作者自己的机器上做过实测（Windows 10 22H2 / Windows 11），
>   **没有大规模用户验证**，也**没有代码签名**。
> - **你可能遇到功能异常，甚至数据损失**：请不要把它当作唯一的文件管理手段；
>   重要数据请另留一份可访问的副本 —— 远程目录里的删除、覆盖、改权限都是**立刻真实生效**的。
> - 安装包**未签名**：SmartScreen 会提示“未知发布者”，需要点“更多信息 → 仍要运行”；
>   个别杀毒软件也可能误报。这是未签名软件的必然现象。
> - 安装/升级/卸载会在**替换或删除扩展 DLL 的那几秒**终止资源管理器（桌面短暂黑屏 1–3 秒后自动恢复），
>   向导会先弹一个讲清后果的确认框；**全新安装不会**终止资源管理器。
> - 发现问题属于预期：欢迎到 [Issues](https://github.com/RyzeZhou/ExploreRemoteFiles/issues) 报告
>   （附日志更好，见文末“出问题了怎么办”）。

<!-- 截图位 1：导航窗格里的「易远传」+ 多站点目录树 + 右侧 POSIX 列
     放好后把这一行换成：![导航窗格与 POSIX 列](assets/screenshots/01-namespace.png) -->

## 它能做什么

**浏览与导航**

- 导航窗格里的「易远传」入口，多个远程站点并列，和本地文件夹一样点进去
- 面包屑 / 地址栏 / 后退 / 上级 / F5 刷新都按资源管理器的原生行为来
- 10 万文件的大目录也不会把资源管理器卡死（网络动作一律在后台，UI 线程不等网络）

**POSIX 语义**（Linux 用户最在意的部分，本地资源管理器没有）

- 列与属性页显示：大小、修改时间、**所有者**、**属组**、**权限**
- 权限 `rwxr-xr-x` 与数字 `755` 两种写法都能看、都能改
- **递归改权限**（`chmod -R`，文件与目录都改）与**递归改属主/属组**（`chown -R`，支持用户名或数字 ID）

**读写与传输**

- 新建文件夹、重命名、删除（含递归删除，带真实进度与取消）
- 复制 / 剪切 / 粘贴：远程 ↔ 本地、远程 → 远程
- 从资源管理器拖放上传、拖出下载

**远程操作队列**

- 删除 / 递归改权限 / 传输共用一个队列窗口：进度、当前项、错误汇总，可暂停

**打开终端**

- 远程目录右键「在 Windows 终端中打开」：Windows Terminal 配置文件与 VS Code 两条路线
- **认证交给终端里的 `ssh`**，产品本身不碰你的凭据

**其它**

- `erf://` 地址协议；托盘图标显示连接与传输状态
- 站点凭据存在 **Windows 凭据管理器**（`ExplorerRemoteFs/*`），不落明文文件
- 文件大小显示口径与资源管理器一致（交给 Windows 自己格式化）
- **不需要预装 .NET**：客户端与命令行各自带运行时

<!-- 截图位 2：属性页（权限 / 所有者 / 属组）
     放好后换成：![权限属性页](assets/screenshots/02-permissions.png) -->

<!-- 截图位 3：远程操作队列窗口
     放好后换成：![远程操作队列](assets/screenshots/03-queue.png) -->

## 下载与安装

到 [**Releases**](https://github.com/RyzeZhou/ExploreRemoteFiles/releases) 下载
**`Erf-0.1-Alpha-Setup.exe`**（单文件、自包含，不需要预装 .NET）。

- 系统要求：**Windows 10 19045+ / Windows 11，64 位**
- **只装当前用户**（默认 `%LOCALAPPDATA%\ExplorerRemoteFs`），不弹 UAC、不需要管理员权限
- 向导里可选安装目录、是否创建桌面快捷方式、是否登录时自启动
- 卸载：**设置 → 应用 → 已安装的应用 → 易远传**。站点配置与凭据会保留

## 第一次使用

1. 装完后打开资源管理器，导航窗格里点「易远传」（或双击桌面快捷方式打开客户端窗口）
2. 在客户端里**添加站点**：主机、端口、用户名；密码存进 Windows 凭据管理器
   （SFTP 也可以直接复用 `~/.ssh/config` 里已有的主机别名）
3. 回到资源管理器，进站点目录就是远程文件系统；右键有权限、终端、复制等命令

## 已知限制（Alpha）

- **Windows 11 上“复制 / 拖拽到本地”存在异常** —— 已收到实测报告，正在排查
- 上传/下载尚未并入统一操作队列的进度与暂停体系（复制/移动本身可用）
- 深相对路径经剪贴板协议无法表达 —— 已改为**明确拒绝**而不是静默出错
- SFTP 上“多而小文件”的传输性能与完整性是下一步工作（ERF 协议：并行会话 / 打包流 + 校验）
- 只有 per-user 安装；不做 MSIX（MSIX 不支持这类 in-proc Shell 扩展），也暂不做 per-machine 安装
- 界面目前只有简体中文与英文

## 出问题了怎么办

1. 运行时日志：`%LOCALAPPDATA%\ExplorerRemoteFs\logs\remotefs-debug.log`
2. 安装/卸载问题：用 `/LOG="C:\path\setup.log"` 重跑一次，把日志一起附上
3. 到 [Issues](https://github.com/RyzeZhou/ExploreRemoteFiles/issues) 报告，
   写清 Windows 版本、协议（SFTP/FTP）、操作步骤与现象

## 从源码构建

需要 VS2022（C++ 工具集 + Windows SDK）、.NET 8 SDK、Inno Setup 6/7：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-release.ps1   # 扩展 DLL / CLI / 客户端
powershell -ExecutionPolicy Bypass -File src-setup\build-inno.ps1    # 出安装包
powershell -ExecutionPolicy Bypass -File src-setup\inno-test.ps1     # 自检：装到临时目录→断言→卸载
```

架构一句话：资源管理器里跑一个 C++ Shell 命名空间扩展（进程内、只读缓存、绝不阻塞 UI），
网络动作交给常驻的 WPF 客户端与一次性 CLI 子进程 —— **资源管理器进程永远不做网络 I/O**。

## 许可

[MIT](LICENSE)。作者 RyzeZhou。
