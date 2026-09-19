# Inno installer self-check: silent install to a throwaway directory, assert everything,
# silent uninstall, assert the machine is clean again.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src-setup\inno-test.ps1
#
# 2026-09-18: no longer needs to run detached. The installer used to terminate explorer.exe
# on purpose, which could take the calling console down with it; it no longer touches
# explorer at all (see erf.iss header), so a plain foreground run is fine.
#
# Why a UI-less test is enough here: Inno's wizard is Inno's own code -- what *we* have to prove
# is the parts we wrote ([Registry] entries, [Code] steps). The wizard pages themselves are
# checked by eye once (see the -WizardShots switch).
#
# Phase 1c exists because the *real* user-reported failure could not be reproduced by any of the
# phases that launch Setup.exe from PowerShell: those children have pwsh as their parent, so the
# old `taskkill /IM explorer.exe /F /T` never reached them. A double-click makes the installer a
# child of explorer.exe -- and /T then kills the installer itself (install stops right before the
# [Registry] section, so the nav-pane entry never appears; uninstall stops before deleting it).
# Phase 1c reproduces that parent chain by launching a .lnk through explorer.exe.
# ASCII only (PowerShell 5.1 reads BOM-less files as ANSI).
param(
    [string]$Setup = (Join-Path $PSScriptRoot '..\dist\Erf-0.1-Alpha-Setup.exe'),
    [string]$TestDir = "$env:LOCALAPPDATA\ExplorerRemoteFs-InnoTest",
    [string]$LogFile = (Join-Path $PSScriptRoot 'inno-test.log'),
    [switch]$WizardShots
)
$ErrorActionPreference = 'Continue'
$fails = 0
Set-Content -LiteralPath $LogFile -Value ("### inno-test " + (Get-Date -Format 'HH:mm:ss') + " setup=$Setup dir=$TestDir") -Encoding UTF8
function Log([string]$t) { Add-Content -LiteralPath $LogFile -Value $t -Encoding UTF8 }
function Check([string]$name, $ok, $detail) {
    if ($ok) { Log ("PASS  " + $name + "  " + $detail) } else { Log ("FAIL  " + $name + "  " + $detail); $script:fails++ }
}
function RegValue([string]$path, [string]$name) {
    try { return (Get-ItemProperty -Path $path -Name $name -ErrorAction Stop).$name } catch { return $null }
}
function RegDefault([string]$path) {
    # Get-ItemProperty -Name '' does NOT read the default value (throws) -- use the registry API
    try { return (Get-Item -LiteralPath $path -ErrorAction Stop).GetValue('') } catch { return $null }
}
function Sha256([string]$path) {
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $fs = [IO.File]::OpenRead($path)
        try { return (($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) -join '') }
        finally { $fs.Close(); $sha.Dispose() }
    } catch { return '' }
}
function DesktopShortcut() {
    # 按"目标是不是我们的客户端"来找快捷方式，省得在 ASCII 脚本里写中文文件名
    $shell = New-Object -ComObject WScript.Shell
    Get-ChildItem (Join-Path $env:USERPROFILE 'Desktop') -Filter *.lnk -ErrorAction SilentlyContinue |
        Where-Object { ($shell.CreateShortcut($_.FullName)).TargetPath -like '*RemoteFsClient.exe' } |
        Select-Object -First 1
}
function ShellPid() {
    # 桌面 shell 进程 = GetShellWindow() 那个窗口的宿主进程。
    # 别用"explorer.exe 里 PID 最小的那个"：`explorer.exe <文件>` 这种转发进程同样叫 explorer.exe，
    # 而且 PID 会回绕，最小 PID 未必是 shell —— 实测这样写会假报"shell 被杀"。
    if (-not ('W.U' -as [type])) {
        Add-Type -Namespace W -Name U -ErrorAction SilentlyContinue -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
'@
    }
    $h = [W.U]::GetShellWindow()
    if ($h -eq [IntPtr]::Zero) { return 0 }
    [uint32]$owner = 0
    [void][W.U]::GetWindowThreadProcessId($h, [ref]$owner)
    return [int]$owner
}
function SetupProcesses() {
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like 'Erf-*-Setup*' }
}
function Wait-SetupGone([int]$TimeoutSec = 240) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (SetupProcesses)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
function NamespaceVisible([string]$clsid) {
    # 枚举桌面命名空间项：这一步会真的实例化我们的 in-proc 扩展，等于验证"shell 认得这个条目"
    try {
        $shell = New-Object -ComObject Shell.Application
        $hit = @($shell.NameSpace(0).Items() | Where-Object { $_.Path -like "*$clsid*" })
        return $hit.Count -ge 1
    } catch { return $false }
}

