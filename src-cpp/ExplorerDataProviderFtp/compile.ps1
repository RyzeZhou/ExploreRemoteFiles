param([string]$OutputPath = (Join-Path $PSScriptRoot 'ExplorerDataProviderFtp.dll'))

$ErrorActionPreference = 'Stop'

# Load the installed x64 MSVC/Windows SDK environment instead of relying on
# a particular Visual Studio version or the old Win10 VM path.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if ($vsInstall) { $vsInstall = $vsInstall.Trim() }
}
if (-not $vsInstall) {
    throw 'Visual C++ x64 build tools were not found. Install the VS Desktop C++ workload to build the shell extension.'
}
$vcvars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "The x64 MSVC environment script was not found: $vcvars"
}
$envDump = & cmd.exe /d /s /c ('call "' + $vcvars + '" >nul && set')
foreach ($line in $envDump) {
    if ($line -match '^(?<name>[A-Za-z_][A-Za-z0-9_]*)=(?<value>.*)$') {
        Set-Item -Path ("Env:{0}" -f $matches.name) -Value $matches.value
    }
}
foreach ($tool in @('cl.exe', 'link.exe', 'rc.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required build tool was not found after loading vcvars64.bat: $tool"
    }
}
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
