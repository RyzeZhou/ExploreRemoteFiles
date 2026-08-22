@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "D:\tools\explorer-remote-fs\src-cpp\ExplorerDataProvider"
msbuild ExplorerDataProvider.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal
echo === ExitCode: %ERRORLEVEL% ===
