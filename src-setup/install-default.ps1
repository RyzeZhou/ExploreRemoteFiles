# Install (or upgrade in place) to the default directory with the Inno Setup package.
# Used to refresh this machine after a rebuild -- and it is also how the old
# homemade installer's leftovers get cleaned up first, if any are still registered.
#
#   powershell -NoProfile -Command "Start-Process pwsh -ArgumentList '-NoProfile','-File','src-setup\install-default.ps1' -WindowStyle Hidden"
#
# Run it detached: the installer kills explorer.exe on purpose (the shell extension DLL stays
# mapped otherwise), which can take the calling console down with it.  ASCII only.
param(
    [string]$Setup = (Join-Path $PSScriptRoot '..\dist\Erf-0.1-Alpha-Setup.exe'),
    [string]$LogFile = (Join-Path $PSScriptRoot 'install-default.log')
)
$ErrorActionPreference = 'Continue'
Set-Content -LiteralPath $LogFile -Value ("### install-default " + (Get-Date -Format 'HH:mm:ss')) -Encoding UTF8
function Log([string]$t) { Add-Content -LiteralPath $LogFile -Value $t -Encoding UTF8 }
function Wait-Proc($p) {
    $deadline = (Get-Date).AddMinutes(10)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not $p.HasExited) { $p.Kill(); return 'TIMEOUT' }
    return $p.ExitCode
}

# 1) 如果旧的自制安装器还注册着（它的卸载键叫 ExplorerRemoteFs，Inno 版叫 ExplorerRemoteFs_is1），
#    先老老实实卸掉，免得两份注册互相打架。
$legacy = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs'
if (Get-ItemProperty -Path $legacy -Name DisplayName -ErrorAction SilentlyContinue) {
    $old = Join-Path $env:LOCALAPPDATA 'ExplorerRemoteFs\Uninstall.exe'
    Log "found legacy (homemade installer) registration; uninstalling it first"
    if (Test-Path $old) {
        Log ("  legacy uninstall exit=" + (Wait-Proc (Start-Process -FilePath $old -ArgumentList '--silent' -PassThru)))
        # 自制卸载器是"自杀式"清理：它先把自己复制到 %TEMP%，等自己退出后再删安装目录。
        # 不等它删完就开始装新版 -> 新装的文件会被它顺手删掉（实测 2026-09-17：
        # 装完只剩一个被占用的 RemoteFsClient.exe，DLL/cli 全没了）。
        for ($i = 0; $i -lt 40 -and (Test-Path (Join-Path $env:LOCALAPPDATA 'ExplorerRemoteFs')); $i++) {
            Start-Sleep -Milliseconds 500
        }
        Log ("  legacy dir cleaned: " + (-not (Test-Path (Join-Path $env:LOCALAPPDATA 'ExplorerRemoteFs'))))
    } else {
        Log ("  legacy Uninstall.exe missing ($old); removing its keys manually")
        Remove-Item $legacy -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item 'HKCU:\Software\ExplorerRemoteFs' -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# 2) 用 Inno 安装包装到默认目录
if (-not (Test-Path $Setup)) { Log ("setup not found: " + $Setup); exit 1 }
$innoLog = Join-Path $PSScriptRoot 'inno-install-default.log'
Log ("installing: " + $Setup)
Log ("  exit=" + (Wait-Proc (Start-Process -FilePath $Setup -PassThru -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS=startup,desktopicon', "/LOG=$innoLog"))))

$dir = Join-Path $env:LOCALAPPDATA 'ExplorerRemoteFs'
foreach ($item in @('ExplorerDataProviderFtp.dll', 'unins000.exe', 'cli\ExplorerRemoteFs.Cli.exe', 'client\RemoteFsClient.exe')) {
    Log ("  {0,-40} {1}" -f $item, (Test-Path (Join-Path $dir $item)))
}
$arp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs_is1'
Log ("ARP:      " + (Get-ItemProperty $arp -Name DisplayName -ErrorAction SilentlyContinue).DisplayName + " " +
                    (Get-ItemProperty $arp -Name DisplayVersion -ErrorAction SilentlyContinue).DisplayVersion)
Log ("CliPath:  " + (Get-ItemProperty 'HKCU:\Software\ExplorerRemoteFs' -Name CliPath -ErrorAction SilentlyContinue).CliPath)
Log ("namespace:" + (Get-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\{C816CE0E-728C-4FC9-98E5-D0B35B384597}' -ErrorAction SilentlyContinue).GetValue(''))
Log ("client running: " + ($null -ne (Get-Process -Name RemoteFsClient -ErrorAction SilentlyContinue)))
