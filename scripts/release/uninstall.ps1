# ExplorerRemoteFs 卸载脚本（当前用户，无需管理员）
# 用法：右键"使用 PowerShell 运行"，或
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1
# 可选：-RemoveConfig 同时删除站点配置（%APPDATA%\ExplorerRemoteFs\connections.json）
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ExplorerRemoteFs",
    [switch]$RemoveConfig
)
$ErrorActionPreference = 'Continue'
$folder='{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx='{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$props='{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'
$hk='HKCU:\Software\Classes'

$explorerWasRunning = $null -ne (Get-Process -Name explorer -ErrorAction SilentlyContinue)
try {

Write-Host "==> Removing ExplorerRemoteFs registration..."
# Terminate the resident tray service first so its files are not locked
# when the install dir is removed below.
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

# context menu + property sheet handlers
Remove-Item "$hk\RemoteFsMicrosoftCoreType" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$hk\RemoteFsFileType" -Recurse -Force -ErrorAction SilentlyContinue
# CLSIDs
Remove-Item "$hk\CLSID\$folder" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$hk\CLSID\$ctx"    -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$hk\CLSID\$props"  -Recurse -Force -ErrorAction SilentlyContinue
# Clean up only this extension's legacy desktop keys from older installs.
Remove-Item "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder" -Recurse -Force -ErrorAction SilentlyContinue
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" -Name $folder -ErrorAction SilentlyContinue
# config key (CliPath)
Remove-Item "HKCU:\Software\ExplorerRemoteFs" -Recurse -Force -ErrorAction SilentlyContinue

# install dir (DLL / CLI / client)
if (Test-Path $InstallDir) {
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
}

# optional: site config (connections.json) — passwords live in Windows Credential Manager
if ($RemoveConfig) {
    Remove-Item "$env:APPDATA\ExplorerRemoteFs" -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Removed site config. Note: passwords in Credential Manager (ExplorerRemoteFs/*) were NOT touched."
} else {
    Write-Host "Kept site config at $env:APPDATA\ExplorerRemoteFs (re-run with -RemoveConfig to delete)."
}

Write-Host "==> Restarting Explorer..."
Write-Host "DONE. FTP entry removed from the navigation pane."

}
finally {
    if ($explorerWasRunning) {
        Start-Process explorer.exe
        Start-Sleep -Seconds 1
    }
}
