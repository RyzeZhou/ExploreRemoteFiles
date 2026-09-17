# Inno installer self-check: silent install to a throwaway directory, assert everything,
# silent uninstall, assert the machine is clean again.
#
#   powershell -NoProfile -Command "Start-Process pwsh -ArgumentList '-NoProfile','-File','src-setup\inno-test.ps1' -WindowStyle Hidden"
#
# Must run detached: the installer kills explorer.exe on purpose (the shell extension DLL stays
# mapped otherwise), which can take the calling console down with it. Watch inno-test.log.
#
# Why a UI-less test is enough here: Inno's wizard is Inno's own code -- what *we* have to prove
# is the parts we wrote ([Registry] entries, [Code] steps). The wizard pages themselves are
# checked by eye once (see the -WizardShots switch).
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

$folder = '{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx = '{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$props = '{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'
$arp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs_is1'
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$hk = 'HKCU:\Software\Classes'

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

# ---- phase 1: silent install ----
if (Test-Path $TestDir) { Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue }
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
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

Check 'ARP DisplayName' ($null -ne (RegValue $arp 'DisplayName')) (RegValue $arp 'DisplayName')
Check 'ARP DisplayVersion' ((RegValue $arp 'DisplayVersion') -eq '0.1-Alpha') (RegValue $arp 'DisplayVersion')
Check 'ARP InstallLocation' ((RegValue $arp 'InstallLocation') -like "$TestDir*") (RegValue $arp 'InstallLocation')
Check 'ARP UninstallString' ((RegValue $arp 'UninstallString') -like "*unins000.exe*") (RegValue $arp 'UninstallString')
Check 'ARP EstimatedSize' ($null -ne (RegValue $arp 'EstimatedSize')) (RegValue $arp 'EstimatedSize')

Check 'CliPath' ((RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath') -like "$TestDir*") (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath')
Check 'ClientPath' ((RegValue 'HKCU:\Software\ExplorerRemoteFs' 'ClientPath') -like "$TestDir*") (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'ClientPath')
Check 'Run entry (startup task)' ((RegValue $run 'ExplorerRemoteFs') -like "*RemoteFsClient.exe*") (RegValue $run 'ExplorerRemoteFs')
Check 'resident client started' ($null -ne (Get-Process -Name RemoteFsClient -ErrorAction SilentlyContinue)) 'RemoteFsClient.exe'

foreach ($c in @($folder, $ctx, $props)) {
    $v = RegDefault "$hk\CLSID\$c\InprocServer32"
    Check ("InprocServer32 " + $c.Substring(0, 9)) ($v -like "$TestDir*") $v
}
Check 'DefaultIcon uses our dll,-101' ((RegDefault "$hk\CLSID\$folder\DefaultIcon") -like "*,-101") (RegDefault "$hk\CLSID\$folder\DefaultIcon")
# 注意：注册表 DWORD 读出来是 Int32，0xA8000020 会变成负数 —— 必须按无符号比较
# （第一版就是这里假报 FAIL，值其实是对的）
$attr = RegValue "$hk\CLSID\$folder\ShellFolder" 'Attributes'
Check 'ShellFolder Attributes' (($null -ne $attr) -and ([uint32]$attr -eq [uint32]0xA8000020)) ("0x" + ('{0:X}' -f [uint32]$attr))
Check 'pinned to nav pane' ((RegValue "$hk\CLSID\$folder" 'System.IsPinnedToNameSpaceTree') -eq 1) 'System.IsPinnedToNameSpaceTree'
Check 'namespace entry name' ((RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder") -eq '易远传') (RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder")
Check 'desktop icon hidden (our value only)' ((RegValue "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" $folder) -eq 1) 'HideDesktopIcons'
Check 'ctx handler registered (dir)' ($null -ne (RegDefault "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx")) 'ContextMenuHandlers'
Check 'props handler registered (dir)' ($null -ne (RegDefault "$hk\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\$props")) 'PropertySheetHandlers'
Check 'ctx handler registered (file)' ($null -ne (RegDefault "$hk\RemoteFsFileType\shellex\ContextMenuHandlers\$ctx")) 'RemoteFsFileType'
Check 'file open verb' ((RegDefault "$hk\RemoteFsFileType\shell\open\command") -like "*ExplorerRemoteFs.Cli.exe*open*") (RegDefault "$hk\RemoteFsFileType\shell\open\command")
Check 'erf:// owner marker' ((RegValue "$hk\erf" 'ERF.HandlerOwner') -eq 'ExplorerRemoteFs') (RegValue "$hk\erf" 'ERF.HandlerOwner')
Check 'erf:// handler command' ((RegDefault "$hk\erf\shell\open\command") -like "*--open-erf*") (RegDefault "$hk\erf\shell\open\command")
Check 'Winlogon AutoRestartShell restored' ($null -eq (RegValue 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon' 'AutoRestartShell') -or (RegValue 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon' 'AutoRestartShell') -eq 1) (RegValue 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon' 'AutoRestartShell')

# ---- phase 2: silent uninstall ----
$code2 = RunExe (Join-Path $TestDir 'unins000.exe') @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') 'uninstall'
Check 'uninstaller exit code 0' ($code2 -eq 0) ("exit=" + $code2)
for ($i = 0; $i -lt 40 -and (Test-Path $TestDir); $i++) { Start-Sleep -Milliseconds 500 }
Check 'install dir removed' (-not (Test-Path $TestDir)) $TestDir
if (Test-Path $TestDir) {
    Get-ChildItem $TestDir -Recurse -File -ErrorAction SilentlyContinue |
        Select-Object -First 5 | ForEach-Object { Log ("   leftover: " + $_.FullName) }
}
Check 'ARP removed' ($null -eq (RegValue $arp 'DisplayName')) 'ARP'
Check 'Run entry removed' ($null -eq (RegValue $run 'ExplorerRemoteFs')) 'Run'
Check 'CliPath removed' ($null -eq (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath')) 'CliPath'
foreach ($c in @($folder, $ctx, $props)) {
    Check ("CLSID removed " + $c.Substring(0, 9)) ($null -eq (RegDefault "$hk\CLSID\$c\InprocServer32")) $c
}
Check 'erf:// removed (owner was ours)' ($null -eq (RegDefault "$hk\erf\shell\open\command")) 'erf'
Check 'namespace key removed' ($null -eq (RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder")) 'Desktop\NameSpace'
Check 'ctx handler removed' ($null -eq (RegDefault "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx")) 'RemoteFsMicrosoftCoreType'
Check 'site config kept' (Test-Path "$env:APPDATA\ExplorerRemoteFs") "$env:APPDATA\ExplorerRemoteFs"
Check 'Winlogon value not left behind' ($null -eq (RegValue 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon' 'AutoRestartShell') -or (RegValue 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon' 'AutoRestartShell') -eq 1) (RegValue 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon' 'AutoRestartShell')

Log ("=== " + $(if ($fails -eq 0) { 'ALL PASS' } else { "$fails FAILED" }))
exit $fails
