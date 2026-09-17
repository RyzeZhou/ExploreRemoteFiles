# 用 Inno Setup 编译安装包：src-setup\erf.iss -> dist\Erf-<版本>-Setup.exe
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File src-setup\build-inno.ps1
#   （产物是单文件、自包含的 exe，双击就是经典向导；静默安装：/VERYSILENT /DIR=... /LOG=...）
#
# ASCII only（PowerShell 5.1 对无 BOM 文件按 ANSI 解，中文注释会吃掉代码行）。
param(
    [string]$PayloadDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\ExplorerRemoteFs-win-x64'),
    [string]$OutputDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'),
    [string]$Iscc = ''
)
$ErrorActionPreference = 'Stop'

function Find-Iscc {
    # 1) 注册表里的安装位置（Inno 卸载项）  2) 常见路径  3) PATH
    foreach ($root in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
                        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
                        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
        $hit = Get-ItemProperty $root -ErrorAction SilentlyContinue |
               Where-Object { $_.DisplayName -like 'Inno Setup*' } | Select-Object -First 1
        if ($hit -and $hit.InstallLocation) {
            $exe = Join-Path $hit.InstallLocation 'ISCC.exe'
            if (Test-Path $exe) { return $exe }
        }
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
if (-not (Test-Path (Join-Path $PayloadDir 'ExplorerDataProviderFtp.dll'))) {
    throw "随包目录里没有 ExplorerDataProviderFtp.dll：$PayloadDir —— 先跑 scripts\build-release.ps1"
}

$iss = Join-Path $PSScriptRoot 'erf.iss'
$sw = [Diagnostics.Stopwatch]::StartNew()
# 注意：Inno 7 的 ISCC 不认 /Q、/Qp（会报 "more than one script filename"），不要加静默开关
& $Iscc "/DPayloadDir=$PayloadDir" "/DOutputDir=$OutputDir" $iss
$code = $LASTEXITCODE
$sw.Stop()
if ($code -ne 0) { throw "Inno 编译失败（ISCC 退出码 $code）" }

$out = Join-Path $OutputDir 'Erf-0.1-Alpha-Setup.exe'
$size = if (Test-Path $out) { [math]::Round((Get-Item $out).Length / 1MB, 1) } else { 0 }
Write-Output ("Inno 编译完成：" + $sw.Elapsed.TotalSeconds.ToString('0.0') + " 秒")
Write-Output ("  $out  ($size MB)")
