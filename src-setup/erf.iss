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
;    · 装/卸前临时关掉资源管理器自动重启 + 结束常驻客户端与 CLI，见 [Code]
;    · 卸载保留站点配置与凭据；HideDesktopIcons 那种"Windows 自己的键"只动我们那一个值
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
Name: "addtopath"; Description: "把命令行工具 ExplorerRemoteFs.Cli.exe 加进 PATH"; Flags: unchecked

[Files]
; 先拷不会被占用的东西（几百 MB）：cli\、client\、说明文件。
; 这一步**完全不碰资源管理器** —— 以前是"一上来就杀 explorer，等整包拷完才重启"，
; 用户要面对十几秒的黑屏，纯属自找的。
Source: "{#PayloadDir}\cli\*";    DestDir: "{app}\cli";    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\client\*"; DestDir: "{app}\client"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
; 扩展 DLL 是**唯一**被 explorer 映射着的文件，放到最后，并且只在写它的前后各停一次
; explorer（BeforeInstall/AfterInstall）—— 黑屏窗口就只有这一个文件的拷贝时间（1 秒级）。
; 全新安装时这个 DLL 还不存在，StopShellForDll 会直接返回，连停都不停。
Source: "{#PayloadDir}\ExplorerDataProviderFtp.dll"; DestDir: "{app}"; Flags: ignoreversion; \
    BeforeInstall: StopShellForDll; AfterInstall: StartShellAfterDll
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

[Run]
; 装完就启动常驻服务（静默安装也一样启动——否则静默升级完托盘是空的，要等下次登录）
Filename: "{app}\client\RemoteFsClient.exe"; Parameters: "--background"; Flags: nowait runhidden
Filename: "{app}\README.txt"; Description: "查看说明文件"; Flags: shellexec postinstall skipifsilent unchecked

[UninstallRun]
; 卸载前先结束占用文件的进程（卸载器自己也会在 [Code] 里再杀一次，双保险）
Filename: "{cmd}"; Parameters: "/c taskkill /IM RemoteFsClient.exe /F /T"; Flags: runhidden; RunOnceId: "StopClient"
Filename: "{cmd}"; Parameters: "/c taskkill /IM ExplorerRemoteFs.Cli.exe /F"; Flags: runhidden; RunOnceId: "StopCli"

[UninstallDelete]
; 应用运行时可能落在安装目录里的东西（日志本体在 %LOCALAPPDATA%，不在这里）
Type: filesandordirs; Name: "{app}\cli"
Type: filesandordirs; Name: "{app}\client"

[Messages]
; "准备安装"页上的提醒（安装前必读）。%n 是换行。
chinese.ReadyLabel2b=单击"安装"开始安装。%n%n注意：升级安装时，会在替换资源管理器扩展 DLL 的那几秒终止 explorer.exe —— 桌面会短暂黑屏、任务栏与文件管理器暂时不可用（通常 1–3 秒），随后自动恢复；全新安装不需要终止。卸载同理，会有单独的确认框。
english.ReadyLabel2b=Click Install to continue.%n%nNOTE: when upgrading, explorer.exe is terminated for the 1-3 seconds it takes to replace the shell extension DLL (the desktop and taskbar disappear briefly, then come back automatically). A fresh install does not need this; uninstalling asks for its own confirmation.

[Code]
var
  AutoRestartSaved: Boolean;        { 我们是否改过 AutoRestartShell }
  AutoRestartWas: Cardinal;         { 改之前的值（RegQueryDWordValue 要 Cardinal） }
  AutoRestartExisted: Boolean;      { 改之前到底有没有这个值 }
  ShellStopped: Boolean;            { 当前 explorer 是否被我们停了（要负责拉回来） }

{ 让资源管理器立刻发现新的命名空间项 —— 不杀进程也能刷新 }
procedure SHChangeNotify(wEventID: Longint; uFlags: Cardinal; dwItem1, dwItem2: Cardinal);
  external 'SHChangeNotify@shell32.dll stdcall';

{ ─────────────────────────────────────────────────────────────────────────
  资源管理器（explorer.exe）处理总原则
  硬约束：ExplorerDataProviderFtp.dll 被 explorer 映射着，映射期间既不能覆盖也不能删除。
  但**只有这一个文件**有这个约束。所以：
    · 安装：先拷完 cli\/client\/说明（不碰 explorer），最后写 DLL 时用 BeforeInstall/
      AfterInstall 各停一次 —— 黑屏窗口 = 一个 400 KB 文件的拷贝时间。
    · 卸载：Inno 的卸载器没有"删到某个文件之前"的钩子，DLL 的删除发生在中途，
      所以只能整段停，但那段只是删文件，同样是秒级。
  另外：杀掉 explorer 后 Windows 默认会立刻把它拉起来（AutoRestartShell），新 explorer
  马上又把旧 DLL 读回内存 —— 于是"覆盖安装报成功、版本还是旧的"。所以停之前先关自动重启，
  拉回来之前恢复原值（原来没有这个值就删掉，绝不在用户机器上留垃圾设置）。
  ───────────────────────────────────────────────────────────────────────── }
procedure SaveAndDisableAutoRestart();
begin
  if AutoRestartSaved then
    Exit;
  AutoRestartExisted := RegQueryDWordValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon',
                                           'AutoRestartShell', AutoRestartWas);
  AutoRestartSaved := True;
  RegWriteDWordValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon', 'AutoRestartShell', 0);
