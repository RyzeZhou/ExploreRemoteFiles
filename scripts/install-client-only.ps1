# 安装"只替换客户端"的最小更新（DLL 未变，无需重启资源管理器）。
# 用法（在本机）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .	mp\explore-remote-files\install-client-only.ps1
$ErrorActionPreference = 'Stop'
$root    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$src     = Join-Path $root 'dist\client-only'
$install = 'D:\Program\ExplorerRemoteFs'
$dst     = Join-Path $install 'client'

if (-not (Test-Path (Join-Path $src 'RemoteFsClient.exe'))) {
    throw "先编译：$src\RemoteFsClient.exe 不存在（在 Win10 VM 上跑 build-client-only.ps1）"
}

Write-Host '==> 1/4 停止常驻服务与协议 shim'
Get-Process RemoteFsClient -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host ("  stop pid={0}" -f $_.Id)
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
Get-Process ExplorerRemoteFs.Cli -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

Write-Host '==> 2/4 备份现有 client 目录'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $install ("client.bak-" + $stamp)
Copy-Item $dst $backup -Recurse -Force
Write-Host "  备份 -> $backup"

Write-Host '==> 3/4 替换 client 目录'
Get-ChildItem $dst -Force | Remove-Item -Recurse -Force
Copy-Item (Join-Path $src '*') $dst -Recurse -Force
$newExe = Join-Path $dst 'RemoteFsClient.exe'
$fi = Get-Item $newExe
Write-Host ("  新客户端: {0} bytes, {1}" -f $fi.Length, $fi.LastWriteTime)

Write-Host '==> 4/4 启动常驻服务'
Start-Process -FilePath $newExe -ArgumentList '--background'
Start-Sleep -Seconds 2
Get-Process RemoteFsClient -ErrorAction SilentlyContinue | Select-Object Id, ProcessName, StartTime | Format-Table -AutoSize
Write-Host 'DONE. 客户端已更新；资源管理器无需重启（DLL 未变）。'
