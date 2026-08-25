$ErrorActionPreference = 'Stop'
$msvc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
$sdk = 'C:\Program Files (x86)\Windows Kits\10'
$ver = '10.0.26100.0'
$env:PATH = "$msvc\bin\Hostx64\x64;$sdk\bin\$ver\x64;$env:PATH"
$env:INCLUDE = "$msvc\include;$sdk\Include\$ver\shared;$sdk\Include\$ver\ucrt;$sdk\Include\$ver\um"
$env:LIB = "$msvc\lib\x64;$sdk\Lib\$ver\ucrt\x64;$sdk\Lib\$ver\um\x64"
Set-Location $PSScriptRoot
Remove-Item *.obj,*.res,*.exp,*.lib,*.pdb -ErrorAction SilentlyContinue
$cpps = @('Category.cpp','ContextMenu.cpp','Dll.cpp','ExplorerDataProvider.cpp','FVCommands.cpp','Utils.cpp')
rc.exe /nologo /dUNICODE /d_UNICODE /fo ExplorerDataProvider.res ExplorerDataProvider.rc
foreach($cpp in $cpps){ cl.exe /nologo /c /EHsc /MD /std:c++17 /W3 /utf-8 /D_WINDOWS /D_USRDLL /DUNICODE /D_UNICODE $cpp; if($LASTEXITCODE -ne 0){exit $LASTEXITCODE} }
$objs=$cpps | ForEach-Object {[IO.Path]::GetFileNameWithoutExtension($_)+'.obj'}
link.exe /nologo /DLL /OUT:ExplorerDataProviderFtp.dll /DEF:ExplorerDataProvider.def /MACHINE:X64 $objs ExplorerDataProvider.res propsys.lib user32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib uuid.lib comctl32.lib
exit $LASTEXITCODE
