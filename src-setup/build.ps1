param([string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\ExplorerRemoteFs-win-x64'))
$ErrorActionPreference = 'Stop'

# 编译安装器（原生，无 .NET 依赖）：一个 exe，按文件名区分 Setup / Uninstall。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
$msvc = Get-ChildItem -LiteralPath (Join-Path $vsInstall 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = (Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
           Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1).Name

$env:PATH    = "$($msvc.FullName)\bin\Hostx64\x64;$sdkRoot\bin\$sdkVer\x64;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)\include;$sdkRoot\Include\$sdkVer\shared;$sdkRoot\Include\$sdkVer\ucrt;$sdkRoot\Include\$sdkVer\um"
$env:LIB     = "$($msvc.FullName)\lib\x64;$sdkRoot\Lib\$sdkVer\ucrt\x64;$sdkRoot\Lib\$sdkVer\um\x64"

Set-Location $PSScriptRoot
# /we4129：把「未知字符转义序列」（C4129）升级为错误。
# 理由（实测 2026-09-17）：`L"%s\Uninstall.exe"` 里的 `\U` 被 MSVC 当成无效的通用字符名，
# **静默吃掉反斜杠** → 目标路径变成 "...\ExplorerRemoteFs-TestUninstall.exe"，复制"成功"
# 但文件没进安装目录，ARP 的 UninstallString 指向不存在的文件（点卸载没反应）。
# 这类 bug 编译不报错、运行时只在 silent 模式下打一行警告，只能靠编译期拦住。
& cl.exe /nologo /EHsc /std:c++17 /W3 /we4129 /utf-8 /DUNICODE /D_UNICODE ErfSetup.cpp `
    /Fe:erf-setup.exe /link shlwapi.lib shell32.lib advapi32.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw '安装器编译失败' }

if (-not (Test-Path $OutDir)) { throw "找不到输出目录 $OutDir —— 先跑 scripts\build-release.ps1" }
Copy-Item (Join-Path $PSScriptRoot 'erf-setup.exe') (Join-Path $OutDir 'Setup.exe') -Force
Copy-Item (Join-Path $PSScriptRoot 'erf-setup.exe') (Join-Path $OutDir 'Uninstall.exe') -Force
Write-Output "安装器已就位："
Write-Output "  $(Join-Path $OutDir 'Setup.exe')"
Write-Output "  $(Join-Path $OutDir 'Uninstall.exe')"
