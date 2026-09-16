# 编译微软 Explorer Data Provider Sample 为 x64 DLL
$msvc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer = "10.0.26100.0"
$env:PATH = "$msvc\bin\Hostx64\x64;$sdk\bin\$sdkVer\x64;$env:PATH"
$env:INCLUDE = "$msvc\include;$sdk\Include\$sdkVer\shared;$sdk\Include\$sdkVer\ucrt;$sdk\Include\$sdkVer\um"
$env:LIB = "$msvc\lib\x64;$sdk\Lib\$sdkVer\ucrt\x64;$sdk\Lib\$sdkVer\um\x64"

Set-Location "D:\tools\explorer-remote-fs\src-cpp\ExplorerDataProvider"
Remove-Item *.obj,*.res,*.dll,*.exp,*.lib,*.pdb -ErrorAction SilentlyContinue

$cpps = @("Category.cpp","ContextMenu.cpp","Dll.cpp","ExplorerDataProvider.cpp","FVCommands.cpp","Utils.cpp")

# 1. 编译 .rc 资源为 .res
rc.exe /nologo /dUNICODE /d_UNICODE /fo ExplorerDataProvider.res ExplorerDataProvider.rc 2>&1 | Out-Null
"rc.exe -> ExplorerDataProvider.res: $(Test-Path ExplorerDataProvider.res)"

# 2. 编译每个 .cpp 为 .obj
$incArgs = @("/I","$msvc\include","/I","$sdk\Include\$sdkVer\shared","/I","$sdk\Include\$sdkVer\ucrt","/I","$sdk\Include\$sdkVer\um")
$defArgs = @("/D_WINDOWS","/D_USRDLL","/DEXPLORERDATAPROVIDER_EXPORTS","/DNDEBUG","/DUNICODE","/D_UNICODE")
$commonArgs = @("/nologo","/c","/EHsc","/MD","/std:c++17","/W3") + $defArgs + $incArgs

$okCount = 0
foreach ($cpp in $cpps) {
  $out = & cl.exe @commonArgs $cpp 2>&1
  $errs = $out | Where-Object { $_ -match "error C" }
  if ($errs) {
    "  $cpp ERRORS:"
    $errs | Select-Object -First 3 | ForEach-Object { "    $_" }
  } else {
    "  $cpp OK"
    $okCount++
  }
}
"=== $okCount / $($cpps.Count) cpp compiled ==="

if ($okCount -eq $cpps.Count) {
  # 3. 链接为 DLL
  $objs = $cpps | ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_) + ".obj" }
  $linkArgs = @("/nologo","/DLL","/OUT:ExplorerDataProvider.dll","/DEF:ExplorerDataProvider.def","/MACHINE:X64") + $objs + @("ExplorerDataProvider.res","propsys.lib","user32.lib","shell32.lib","ole32.lib","oleaut32.lib","advapi32.lib","uuid.lib")
  $out = & link.exe @linkArgs 2>&1
  $errs = $out | Where-Object { $_ -match "error|unresolved" }
  if ($errs) {
    "=== LINK ERRORS ==="
    $errs | Select-Object -First 5
  } else {
    "=== LINK OK ==="
    "DLL: $(Test-Path ExplorerDataProvider.dll), size: $((Get-Item ExplorerDataProvider.dll -ErrorAction SilentlyContinue).Length)"
  }
}