$folder = '{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx = '{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$props = '{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'
$erfDelegate = '{A970407D-FE36-4C49-A433-61E605D9DDEA}'
$arp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs_is1'
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runOnce = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
$hk = 'HKCU:\Software\Classes'
$winlogon = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon'
$payloadDll = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\ExplorerRemoteFs-win-x64\ExplorerDataProviderFtp.dll'

function RunExe([string]$exe, [string[]]$exeArgs, [string]$tag) {
    $p = Start-Process -FilePath $exe -ArgumentList $exeArgs -PassThru
    $deadline = (Get-Date).AddMinutes(10)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not $p.HasExited) { Log ("   TIMEOUT: " + $exe); return -1 }
    Log ("   exit=" + $p.ExitCode + "  (" + $tag + ")")
    return $p.ExitCode
}

if (-not (Test-Path $Setup)) { Log ("setup not found: " + $Setup); Log '=== 1 FAILED'; exit 1 }
Log ("setup size: " + [math]::Round((Get-Item $Setup).Length / 1MB, 1) + " MB")

# ---- phase 0: static guard ----
# /T 是本次故障的开关：taskkill /T 连子进程树一起杀，而双击时安装程序就在 explorer 的子树里。
# 这条断言很便宜，但它挡住的正是"又有人顺手把 /T 加回去"。
$issText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'erf.iss'))
$killLines = @([regex]::Matches($issText, '(?m)^\s*[^;\s].*taskkill.*$') | ForEach-Object { $_.Value.Trim() })
$withT = @($killLines | Where-Object { $_ -match '/T\b' })
Check 'no taskkill /T anywhere in erf.iss' ($withT.Count -eq 0) (($killLines -join ' | ') + $(if ($withT.Count) { "  OFFENDERS: " + ($withT -join ' | ') } else { '' }))

# ---- phase 1: silent install (launched from PowerShell) ----
if (Test-Path $TestDir) { Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue }
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
$shellBefore = ShellPid
Log ("   shell pid before install = " + $shellBefore)
$innoLog = Join-Path $PSScriptRoot 'inno-setup-install.log'
Remove-Item $innoLog -Force -ErrorAction SilentlyContinue
$code = RunExe $Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$TestDir",
                        '/TASKS=startup', "/LOG=$innoLog") 'install'
Check 'installer exit code 0' ($code -eq 0) ("exit=" + $code)

Check 'extension dll' (Test-Path "$TestDir\ExplorerDataProviderFtp.dll") "$TestDir\ExplorerDataProviderFtp.dll"
Check 'cli exe' (Test-Path "$TestDir\cli\ExplorerRemoteFs.Cli.exe") 'cli\ExplorerRemoteFs.Cli.exe'
Check 'client exe' (Test-Path "$TestDir\client\RemoteFsClient.exe") 'client\RemoteFsClient.exe'
Check 'readme' (Test-Path "$TestDir\README.txt") 'README.txt'
Check 'uninstaller (unins000.exe)' (Test-Path "$TestDir\unins000.exe") 'unins000.exe'
Check 'roaming translation template' (Test-Path "$env:APPDATA\ExplorerRemoteFs\explorer-translations.yaml") 'appdata yaml'
Check 'installed dll matches payload' ((Sha256 "$TestDir\ExplorerDataProviderFtp.dll") -eq (Sha256 $payloadDll)) 'sha256 compare'
Check 'shell survived the install' ((ShellPid) -eq $shellBefore) ("pid now " + (ShellPid))

Check 'ARP DisplayName' ($null -ne (RegValue $arp 'DisplayName')) (RegValue $arp 'DisplayName')
Check 'ARP DisplayVersion' ((RegValue $arp 'DisplayVersion') -eq '0.1-Alpha') (RegValue $arp 'DisplayVersion')
Check 'ARP InstallLocation' ((RegValue $arp 'InstallLocation') -like "$TestDir*") (RegValue $arp 'InstallLocation')
Check 'ARP UninstallString' ((RegValue $arp 'UninstallString') -like "*unins000.exe*") (RegValue $arp 'UninstallString')
Check 'ARP EstimatedSize' ($null -ne (RegValue $arp 'EstimatedSize')) (RegValue $arp 'EstimatedSize')

