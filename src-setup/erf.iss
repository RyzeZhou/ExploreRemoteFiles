; ============================================================================
;  易远传 (Explorer Remote Files) —— Inno Setup 安装脚本
;
;  编译：  src-setup\build-inno.ps1        （内部就是 ISCC.exe erf.iss）
;  产物：  dist\Erf-0.1-Alpha-Setup.exe    （单文件、自包含，双击即经典向导）
;
;  为什么是 Inno：这套"选择安装目录 → 下一步 → 安装 → 完成"的向导、升级/回滚、
;  "应用和功能"里的卸载项、静默安装（/VERYSILENT /DIR /LOG）、数字签名接入，
;  Inno 都已经做了十几年。之前手写的那版（ErfSetup.cpp/Wizard.cpp）已删除，
;  需要时去 git 历史里找（提交 daed431 附近）。
;
;  与老版 install.ps1 的语义对照（都保留）：
;    · 仅当前用户（PrivilegesRequired=lowest），全部写 HKCU，不碰 UAC
;    · erf:// 协议带归属标记，已被别人占用就中止安装而不是覆盖
;    · 装/卸前结束常驻客户端与 CLI（它们锁着 cli\*.exe、client\*.exe）
;    · 卸载保留站点配置与凭据；HideDesktopIcons 那种"Windows 自己的键"只动我们那一个值
;
;  ★ 2026-09-18 重写"资源管理器占用"这一节 —— 安装与卸载都不再终止 explorer.exe。
;
;  旧做法（已删除）是"先关掉 AutoRestartShell、再 taskkill /IM explorer.exe /F /T，
;  替换完 DLL 再把 explorer 拉回来"。它在开发机上"看起来能跑"，在用户双击安装时必然出事：
;
;    1) Inno 的安装程序会把自己解压到临时目录再跑一遍，进程链是
;         explorer.exe → Erf-…-Setup.exe → Erf-…-Setup.tmp（真正的安装进程）
;       卸载器同理（explorer.exe → unins000.exe → unins000.tmp）。
;    2) taskkill 的 /T 是"连同整棵子进程树一起杀"。于是这条命令把**安装程序自己**
;       也杀了 —— 安装中断在写 DLL 那一步，后面的 [Registry] 段一条都没执行，
;       导航窗格里当然就没有"易远传"；卸载则连"删注册表"都没走到，条目删不掉。
;    3) 更糟的是 AutoRestartShell 已经被写成 0 而没人恢复它 → Windows 不会再把
;       explorer 拉起来 → 桌面一直黑着，用户只能注销或重启。
;    （为什么自动化测试没发现：测试都从 PowerShell 启动 Setup.exe，父进程是 pwsh，
;      不在 explorer 的进程树里，所以 /T 波及不到测试进程。只有"人双击"才会中招。）
;
;  现在的做法：**一个进程都不杀**，用改名绕开"已映射文件不能覆盖/删除"这个约束 ——
;  NTFS 允许对已被映射的文件改名（只是改目录项，文件对象仍在内存里），
;  所以先把旧的 ExplorerDataProviderFtp.dll 改成 .old，再把新文件写到原路径即可。
;  改名后的旧文件在下次登录时由 RunOnce 清掉（中间不需要重启，也不会黑屏）。
;  代价只有一个：explorer 内存里还加载着旧 DLL，**新版本要等 explorer 下次启动才生效** ——
;  所以"附加任务"里给了可选的"立即重启资源管理器"（不带 /T 的安全重启）。
; ============================================================================

#define AppName        "易远传 (Explorer Remote Files)"
#define AppShortName   "易远传"
#define AppVersion     "0.1-Alpha"
#define AppPublisher   "RyzeZhou"
#define NsFolderClsid  "{{C816CE0E-728C-4FC9-98E5-D0B35B384597}"
#define CtxClsid       "{{CB8F539D-3B97-4473-9E07-C8248C53248E}"
#define PropsClsid     "{{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}"