end;

procedure RestoreAutoRestart();
begin
  if not AutoRestartSaved then
    Exit;
  if AutoRestartExisted then
    RegWriteDWordValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon',
                       'AutoRestartShell', AutoRestartWas)
  else
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows NT\CurrentVersion\Winlogon', 'AutoRestartShell');
  AutoRestartSaved := False;
end;

procedure KillByName(const ExeName: String);
var
  Code: Integer;
begin
  Exec(ExpandConstant('{cmd}'), '/c taskkill /IM ' + ExeName + ' /F /T >nul 2>&1', '',
       SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure StartExplorer();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{win}\explorer.exe'), '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

{ 结束时把一切恢复原状（中途取消/失败也要走这里） }
procedure BringShellBack();
begin
  if ShellStopped then
  begin
    RestoreAutoRestart();
    StartExplorer();
    ShellStopped := False;
  end
  else
    RestoreAutoRestart();
end;

{ ── 安装路径：只结束常驻服务与 CLI（它们锁着 cli\*.exe、client\*.exe），不碰 explorer ── }
procedure StopHelpersForInstall();
begin
  KillByName('RemoteFsClient.exe');
  KillByName('ExplorerRemoteFs.Cli.exe');
  Sleep(400);
end;

{ ── 写扩展 DLL 之前：只有"升级"才需要（全新安装时旧 DLL 不存在，没东西被映射）── }
procedure StopShellForDll();
begin
  if not FileExists(ExpandConstant('{app}\ExplorerDataProviderFtp.dll')) then
    Exit;
  SaveAndDisableAutoRestart();
  KillByName('explorer.exe');
  Sleep(700);
  ShellStopped := True;
end;

procedure StartShellAfterDll();
begin
  BringShellBack();
end;

{ ── 卸载：整段停（DLL 的删除在文件队列中途），但这段只有几秒 ── }
procedure StopEverythingForUninstall();
begin
  SaveAndDisableAutoRestart();
  StopHelpersForInstall();
  KillByName('explorer.exe');
  Sleep(800);
  ShellStopped := True;
end;

{ ─────────────────────────────────────────────────────────────────────────
  确认框：把"会终止资源管理器"这件事讲清楚，用户点"确定"才继续。
  静默安装（/SILENT、/VERYSILENT、/SUPPRESSMSGBOXES）不弹框，直接按默认按钮继续 ——
  自动化与无人值守部署不能被一个对话框卡住。
  ───────────────────────────────────────────────────────────────────────── }
function ConfirmExplorerStop(const Action: String): Boolean;
begin
  Result := SuppressibleMsgBox(
    '注意：' + Action + '过程可能需要终止资源管理器（explorer.exe）进程。' + #13#10 + #13#10 +
    '· 升级安装与卸载时，会在替换/删除扩展 DLL 的那几秒终止它' + #13#10 +
    '· 桌面会短暂黑屏，任务栏与文件管理器暂时不可用（通常 1–3 秒），随后自动恢复' + #13#10 +
    '· 正在进行的文件复制/下载请等它结束后再继续' + #13#10 +
    '· 全新安装不会终止资源管理器' + #13#10 + #13#10 +
    '确认后继续。',
    mbConfirmation, MB_OKCANCEL, IDOK) = IDOK;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  NeedsRestart := False;
  { 确认只做一次，而且就在"点下安装按钮"这一步 —— 不按"这次到底要不要停 explorer"去
    精细判断：用户读到的信息应该是稳定的，而不是"有时弹有时不弹"。真正的停与不停由
    StopShellForDll 决定（全新安装不会停）。静默安装不弹框（见 ConfirmExplorerStop）。 }
  if not ConfirmExplorerStop('安装') then
    Result := '安装已取消：你选择了不终止资源管理器。扩展 DLL 无法在被占用的状态下替换。';
end;

function InitializeUninstall(): Boolean;
begin
  Result := ConfirmExplorerStop('卸载');
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
    StopHelpersForInstall()          { 只结束常驻服务/CLI；explorer 交给 DLL 条目的钩子 }
  else if CurStep = ssPostInstall then
  begin
    WriteErfProtocol();
    { 新装的命名空间项要立刻出现在导航窗格里：发个关联变更通知即可，不必重启 explorer }
    SHChangeNotify($08000000, 0, 0, 0);   { SHCNE_ASSOCCHANGED }
  end;
end;

procedure DeinitializeSetup();
begin
  { 兜底：中途取消/失败也要把 explorer 拉回来、把设置还回去 }
  BringShellBack();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Owner: String;
begin
  if CurUninstallStep = usUninstall then
    StopEverythingForUninstall()
  else if CurUninstallStep = usPostUninstall then
  begin
    { erf:// 只有确认是我们注册的才删（别人后来抢注了就不动） }
    if RegQueryStringValue(HKCU, 'Software\Classes\erf', 'ERF.HandlerOwner', Owner) and
       (Owner = 'ExplorerRemoteFs') then
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\erf');
    SHChangeNotify($08000000, 0, 0, 0);
    BringShellBack();
  end;
end;

procedure DeinitializeUninstall();
begin
  BringShellBack();
end;
