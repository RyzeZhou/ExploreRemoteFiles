# 用 Inno Setup 编译安装包：src-setup\erf.iss -> dist\Erf-<版本>-Setup.exe
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src-setup\build-inno.ps1
#
# 会把随包 DLL 的 SHA256 传给安装脚本（/DExpectedDllSha256），装完由 [Code] 自校验
# "扩展 DLL 到底换没换" —— 实测踩过：explorer 占用着 DLL 时升级会"成功但不替换"，
# 于是旧 DLL 配新 CLI，取文件失败被笼统报成"执行读取操作时发生磁盘错误"。
# ASCII only（PowerShell 5.1 对无 BOM 文件按 ANSI 解，中文注释会吃掉代码行）。
param(
    [string]$PayloadDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\ExplorerRemoteFs-win-x64'),
    [string]$OutputDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'),
    [string]$Iscc = '',
    [string]$DllSha256 = ''
)
$ErrorActionPreference = 'Stop'

function Get-Sha256([string]$path) {
    # 不用 Get-FileHash：受限/精简会话里它可能不存在（实测在本机 PS 5.1 下报 CommandNotFound）
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $fs = [IO.File]::OpenRead($path)
        try { return (($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('x2') }) -join '') }
        finally { $fs.Close(); $sha.Dispose() }
    } catch { return '' }
}

function Find-Iscc {
    $hit = @()
    foreach ($root in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
                        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
                        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
        $hit += Get-ItemProperty $root -ErrorAction SilentlyContinue |
                Where-Object { $_.DisplayName -like 'Inno Setup*' -and $_.InstallLocation }
    }
    foreach ($h in $hit) {
        $exe = Join-Path $h.InstallLocation 'ISCC.exe'
        if (Test-Path $exe) { return $exe }
    }
    foreach ($guess in @('D:\Program\Inno Setup 7\ISCC.exe',
                         'C:\Program Files (x86)\Inno Setup 7\ISCC.exe',
                         'C:\Program Files\Inno Setup 7\ISCC.exe',
                         'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
                         'C:\Program Files\Inno Setup 6\ISCC.exe')) {
        if (Test-Path $guess) { return $guess }
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return ''
}

if (-not $Iscc) { $Iscc = Find-Iscc }
if (-not $Iscc) { throw '找不到 ISCC.exe（Inno Setup 的命令行编译器）。装了 Inno Setup 后再跑，或用 -Iscc 指定路径。' }

$dll = Join-Path $PayloadDir 'ExplorerDataProviderFtp.dll'
if (-not (Test-Path $dll)) { throw "随包目录里没有 ExplorerDataProviderFtp.dll：$dll —— 先跑 scripts\build-release.ps1" }
if (-not $DllSha256) { $DllSha256 = Get-Sha256 $dll }
if (-not $DllSha256) { throw "算不出 $dll 的 SHA256" }
Write-Output ("payload DLL sha256: " + $DllSha256)

$iss = Join-Path $PSScriptRoot 'erf.iss'
$sw = [Diagnostics.Stopwatch]::StartNew()
# 注意：Inno 7 的 ISCC 不认 /Q、/Qp（会报 "more than one script filename"），不要加静默开关
& $Iscc "/DPayloadDir=$PayloadDir" "/DOutputDir=$OutputDir" "/DExpectedDllSha256=$DllSha256" $iss
$code = $LASTEXITCODE
$sw.Stop()
if ($code -ne 0) { throw "Inno 编译失败（ISCC 退出码 $code）" }

$out = Join-Path $OutputDir 'Erf-0.1-Alpha-Setup.exe'
$size = if (Test-Path $out) { [math]::Round((Get-Item $out).Length / 1MB, 1) } else { 0 }
Write-Output ("Inno build OK in " + $sw.Elapsed.TotalSeconds.ToString('0.0') + " s")
Write-Output ("  $out  ($size MB)")