Check 'CliPath' ((RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath') -like "$TestDir*") (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath')
Check 'ClientPath' ((RegValue 'HKCU:\Software\ExplorerRemoteFs' 'ClientPath') -like "$TestDir*") (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'ClientPath')
Check 'Run entry (startup task)' ((RegValue $run 'ExplorerRemoteFs') -like "*RemoteFsClient.exe*") (RegValue $run 'ExplorerRemoteFs')
Check 'resident client started' ($null -ne (Get-Process -Name RemoteFsClient -ErrorAction SilentlyContinue)) 'RemoteFsClient.exe'
Check 'no desktop shortcut without the task' ($null -eq (DesktopShortcut)) 'only created when the task is checked'

foreach ($c in @($folder, $ctx, $props, $erfDelegate)) {
    $v = RegDefault "$hk\CLSID\$c\InprocServer32"
    Check ("InprocServer32 " + $c.Substring(0, 9)) ($v -like "$TestDir*") $v
}
Check 'DefaultIcon uses our dll,-101' ((RegDefault "$hk\CLSID\$folder\DefaultIcon") -like "*,-101") (RegDefault "$hk\CLSID\$folder\DefaultIcon")
# 注意：注册表 DWORD 读出来是 Int32，0xA8000020 会变成负数 —— 必须按无符号比较
# （第一版就是这里假报 FAIL，值其实是对的）
# 而且不能写 [uint32]$attr：PS 5.1 拒绝把负的 Int32 转成 UInt32（直接抛异常，
# 于是这条断言整条被跳过、连 FAIL 都不记）。右边也别写 0xA8000020 —— PS 5.1 把
# 这个十六进制字面量解析成 Int32 -1476394976，比较永远不相等。写十进制最稳。
$attr = RegValue "$hk\CLSID\$folder\ShellFolder" 'Attributes'
$attrU = if ($null -ne $attr) { [int64]$attr -band 0xFFFFFFFF } else { -1 }
Check 'ShellFolder Attributes' ($attrU -eq 2818572320) ("0x" + ('{0:X}' -f $attrU))
Check 'pinned to nav pane' ((RegValue "$hk\CLSID\$folder" 'System.IsPinnedToNameSpaceTree') -eq 1) 'System.IsPinnedToNameSpaceTree'
Check 'namespace entry name' ((RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder") -eq '易远传') (RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder")
Check 'desktop icon hidden (our value only)' ((RegValue "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" $folder) -eq 1) 'HideDesktopIcons'
Check 'ctx handler registered (dir)' ($null -ne (RegDefault "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx")) 'ContextMenuHandlers'
Check 'props handler registered (dir)' ($null -ne (RegDefault "$hk\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\$props")) 'PropertySheetHandlers'
Check 'ctx handler registered (file)' ($null -ne (RegDefault "$hk\RemoteFsFileType\shellex\ContextMenuHandlers\$ctx")) 'RemoteFsFileType'
Check 'file open verb' ((RegDefault "$hk\RemoteFsFileType\shell\open\command") -like "*ExplorerRemoteFs.Cli.exe*open*") (RegDefault "$hk\RemoteFsFileType\shell\open\command")
Check 'erf:// owner marker' ((RegValue "$hk\erf" 'ERF.HandlerOwner') -eq 'ExplorerRemoteFs') (RegValue "$hk\erf" 'ERF.HandlerOwner')
Check 'erf:// handler command' ((RegDefault "$hk\erf\shell\open\command") -like "*--open-erf*") (RegDefault "$hk\erf\shell\open\command")
Check 'erf:// DelegateExecute current-tab handler' ((RegValue "$hk\erf\shell\open\command" 'DelegateExecute') -eq $erfDelegate) (RegValue "$hk\erf\shell\open\command" 'DelegateExecute')
Check 'Winlogon AutoRestartShell not written' ($null -eq (RegValue $winlogon 'AutoRestartShell')) (RegValue $winlogon 'AutoRestartShell')
# shell 真的认这个条目吗（会实例化我们的 in-proc 扩展）
Check 'shell sees the namespace entry' (NamespaceVisible $folder) 'enumerated Desktop namespace via Shell.Application'

# ---- phase 1b: silent upgrade over the existing install ----
# 目标目录里已经有 DLL —— 这正是旧版本会 "taskkill /T explorer" 的那条路径。
# 新版：先把旧 DLL 挪开（删除，删不掉就改名 .old）再写新的，全程不碰 explorer。
$innoLog2 = Join-Path $PSScriptRoot 'inno-setup-upgrade.log'
Remove-Item $innoLog2 -Force -ErrorAction SilentlyContinue
$shellBefore1b = ShellPid
$code1b = RunExe $Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$TestDir",
                          '/TASKS=startup,desktopicon', "/LOG=$innoLog2") 'upgrade'
Check 'upgrade exit code 0' ($code1b -eq 0) ("exit=" + $code1b)
Check 'dll still there after upgrade' (Test-Path "$TestDir\ExplorerDataProviderFtp.dll") "$TestDir\ExplorerDataProviderFtp.dll"
Check 'dll is the new one after upgrade' ((Sha256 "$TestDir\ExplorerDataProviderFtp.dll") -eq (Sha256 $payloadDll)) 'sha256 compare'
Check 'client running after upgrade' ($null -ne (Get-Process -Name RemoteFsClient -ErrorAction SilentlyContinue)) 'RemoteFsClient.exe'
Check 'desktop shortcut created when task checked' ($null -ne (DesktopShortcut)) (DesktopShortcut).FullName
Check 'shell survived the upgrade' ((ShellPid) -eq $shellBefore1b) ("pid now " + (ShellPid))
Check 'Winlogon AutoRestartShell still not written' ($null -eq (RegValue $winlogon 'AutoRestartShell')) (RegValue $winlogon 'AutoRestartShell')

# ---- phase 1c: upgrade launched through explorer.exe (the double-click path) ----
# 只有这条路径能复现用户的故障：安装程序成为 explorer.exe 的子进程。
$innoLog3 = Join-Path $PSScriptRoot 'inno-setup-dblclick.log'
Remove-Item $innoLog3 -Force -ErrorAction SilentlyContinue
$lnk = Join-Path $env:TEMP 'erf-dblclick-test.lnk'
Remove-Item $lnk -Force -ErrorAction SilentlyContinue
$ws = New-Object -ComObject WScript.Shell
$sc = $ws.CreateShortcut($lnk)
$sc.TargetPath = (Resolve-Path $Setup).Path
$sc.Arguments = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="' + $TestDir + '" /TASKS=startup /LOG="' + $innoLog3 + '"'
$sc.Save()
$shellBefore1c = ShellPid
Log ("   launching setup through explorer.exe (double-click path), shell pid = " + $shellBefore1c)
Start-Process explorer.exe -ArgumentList $lnk
$gone = Wait-SetupGone 240
Check 'double-click install finished (setup process exited)' $gone 'setup processes gone'
Check 'shell survived the double-click install' ((ShellPid) -eq $shellBefore1c) ("pid now " + (ShellPid))
Check 'registry written by the double-click install' ((RegDefault "$hk\CLSID\$folder\InprocServer32") -like "$TestDir*") (RegDefault "$hk\CLSID\$folder\InprocServer32")
Check 'nav-pane entry present after double-click install' ((RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder") -eq '易远传') 'Desktop\NameSpace'
Check 'dll updated by the double-click install' ((Sha256 "$TestDir\ExplorerDataProviderFtp.dll") -eq (Sha256 $payloadDll)) 'sha256 compare'
Check 'AutoRestartShell untouched by the double-click install' ($null -eq (RegValue $winlogon 'AutoRestartShell')) (RegValue $winlogon 'AutoRestartShell')
Remove-Item $lnk -Force -ErrorAction SilentlyContinue

# ---- phase 2: silent uninstall ----
# 先把扩展 DLL 映射住，模拟"用户装完就打开过易远传、资源管理器正加载着扩展"这个常见状态：
# 这时 DLL 删不掉，只能改名成 .old 并交给下次登录的 RunOnce 清理 —— 这条路径必须被覆盖，
# 否则一旦它坏了（比如 RunOnce 命令写错），只有真实用户会发现。
# 辅助进程写成独立脚本再 -File 启动：把这段 C# 声明塞进 -Command 字符串会被
# Start-Process 的参数拼接毁掉引号（第一版就是这么失败的：DLL 根本没被映射住，
# 于是"改名 + RunOnce"这条路径一次都没被覆盖，断言还静默地跳过去了）。
$lockerScript = Join-Path $env:TEMP 'erf-dll-locker.ps1'
Set-Content -LiteralPath $lockerScript -Encoding UTF8 -Value @'
param([string]$Dll)
Add-Type -Namespace L -Name N -MemberDefinition '[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibrary(string p);'
$h = [L.N]::LoadLibrary($Dll)
if ($h -eq [IntPtr]::Zero) { exit 1 }
Start-Sleep -Seconds 300
'@
$locker = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $lockerScript, '-Dll', "$TestDir\ExplorerDataProviderFtp.dll")
Start-Sleep -Seconds 3
$locked = ((tasklist /m ExplorerDataProviderFtp.dll 2>&1 | Out-String) -match 'ExplorerDataProviderFtp')
Log ("   extension dll mapped by a helper process? " + $locked)
$shellBefore2 = ShellPid
$code2 = RunExe (Join-Path $TestDir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 'uninstall'
Check 'uninstaller exit code 0' ($code2 -eq 0) ("exit=" + $code2)
for ($i = 0; $i -lt 40 -and (Test-Path $TestDir); $i++) { Start-Sleep -Milliseconds 500 }
Check 'shell survived the uninstall' ((ShellPid) -eq $shellBefore2) ("pid now " + (ShellPid))
# 扩展 DLL 若当时被某个进程映射着就删不掉，只能改名成 .old 留给下次登录的 RunOnce 清理 ——
# 所以"目录已删"不是硬要求。真正要断言的是：**应用文件**都没了
# （unins000.exe/.dat 是卸载器自己的文件，它的自删除是 Inno 的事，偶发晚一拍不算我们的问题）。
$leftover = @(Get-ChildItem $TestDir -Recurse -File -ErrorAction SilentlyContinue)
$oldFiles = @($leftover | Where-Object { $_.Name -like '*.old*' })
$appLeftover = @($leftover | Where-Object { $_.Name -notlike '*.old*' -and $_.Name -notlike 'unins000.*' })
Check 'app files removed by uninstall' ($appLeftover.Count -eq 0) (($leftover | ForEach-Object { $_.Name }) -join ', ')
if ($appLeftover.Count -gt 0) {
    $appLeftover | Select-Object -First 5 | ForEach-Object { Log ("   leftover: " + $_.FullName) }
}
$cleanupCmd = RegValue $runOnce 'ErfCleanup'
if ($locked) {
    Check 'mapped dll was renamed to .old (cannot be deleted)' ($oldFiles.Count -ge 1) (($leftover | ForEach-Object { $_.Name }) -join ', ')
}
if ($oldFiles.Count -gt 0) {
    # RunOnce 的值是整条命令行 —— 必须自己带上 cmd.exe，光写 "/c del ..." 会被当成程序名
    Check 'RunOnce cleanup command is a full command line' ($cleanupCmd -like '*cmd.exe*') $cleanupCmd
    Check 'RunOnce cleanup points at the leftover .old' ($cleanupCmd -like "*$($oldFiles[0].Name)*") $cleanupCmd
} else {
    Log ("   (nothing left behind, RunOnce = " + $cleanupCmd + ")")
}
Check 'ARP removed' ($null -eq (RegValue $arp 'DisplayName')) 'ARP'
Check 'Run entry removed' ($null -eq (RegValue $run 'ExplorerRemoteFs')) 'Run'
Check 'CliPath removed' ($null -eq (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath')) 'CliPath'
foreach ($c in @($folder, $ctx, $props, $erfDelegate)) {
    Check ("CLSID removed " + $c.Substring(0, 9)) ($null -eq (RegDefault "$hk\CLSID\$c\InprocServer32")) $c
}
Check 'erf:// removed (owner was ours)' ($null -eq (RegDefault "$hk\erf\shell\open\command")) 'erf'
Check 'namespace key removed' ($null -eq (RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder")) 'Desktop\NameSpace'
Check 'ctx handler removed' ($null -eq (RegDefault "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx")) 'RemoteFsMicrosoftCoreType'
Check 'desktop shortcut removed on uninstall' ($null -eq (DesktopShortcut)) 'desktop .lnk'
Check 'site config kept' (Test-Path "$env:APPDATA\ExplorerRemoteFs") "$env:APPDATA\ExplorerRemoteFs"
Check 'Winlogon value not left behind' ($null -eq (RegValue $winlogon 'AutoRestartShell')) (RegValue $winlogon 'AutoRestartShell')

# 测试自己的收尾：别把"下次登录删测试目录"这种命令留在用户机器上
Remove-ItemProperty -Path $runOnce -Name 'ErfCleanup' -ErrorAction SilentlyContinue
# 放掉 phase 2 里那个映射住 DLL 的辅助进程，并清掉它留下的 .old
Stop-Process -Id $locker.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $lockerScript -Force -ErrorAction SilentlyContinue
# `explorer.exe <lnk>` 会留下一个转发用的 explorer.exe 进程（不是 shell），顺手收掉
Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*erf-dblclick-test.lnk*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Log ("=== " + $(if ($fails -eq 0) { 'ALL PASS' } else { "$fails FAILED" }))
exit $fails
