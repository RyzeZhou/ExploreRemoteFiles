ï»¿# ExplorerRemoteFs å¸è½½èæ¬ï¼å½åç¨æ·ï¼æ éç®¡çåï¼
# ç¨æ³ï¼å³é®"ä½¿ç¨ PowerShell è¿è¡"ï¼æ
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1
# å¯éï¼-RemoveConfig åæ¶å é¤ç«ç¹éç½®ï¼%APPDATA%\ExplorerRemoteFs\connections.jsonï¼
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ExplorerRemoteFs",
    [switch]$RemoveConfig
)
$ErrorActionPreference = 'Continue'
$folder='{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx='{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$props='{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'
$hk='HKCU:\Software\Classes'
$erfProtocol="$hk\erf"

$explorerWasRunning = $null -ne (Get-Process -Name explorer -ErrorAction SilentlyContinue)
try {

Write-Host "==> Removing ExplorerRemoteFs registration..."
# Terminate the resident tray service first so its files are not locked
# when the install dir is removed below.
#
# è¿è¦ä¸´æ¶å³æ"èµæºç®¡çå¨èªå¨éå¯"ï¼ææ explorer.exe å Windows é»è®¤ä¼ç«å»æå®æèµ·æ¥
# ï¼AutoRestartShell=1ï¼ï¼æ°èµ·ç explorer é©¬ä¸åææ©å± DLL å è½½åå» ââ å®æµç»æå°±æ¯
# **DLL å ä¸æãæ§çæ¬çå¨çä¸**ãæä»¥ï¼å³èªå¨éå¯ â æ explorer â å æä»¶ â æ¢å¤è®¾ç½® â éå¯ explorerã
$winlogon = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon'
$autoRestart = (Get-ItemProperty -Path $winlogon -Name AutoRestartShell -ErrorAction SilentlyContinue).AutoRestartShell
try { New-Item -Path $winlogon -Force | Out-Null; Set-ItemProperty -Path $winlogon -Name AutoRestartShell -Value 0 -Type DWord } catch { }
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
# CLI 子进程一样会锁住 cli*.dll（实测 2026-09-17：卸载后整个 cli 目录删不掉）
Stop-Process -Name ExplorerRemoteFs.Cli -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

# context menu + property sheet handlers
Remove-Item "$hk\RemoteFsMicrosoftCoreType" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$hk\RemoteFsFileType" -Recurse -Force -ErrorAction SilentlyContinue
# Remove the ERF URI protocol only when this installation registered it.
if ((Get-ItemProperty -LiteralPath $erfProtocol -ErrorAction SilentlyContinue).'ERF.HandlerOwner' -eq 'ExplorerRemoteFs') {
    Remove-Item -LiteralPath $erfProtocol -Recurse -Force -ErrorAction SilentlyContinue
}
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
    # éè¯ + æ¥åï¼DLL è¢«æ å°æ¶å ä¸ææ¯ç¡¬äºå®ï¼éé»å¤±è´¥ä¼çä¸"å¸è½½äºä½æä»¶è¿å¨"çåè±¡ã
    for ($i = 0; $i -lt 5 -and (Test-Path $InstallDir); $i++) {
        Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $InstallDir) { Start-Sleep -Milliseconds 600 }
    }
    if (Test-Path $InstallDir) {
        Write-Warning "é¨åæä»¶ä»è¢«å ç¨ï¼æªè½å é¤ï¼$InstallDirï¼éå¯åå¯ä»¥åå ä¸æ¬¡ï¼"
        Get-ChildItem $InstallDir -Recurse -File -ErrorAction SilentlyContinue |
            Select-Object -First 5 | ForEach-Object { Write-Warning ("  æ®çï¼" + $_.FullName) }
    } else {
        Write-Host "==> Install dir removed."
    }
}

# optional: site config (connections.json) â passwords live in Windows Credential Manager
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
    # æ¢å¤"èµæºç®¡çå¨èªå¨éå¯"çåå¼ï¼åæ¥æ²¡æè¿ä¸ªå¼å°±å ææä»¬å çï¼
    try {
        if ($null -eq $autoRestart) { Remove-ItemProperty -Path $winlogon -Name AutoRestartShell -ErrorAction SilentlyContinue }
        else { Set-ItemProperty -Path $winlogon -Name AutoRestartShell -Value $autoRestart -Type DWord }
    } catch { }
    if ($explorerWasRunning) {
        Start-Process explorer.exe
        Start-Sleep -Seconds 1
    }
}
