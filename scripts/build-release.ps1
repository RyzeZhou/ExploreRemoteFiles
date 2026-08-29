# 构建发布包：C++ DLL + self-contained CLI/GUI + 安装脚本 + zip
# 用法：powershell -ExecutionPolicy Bypass -File scripts/build-release.ps1
$ErrorActionPreference = 'Stop'
$root = 'D:\tools\explorer-remote-fs'
Set-Location "$root\src-cpp\ExplorerDataProviderFtp"

Write-Host '==> 1/5 编译 C++ DLL（需先停 Explorer 释放占用）'
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
powershell -NoProfile -ExecutionPolicy Bypass -File .\compile.ps1 | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw 'C++ compile failed' }

Write-Host '==> 2/5 准备发布目录'
$pkg = "$root\dist\package"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $pkg | Out-Null
Copy-Item "$root\src-cpp\ExplorerDataProviderFtp\ExplorerDataProviderFtp.dll" $pkg
Copy-Item "$root\scripts\release\install.ps1", "$root\scripts\release\uninstall.ps1", "$root\scripts\release\README.txt" $pkg

Write-Host '==> 3/5 发布 CLI（self-contained win-x64）'
dotnet publish "$root\src\ExplorerRemoteFs.Cli\ExplorerRemoteFs.Cli.csproj" -c Release -r win-x64 --self-contained true -o "$pkg\cli" | Out-Null

Write-Host '==> 4/5 发布 GUI 客户端（self-contained win-x64）'
dotnet publish "$root\src-client\RemoteFsClient\RemoteFsClient.csproj" -c Release -r win-x64 --self-contained true -o "$pkg\client" | Out-Null

Write-Host '==> 5/5 打包 zip'
$zip = "$root\dist\ExplorerRemoteFs-win-x64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -CompressionLevel Optimal
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "DONE: $zip ($mb MB)"
Start-Process explorer.exe