#ifndef PayloadDir
  #define PayloadDir "..\dist\ExplorerRemoteFs-win-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
; AppId 决定"应用和功能"里的身份与升级识别（卸载项注册表键名 ExplorerRemoteFs_is1），一旦发布就不能再改。
#ifndef ExpectedDllSha256
  ; 由 build-inno.ps1 传入：随包 DLL 的 SHA256。安装完用它自校验"到底换没换 DLL"。
  #define ExpectedDllSha256 ""
#endif

AppId=ExplorerRemoteFs
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion=0.1.0.0
DefaultDirName={localappdata}\ExplorerRemoteFs
DefaultGroupName={#AppShortName}
DisableProgramGroupPage=yes
DisableDirPage=no
; 只装当前用户：不弹 UAC，不需要管理员（per-machine 会让 HKCU 注册与凭据隔离复杂化，
; 真要做企业部署时再单开一个 per-machine 的构建）
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
; 必须声明"以 64 位模式安装"：否则 32 位的安装器会把 HKCU\Software\Classes\CLSID\... 重定向写进
; Wow6432Node，64 位资源管理器根本看不到我们的 in-proc 服务器（扩展直接不加载）。
; 实测 2026-09-17：不加这一行时注册项全部落进 Wow6432Node\CLSID，64 位视图里还是旧值。
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Erf-{#AppVersion}-Setup
SetupIconFile=..\assets\erf.ico
WizardStyle=classic
Compression=lzma2/fast
SolidCompression=no
SetupLogging=yes
CloseApplications=no
RestartApplications=no
AllowNoIcons=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\client\RemoteFsClient.exe
AppComments=把远程 Linux 主机（SFTP/FTP）挂进资源管理器导航窗格
AppSupportURL=https://github.com/RyzeZhou/ExploreRemoteFiles

[Languages]
Name: "chinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "登录时自动启动常驻服务（托盘显示远程连接与传输状态）"; Flags: checkedonce
; GUI 程序按惯例要问一句桌面快捷方式（默认勾选；中文用 Inno 自带翻译的 {cm:CreateDesktopIcon}）
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "addtopath"; Description: "把命令行工具 ExplorerRemoteFs.Cli.exe 加进 PATH"; Flags: unchecked
; 只有"目标目录里已经有旧 DLL"（= 升级）时才有意义，所以用 Check 控制是否出现。
; 默认不勾：这是唯一会让桌面闪一下的动作，必须由用户自己选。
Name: "restartshell"; Description: "安装完成后重启资源管理器，让新版本扩展立即生效（桌面会闪一下，约 1 秒）"; \
    Flags: unchecked; Check: HasExistingDll

[Files]
; 先拷不会被占用的东西（几百 MB）：cli\、client\、说明文件。
; 这一步完全不碰资源管理器，也不碰 explorer。
Source: "{#PayloadDir}\cli\*";    DestDir: "{app}\cli";    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\client\*"; DestDir: "{app}\client"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
; 扩展 DLL 是唯一可能被 explorer 映射着的文件：写它之前先把旧的挪开（删除或改名，见
; PrepareDllSlotForInstall）—— 这样"覆盖被占用文件"这个失败模式根本不会出现，也不需要杀 explorer。
Source: "{#PayloadDir}\ExplorerDataProviderFtp.dll"; DestDir: "{app}"; Flags: ignoreversion; \
    BeforeInstall: PrepareDllSlotForInstall; AfterInstall: VerifyInstalledDll
; 翻译模板进用户配置目录，但**只在不存在时**写（升级不覆盖用户改过的翻译）
Source: "{#PayloadDir}\explorer-translations.yaml"; DestDir: "{userappdata}\ExplorerRemoteFs"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#PayloadDir}\explorer-translations.example.yaml"; DestDir: "{userappdata}\ExplorerRemoteFs"; Flags: onlyifdoesntexist uninsneveruninstall

[Registry]
; ── 路径（C++ 侧 GetCliPath() 读它）────────────────────────────────────────
Root: HKCU; Subkey: "Software\ExplorerRemoteFs"; ValueType: string; ValueName: "CliPath"; \
    ValueData: "{app}\cli\ExplorerRemoteFs.Cli.exe"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\ExplorerRemoteFs"; ValueType: string; ValueName: "ClientPath"; \
    ValueData: "{app}\client\RemoteFsClient.exe"; Flags: uninsdeletevalue
; 注意：这里**不用** uninsdeletekey —— 同一个键下还有用户的显示设置（SizeFormat 等），
; 卸载只该删我们自己写的那两个值。

; ── 命名空间扩展本体 ──────────────────────────────────────────────────────
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}"; ValueType: string; ValueName: ""; \
    ValueData: "{#AppShortName} (Explorer Remote Files)"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}\InprocServer32"; ValueType: string; ValueName: ""; \
    ValueData: "{app}\ExplorerDataProviderFtp.dll"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; \
    ValueData: "Apartment"; Flags: uninsdeletevalue
; 命名空间图标用扩展 DLL 里的图标资源（id 101，见 ExplorerDataProvider.rc 的 IDI_ERF）
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}\DefaultIcon"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\ExplorerDataProviderFtp.dll"",-101"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}\ShellFolder"; ValueType: dword; ValueName: "Attributes"; \
    ValueData: "$A8000020"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}"; ValueType: dword; ValueName: "System.IsPinnedToNameSpaceTree"; \
    ValueData: "1"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\CLSID\{#NsFolderClsid}"; ValueType: dword; ValueName: "SortOrderIndex"; \
    ValueData: "$42"; Flags: uninsdeletevalue

; ── 右键菜单 handler ──────────────────────────────────────────────────────
Root: HKCU; Subkey: "Software\Classes\CLSID\{#CtxClsid}"; ValueType: string; ValueName: ""; \
    ValueData: "ERF context menu"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{#CtxClsid}\InprocServer32"; ValueType: string; ValueName: ""; \
    ValueData: "{app}\ExplorerDataProviderFtp.dll"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\CLSID\{#CtxClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; \
    ValueData: "Apartment"; Flags: uninsdeletevalue

; ── 属性页 handler ────────────────────────────────────────────────────────
Root: HKCU; Subkey: "Software\Classes\CLSID\{#PropsClsid}"; ValueType: string; ValueName: ""; \
    ValueData: "ERF property pages"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\CLSID\{#PropsClsid}\InprocServer32"; ValueType: string; ValueName: ""; \
    ValueData: "{app}\ExplorerDataProviderFtp.dll"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\CLSID\{#PropsClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; \
    ValueData: "Apartment"; Flags: uninsdeletevalue

; ── 挂到我们自己的两个 ProgID 上（目录用 CoreType，文件用 FileType）────────
Root: HKCU; Subkey: "Software\Classes\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\{#CtxClsid}"; \
    ValueType: string; ValueName: ""; ValueData: "{#CtxClsid}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\{#PropsClsid}"; \
    ValueType: string; ValueName: ""; ValueData: "{#PropsClsid}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\RemoteFsFileType\shellex\ContextMenuHandlers\{#CtxClsid}"; \
    ValueType: string; ValueName: ""; ValueData: "{#CtxClsid}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\RemoteFsFileType\shellex\PropertySheetHandlers\{#PropsClsid}"; \
    ValueType: string; ValueName: ""; ValueData: "{#PropsClsid}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\RemoteFsFileType\shell\open\command"; ValueType: string; ValueName: ""; \
    ValueData: """{app}\cli\ExplorerRemoteFs.Cli.exe"" open ""%1"""; Flags: uninsdeletekey

; ── 导航窗格里的入口 + 隐藏它自己的桌面图标 ───────────────────────────────
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\{#NsFolderClsid}"; \
    ValueType: string; ValueName: ""; ValueData: "{#AppShortName}"; Flags: uninsdeletekey
; 这个键是 Windows 的（同键下还可能有 {20D04FE0-...}=0 这种系统值），
; 只写我们 CLSID 命名的值，卸载也只删这一个值 —— 绝不要整体重建该键。
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel"; \
    ValueType: dword; ValueName: "{#NsFolderClsid}"; ValueData: "1"; Flags: uninsdeletevalue

; ── 登录自启（用户可在"任务"页取消勾选）──────────────────────────────────
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
    ValueName: "ExplorerRemoteFs"; ValueData: """{app}\client\RemoteFsClient.exe"" --background"; \
    Flags: uninsdeletevalue; Tasks: startup

; ── PATH（可选任务）──────────────────────────────────────────────────────
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}\cli"; Tasks: addtopath; Check: NeedsAddPath('{app}\cli')

[Icons]
Name: "{userprograms}\{#AppShortName}"; Filename: "{app}\client\RemoteFsClient.exe"
; 桌面快捷方式：指向常驻客户端的 GUI（托盘程序，双击就是客户端窗口）。
; 注意这和"隐藏命名空间自带的桌面图标"是两回事 —— 那个是资源管理器命名空间项，这个是普通 .lnk。
Name: "{userdesktop}\{#AppShortName}"; Filename: "{app}\client\RemoteFsClient.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; 装完就启动常驻服务（静默安装也一样启动——否则静默升级完托盘是空的，要等下次登录）
Filename: "{app}\client\RemoteFsClient.exe"; Parameters: "--background"; Flags: nowait runhidden
Filename: "{app}\README.txt"; Description: "查看说明文件"; Flags: shellexec postinstall skipifsilent unchecked

[UninstallRun]
; 卸载前先结束占用文件的进程（卸载器自己也会在 [Code] 里再杀一次，双保险）。
; 注意这里**没有 /T**：/T 会连子进程树一起杀，而卸载器本身可能就是被调用方的子进程。
Filename: "{cmd}"; Parameters: "/c taskkill /IM RemoteFsClient.exe /F"; Flags: runhidden; RunOnceId: "StopClient"
Filename: "{cmd}"; Parameters: "/c taskkill /IM ExplorerRemoteFs.Cli.exe /F"; Flags: runhidden; RunOnceId: "StopCli"

[UninstallDelete]
; 应用运行时可能落在安装目录里的东西（日志本体在 %LOCALAPPDATA%，不在这里）
Type: filesandordirs; Name: "{app}\cli"
Type: filesandordirs; Name: "{app}\client"

[Messages]
; "准备安装"页上的提醒（安装前必读）。%n 是换行。
chinese.ReadyLabel2b=单击"安装"开始安装。%n%n安装过程不会终止资源管理器，也不会打断你正在进行的文件复制。%n如果是升级安装，新版本的扩展会在资源管理器下次启动时生效 —— 需要立刻生效，就在下一页勾选"重启资源管理器"。
english.ReadyLabel2b=Click Install to continue.%n%nSetup does NOT terminate explorer.exe and does not interrupt file operations in progress.%nOn an upgrade the new shell extension takes effect the next time Explorer starts - tick "restart Explorer" on the previous page to do it now.
chinese.FinishedLabel=安装完成。%n%n「易远传」已加入资源管理器导航窗格；如果它没有立刻出现，请重启资源管理器（任务管理器 → "Windows 资源管理器" → 重新启动）。%n%n常驻服务已在后台启动，托盘图标可见。
english.FinishedLabel=Setup has finished installing. %n%n"ERF sites" has been added to the Explorer navigation pane; if it does not show up right away, restart Windows Explorer (Task Manager -> Windows Explorer -> Restart).%n%nThe resident service has been started in the background.

[Code]
var
  LeftoverDll: String;   { 被挪开的旧 DLL 的完整路径（空 = 没有需要清理的） }

{ 让资源管理器立刻发现新的命名空间项 —— 不杀进程也能刷新 }
procedure SHChangeNotify(wEventID: Longint; uFlags: Cardinal; dwItem1, dwItem2: Cardinal);
  external 'SHChangeNotify@shell32.dll stdcall';

{ 判断桌面 shell 是否活着（0 = 没有 shell，桌面是黑的） }
function GetShellWindow(): HWND;
  external 'GetShellWindow@user32.dll stdcall';

{ ─────────────────────────────────────────────────────────────────────────
  为什么杀进程的地方**一律不带 /T**（这是本次修复的核心，别改回去）
  taskkill /T 会连同目标进程的整棵子进程树一起杀。而双击启动时：
      explorer.exe → Erf-…-Setup.exe（解压壳） → Erf-…-Setup.tmp（真正干活的）
  安装程序自己就在 explorer 的子树里，于是 /T 把安装程序一起杀了：
  安装中断在写 DLL 那一步，后面的 [Registry] 一条都没写（导航窗格没有"易远传"），
  AutoRestartShell 被留成 0（桌面黑了就再也不回来）。卸载器同理（条目删不掉）。
  ───────────────────────────────────────────────────────────────────────── }
procedure KillByName(const ExeName: String);
var
  Code: Integer;
begin
  Exec(ExpandConstant('{cmd}'), '/c taskkill /IM ' + ExeName + ' /F >nul 2>&1', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
end;

{ 常驻客户端与 CLI 会锁住 cli\*.exe / client\*.exe，装/卸前都要先结束它们。
  它们不是资源管理器，杀掉对用户完全不可见。 }
procedure StopHelpers();
begin
  KillByName('RemoteFsClient.exe');
  KillByName('ExplorerRemoteFs.Cli.exe');
  Sleep(400);
end;

{ 挑一个还能用的 ".old" 名字：优先复用，被占用（上次留下的、还被映射着）就换下一个。 }
function PickFreePath(const Base: String): String;
var
  I: Integer;
begin
  Result := Base + '.old';
  if not FileExists(Result) then Exit;
  if DeleteFile(Result) then Exit;
  for I := 2 to 20 do
  begin
    Result := Base + '.old' + IntToStr(I);
    if not FileExists(Result) then Exit;
    if DeleteFile(Result) then Exit;
  end;
  Result := Base + '.old' + GetDateTimeString('yyyymmdd-hhnnss', '-', ':');
end;

{ ── 安装：写扩展 DLL 之前，先把旧的挪开 ──────────────────────────────────
  先试着直接删（explorer 没加载过它时能删掉，目录最干净）；
  删不掉说明 explorer 映射着它 —— 这时改名（对已映射文件是允许的），
  改名后的旧文件交给下次登录的 RunOnce 清理。全程不碰 explorer。 }
procedure PrepareDllSlotForInstall();
var
  Src, Dst: String;
begin
  Src := ExpandConstant('{app}\ExplorerDataProviderFtp.dll');
  if not FileExists(Src) then
    Exit;                                  { 全新安装：目标还不存在，直接写 }
  if DeleteFile(Src) then
  begin
    Log('install: old extension DLL deleted');
    Exit;
  end;
  Dst := PickFreePath(Src);
  if RenameFile(Src, Dst) then
  begin
    LeftoverDll := Dst;
    Log('install: old extension DLL renamed to ' + Dst + ' (still mapped by explorer)');
  end
  else
  begin
    Log('install: WARNING cannot rename ' + Src + ' -- the copy below will fail');
    if not WizardSilent() then
      MsgBox('无法替换资源管理器扩展 DLL：它正被资源管理器占用。' + #13#10 + #13#10 +
             '请重启资源管理器（任务管理器 → "Windows 资源管理器" → 重新启动）后重跑安装程序。',
             mbError, MB_OK);
  end;
end;

{ ── 安装：写完之后自校验"到底换没换" ─────────────────────────────────────
  踩过的坑：explorer 占用着 DLL 时升级会"成功但不替换"，于是旧 DLL 配新 CLI，
  取文件失败被笼统报成"执行读取操作时发生磁盘错误"。现在先挪开再写，
  正常路径下这里必定一致；不一致就说明写失败了，必须让安装失败而不是静默放过。 }
procedure VerifyInstalledDll();
var
  Expected, Actual, Target: String;
begin
  Expected := '{#ExpectedDllSha256}';
  if Expected = '' then
    Exit;                                   { 没传哈希就跳过（手工编译时） }
  Target := ExpandConstant('{app}\ExplorerDataProviderFtp.dll');
  if not FileExists(Target) then
  begin
    MsgBox('安装后找不到扩展 DLL：' + Target, mbError, MB_OK);
    RaiseException('extension DLL missing after install');
  end;
  Actual := GetSHA256OfFile(Target);
  if CompareText(Expected, Actual) <> 0 then
  begin
    MsgBox('扩展 DLL 没有被真正替换（安装目录里的文件与随包文件不一致）。' + #13#10 + #13#10 +
           '最常见原因：资源管理器正占用着它。请重启资源管理器后重跑安装程序。' + #13#10 + #13#10 +
           '期望: ' + Expected + #13#10 + '实际: ' + Actual, mbError, MB_OK);
    Log('install: FAILED extension DLL hash mismatch');
    Abort();   { 静默安装里 RaiseException 只报不拦（实测退出码仍是 0），Abort 才会真的终止 }
  end;
  Log('install: extension DLL verified, sha256=' + Actual);
end;

{ ── 卸载：先试着直接删 DLL，删不掉（explorer 映射着）就改名 ──────────────
  卸载器按记录去删文件时，原路径上已经没有这个文件了，它会跳过；
  .old 留给下次登录的 RunOnce。 }
procedure ReleaseDllForUninstall();
var
  Src, Dst: String;
begin
  Src := ExpandConstant('{app}\ExplorerDataProviderFtp.dll');
  if not FileExists(Src) then
    Exit;
  if DeleteFile(Src) then
  begin
    Log('uninstall: extension DLL deleted');
    Exit;
  end;
  Dst := PickFreePath(Src);
  if RenameFile(Src, Dst) then
    Log('uninstall: extension DLL renamed to ' + Dst + ' (still mapped by explorer)')
  else
    Log('uninstall: WARNING cannot rename ' + Src);
  LeftoverDll := Dst;
end;

{ 把残留文件交给"下次登录"清理：RunOnce 里一条 del + 一条 rd。
  注意 RunOnce 的值是**整条命令行**，必须写全 "cmd.exe" /c —— 老版本只写了 "/c del ..."，
  那会被当成程序名，等于什么都没清理。
  rd 故意不带 /s：只删空目录，万一用户在下次登录前又装回来了，这里绝不会误删新装的文件。 }
procedure ScheduleLeftoverCleanup();
var
  Cmd: String;
begin
  if LeftoverDll = '' then
    Exit;
  Cmd := '"' + ExpandConstant('{cmd}') + '" /c del /f /q "' + LeftoverDll + '" & rd "'
         + ExpandConstant('{app}') + '" 2>nul';
  RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\RunOnce', 'ErfCleanup', Cmd);
  Log('cleanup scheduled at next logon: ' + Cmd);
end;

{ ── 可选的"立即重启资源管理器"（只有用户勾了那个任务才会走到）───────────
  依旧不带 /T；杀之前先把 AutoRestartShell 顶成 1（万一被谁关过，
  系统就不会把桌面拉回来 —— 那正是用户上一次看到的"黑屏不恢复"），
  杀完盯着 shell 回没回来，5 秒还没回来就手动拉一个，绝不把用户丢在黑屏里。 }
procedure RestartShellNow();
var
  Code, I: Integer;
  Had: Boolean;
  Prev: Cardinal;
begin
  Had := RegQueryDWordValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon',
                            'AutoRestartShell', Prev);
  if Had and (Prev = 0) then
    RegWriteDWordValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon',
                       'AutoRestartShell', 1);
  Log('restarting explorer.exe (no /T)');
  KillByName('explorer.exe');
  Sleep(1000);
  for I := 1 to 10 do
  begin
    if GetShellWindow() <> 0 then
    begin
      Log('shell is back');
      Break;
    end;
    Sleep(500);
  end;
  if GetShellWindow() = 0 then
  begin
    Log('shell did not come back by itself -- starting explorer.exe manually');
    Exec(ExpandConstant('{win}\explorer.exe'), '', '', SW_SHOWNORMAL, ewNoWait, Code);
    Sleep(1500);
  end;
  { 还回用户原来的设置（原来没有这个值就把我们加的删掉，绝不留垃圾设置） }
  if Had then
    RegWriteDWordValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon',
                       'AutoRestartShell', Prev)
  else
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon',
                   'AutoRestartShell');
end;

{ 升级才有意义：目标目录里已经有旧 DLL 才显示"重启资源管理器"这个任务 }
function HasExistingDll(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\ExplorerDataProviderFtp.dll'));
end;

{ ── erf:// 归属检查：别人占用了就中止，绝不覆盖 ── }
function InitializeSetup(): Boolean;
var
  Owner: String;
begin
  Result := True;
  if RegQueryStringValue(HKLM, 'Software\Classes\erf', 'ERF.HandlerOwner', Owner) and
     (Owner <> 'ExplorerRemoteFs') then
  begin
    MsgBox('erf:// 地址协议已经被其它程序注册（' + Owner + '）。' + #13#10 +
           '为了避免覆盖别人的注册，安装已中止。', mbError, MB_OK);
    Result := False;
  end
  else if RegQueryStringValue(HKCU, 'Software\Classes\erf', 'ERF.HandlerOwner', Owner) and
          (Owner <> 'ExplorerRemoteFs') then
  begin
    MsgBox('erf:// 地址协议已经被其它程序注册（' + Owner + '）。' + #13#10 +
           '为了避免覆盖别人的注册，安装已中止。', mbError, MB_OK);
    Result := False;
  end;
end;

{ erf:// 的注册项由 [Code] 写，卸载时也只有"确认是我们写的"才删 }
procedure WriteErfProtocol();
begin
  RegWriteStringValue(HKCU, 'Software\Classes\erf', '', 'URL: Explorer Remote Files');
  RegWriteStringValue(HKCU, 'Software\Classes\erf', 'URL Protocol', '');
  RegWriteStringValue(HKCU, 'Software\Classes\erf', 'ERF.HandlerOwner', 'ExplorerRemoteFs');
  RegWriteStringValue(HKCU, 'Software\Classes\erf\shell\open\command', '',
                      '"' + ExpandConstant('{app}\client\RemoteFsClient.exe') + '" --open-erf "%1"');
end;

function NeedsAddPath(Param: String): Boolean;
var
  OrigPath: String;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    Exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    StopHelpers()                    { 只结束常驻服务/CLI；explorer 全程不动 }
  else if CurStep = ssPostInstall then
  begin
    WriteErfProtocol();
    { 新装的命名空间项要立刻出现在导航窗格里：发个关联变更通知即可，不必重启 explorer }
    SHChangeNotify($08000000, 0, 0, 0);   { SHCNE_ASSOCCHANGED }
    ScheduleLeftoverCleanup();
    if WizardIsTaskSelected('restartshell') then
      RestartShellNow();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Owner: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    { 只结束常驻客户端与 CLI（它们锁着 cli\*.exe / client\*.exe）——桌面上什么都看不见 }
    StopHelpers();
    ReleaseDllForUninstall();
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    { erf:// 只有确认是我们注册的才删（别人后来抢注了就不动） }
    if RegQueryStringValue(HKCU, 'Software\Classes\erf', 'ERF.HandlerOwner', Owner) and
       (Owner = 'ExplorerRemoteFs') then
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\erf');
    SHChangeNotify($08000000, 0, 0, 0);
    ScheduleLeftoverCleanup();
  end;
end;
