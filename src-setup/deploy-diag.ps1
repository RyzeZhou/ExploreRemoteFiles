# 部署"带下载/读取诊断日志"的构建，并收集日志。
#
#   powershell -ExecutionPolicy Bypass -File src-setup\deploy-diag.ps1            # 静默升级到默认目录
#   powershell -ExecutionPolicy Bypass -File src-setup\deploy-diag.ps1 -Collect   # 只收集日志
#
# 为什么必须显式传 /DIR：Inno 默认 UsePreviousAppDir=yes，会沿用"上一次装到哪"。
# 如果之前有人装到过别的目录（例如测试目录），不传 /DIR 就会跟着漂过去，而你以为升级了默认目录
# —— 实测 2026-09-17 就踩了这个坑（ARP 指向 ExplorerRemoteFs-DiagCheck，默认目录还是旧 DLL）。
# ASCII only。安装会终止 explorer 几秒（替换扩展 DLL），建议从终端里跑。
param(
    [string]$Setup = (Join-Path $PSScriptRoot '..\dist\Erf-0.1-Alpha-Setup.exe'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\dist\diag'),
    [string]$TargetDir = "$env:LOCALAPPDATA\ExplorerRemoteFs",
    [switch]$Collect
)
$ErrorActionPreference = 'Continue'
$log = Join-Path $env:LOCALAPPDATA 'ExplorerRemoteFs\logs\remotefs-debug.log'

function Test-DiagDll([string]$dir) {
    $dll = Join-Path $dir 'ExplorerDataProviderFtp.dll'
    if (-not (Test-Path $dll)) { return "missing: $dll" }
    $bytes = [IO.File]::ReadAllBytes($dll)
    $text = [Text.Encoding]::Unicode.GetString($bytes)
    if ($text.IndexOf('[DL] Ensure ok local=') -ge 0) { return 'OK (diagnostic logging present)' }
    return 'OLD DLL (no [DL] logging) -> the upgrade did not land'
}

if (-not $Collect) {
    Write-Host "==> 安装诊断构建到 $TargetDir"
    Write-Host "    $Setup"
    $p = Start-Process $Setup -PassThru -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
        "/DIR=$TargetDir", '/TASKS=startup,desktopicon')
    while (-not $p.HasExited) { Start-Sleep -Milliseconds 300 }
    Write-Host ("    安装器退出码 = " + $p.ExitCode)

    $arp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs_is1'
    Write-Host ("    ARP InstallLocation = " + (Get-ItemProperty $arp -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation)
    $check = Test-DiagDll $TargetDir
    Write-Host ("    诊断日志自检: " + $check)
    if ($check -notlike 'OK*') { Write-Warning "诊断 DLL 没装上，先别急着复现，把上面的输出发回来" ; exit 1 }

0：在资源管理器里把远程文件『复制』或『拖拽』到本地，直到报错"
    Write-Host "    然后跑：  powershell -ExecutionPolicy Bypass -File src-setup\deploy-diag.ps1 -Collect"
    exit 0
}

if (-not (Test-Path $log)) { Write-Host "找不到日志：$log"; exit 1 }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir ("remotefs-debug-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
Select-String -Path $log -Pattern '\[DL\]|\[XFER\]|\[DATAOBJ\]|\[NAV\]|\[SAMPLE\]' |
    Select-Object -Last 400 | ForEach-Object { $_.Line } | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host ("已收集最近 400 条相关日志 -> " + $out)
Write-Host ("完整日志（也一并附上更好）: " + $log)
Write-Host "关键看这几行："
Write-Host "  [DL] Ensure start / Ensure ok / Ensure FAILED ...  下载这一步成没成、临时文件在不在、大小对不对"
Write-Host "  [DL] Read FAILED at Ensure ...                     走到读取时文件没准备好"
Write-Host "  [XFER] CreateViewObject ITransferSource ...        引擎建了几次传输源（Win11 会反复建）"
