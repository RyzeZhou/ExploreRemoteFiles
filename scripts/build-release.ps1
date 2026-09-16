# Build a development install directory: C++ DLL + self-contained CLI/GUI + scripts
# 用法：powershell -ExecutionPolicy Bypass -File scripts/build-release.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$distDir = Join-Path $root 'dist'
$nativeDir = Join-Path $distDir '.native-build'
$nativeDll = Join-Path $nativeDir 'ExplorerDataProviderFtp.dll'
New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null
Set-Location "$root\src-cpp\ExplorerDataProviderFtp"

Write-Host '==> 1/4 Build C++ DLL'
& powershell -NoProfile -ExecutionPolicy Bypass -File .\compile.ps1 -OutputPath $nativeDll
if ($LASTEXITCODE -ne 0) { throw 'C++ compile failed' }

Write-Host '==> 2/4 Prepare install directory'
$pkg = Join-Path $distDir 'ExplorerRemoteFs-win-x64'
$cliOut = Join-Path $pkg 'cli'
$clientOut = Join-Path $pkg 'client'
New-Item -ItemType Directory -Force -Path $pkg | Out-Null
# 清空而不是删除目录本身：如果某个进程把这个目录当成当前工作目录，
# Remove-Item 整个目录会失败（"being used by another process"，实测），
# 而删除里面的内容是成功的。清不干净就报错，绝不留下昨天的 DLL 混进包里。
Get-ChildItem $pkg -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
if (Get-ChildItem $pkg -Force) {
    throw "无法清空 $pkg —— 有进程仍占用其中的文件（先关掉资源管理器/客户端再重试）。"
}
Copy-Item $nativeDll $pkg
Copy-Item "$root\scripts\release\install.ps1", "$root\scripts\release\uninstall.ps1", "$root\scripts\release\README.txt", "$root\scripts\release\explorer-translations.yaml", "$root\scripts\release\explorer-translations.example.yaml" $pkg

Write-Host '==> 3/4 Publish CLI (self-contained win-x64)'
& dotnet publish (Join-Path $root 'src\ExplorerRemoteFs.Cli\ExplorerRemoteFs.Cli.csproj') -c Release -r win-x64 --self-contained true -o $cliOut
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $cliOut 'ExplorerRemoteFs.Cli.exe'))) {
    throw 'CLI publish failed or ExplorerRemoteFs.Cli.exe is missing'
}

Write-Host '==> 4/4 Publish GUI client (self-contained win-x64)'
& dotnet publish (Join-Path $root 'src-client\RemoteFsClient\RemoteFsClient.csproj') -c Release -r win-x64 --self-contained true -o $clientOut
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $clientOut 'RemoteFsClient.exe'))) {
    throw 'GUI publish failed or RemoteFsClient.exe is missing'
}

Write-Host "DONE: $pkg"
