param([string]$OutputPath = (Join-Path $PSScriptRoot 'ExplorerDataProviderFtp.dll'))

$ErrorActionPreference = 'Stop'

# Locate MSVC + Windows SDK via vswhere (any VS edition / future upgrades),
# then set PATH/INCLUDE/LIB by direct assignment. NOTE: do NOT apply the
# vcvars64.bat environment via a Set-Item Env: loop — in sandboxed/hybrid
# PowerShell hosts those writes silently fail and cl.exe is never found.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if ($vsInstall) { $vsInstall = $vsInstall.Trim() }
}
if (-not $vsInstall) {
    throw 'Visual C++ x64 build tools were not found. Install the VS Desktop C++ workload to build the shell extension.'
}

$msvcRoot = Join-Path $vsInstall 'VC\Tools\MSVC'
if (-not (Test-Path -LiteralPath $msvcRoot)) { throw "MSVC tools not found: $msvcRoot" }
$msvc = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset version under $msvcRoot" }

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkInc = Join-Path $sdkRoot 'Include'
$sdkVer = $null
if (Test-Path -LiteralPath $sdkInc) {
    $sdkVer = Get-ChildItem -LiteralPath $sdkInc -Directory | Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1
}
if (-not $sdkVer) { throw "Windows 10 SDK not found under $sdkInc" }
$ver = $sdkVer.Name

$msvcBin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
if (-not (Test-Path -LiteralPath (Join-Path $msvcBin 'cl.exe'))) { throw "cl.exe not found: $msvcBin" }

$env:PATH    = "$msvcBin;$sdkRoot\bin\$ver\x64;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)\include;$sdkRoot\Include\$ver\shared;$sdkRoot\Include\$ver\ucrt;$sdkRoot\Include\$ver\um"
$env:LIB     = "$($msvc.FullName)\lib\x64;$sdkRoot\Lib\$ver\ucrt\x64;$sdkRoot\Lib\$ver\um\x64"

Set-Location $PSScriptRoot
Remove-Item *.obj,*.res,*.exp,*.lib,*.pdb -ErrorAction SilentlyContinue
$cpps = @('Category.cpp','ContextMenu.cpp','Dll.cpp','ExplorerDataProvider.cpp','FVCommands.cpp','Utils.cpp')
& rc.exe /nologo /dUNICODE /d_UNICODE /fo ExplorerDataProvider.res ExplorerDataProvider.rc
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'ExplorerDataProvider.res'))) {
    throw 'Windows resource compilation failed; see rc.exe diagnostics above.'
}
foreach($cpp in $cpps){ cl.exe /nologo /c /EHsc /MD /std:c++17 /W3 /utf-8 /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE $cpp; if($LASTEXITCODE -ne 0){exit $LASTEXITCODE} }
$objs=$cpps | ForEach-Object {[IO.Path]::GetFileNameWithoutExtension($_)+'.obj'}
$outDir = Split-Path -Parent $OutputPath
if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$implib = [IO.Path]::ChangeExtension($OutputPath, '.lib')
$pdb = [IO.Path]::ChangeExtension($OutputPath, '.pdb')
link.exe /nologo /DLL /OUT:"$OutputPath" /IMPLIB:"$implib" /PDB:"$pdb" /DEF:ExplorerDataProvider.def /MACHINE:X64 $objs ExplorerDataProvider.res propsys.lib user32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib uuid.lib comctl32.lib comdlg32.lib
exit $LASTEXITCODE
