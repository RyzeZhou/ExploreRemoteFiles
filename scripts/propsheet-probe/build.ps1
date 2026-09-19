# 编译 PropTrigger.exe（属性页取证工具）。
# 与 src-cpp/ExplorerDataProviderFtp/compile.ps1 用同一套 vswhere 定位 MSVC/Windows SDK
# 的方式 —— 不套 vcvars64.bat（那种 Set-Item Env: 的写法在沙箱化的宿主里会静默失败）。
param([string]$OutputPath = (Join-Path $PSScriptRoot 'PropTrigger.exe'))

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if ($vsInstall) { $vsInstall = $vsInstall.Trim() }
}
if (-not $vsInstall) { throw 'Visual C++ x64 build tools were not found.' }

$msvc = Get-ChildItem -LiteralPath (Join-Path $vsInstall 'VC\Tools\MSVC') -Directory |
        Sort-Object Name -Descending | Select-Object -First 1
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
          Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1
$ver = $sdkVer.Name

$msvcBin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$env:PATH    = "$msvcBin;$sdkRoot\bin\$ver\x64;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)\include;$sdkRoot\Include\$ver\shared;$sdkRoot\Include\$ver\ucrt;$sdkRoot\Include\$ver\um"
$env:LIB     = "$($msvc.FullName)\lib\x64;$sdkRoot\Lib\$ver\ucrt\x64;$sdkRoot\Lib\$ver\um\x64"

Set-Location $PSScriptRoot
# 参数用数组展开：反引号续行在这里会把源文件名传丢（cl 会报 "no object files specified"）。
$clArgs = @(
    '/nologo', '/EHsc', '/MD', '/std:c++17', '/W3', '/utf-8', '/DUNICODE', '/D_UNICODE',
    "/Fe:$OutputPath", 'proptrigger.cpp',
    'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'comctl32.lib', 'shlwapi.lib',
    'user32.lib', 'advapi32.lib', 'uuid.lib'
)
& cl.exe @clArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $OutputPath)) { throw 'PropTrigger build failed.' }
Write-Output ("built -> " + $OutputPath + " (" + (Get-Item $OutputPath).Length + " bytes)")
