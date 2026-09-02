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
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $pkg | Out-Null
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
