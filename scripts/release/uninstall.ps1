# ExplorerRemoteFs 卸载脚本（当前用户，无需管理员）
# 正式发布的卸载走 Inno 的 unins000.exe（见 src-setup/erf.iss）；本脚本保留给脚本安装的机器，
# 语义与 Inno 版对齐（保留站点配置与凭据）。
# 用法：右键"使用 PowerShell 运行"，或
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1
# 可选：-RemoveConfig 同时删除站点配置（%APPDATA%\ExplorerRemoteFs\connections.json）
#
# 2026-09-18：不再终止资源管理器（旧版会 Stop-Process explorer，桌面黑 1–3 秒，
# 而且一旦脚本中途被杀，写进 AutoRestartShell 的 0 就没人恢复 → 桌面再也回不来）。
# 扩展 DLL 被 explorer 映射着时删不掉，改成改名 .old，交给下次登录的 RunOnce 清理。
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

Write-Host "==> Removing ExplorerRemoteFs registration..."
# 常驻托盘服务与 CLI 锁着安装目录里的 exe，先结束它们。
# 只结束这两个进程：资源管理器全程不动（桌面上什么都看不见）。
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Stop-Process -Name ExplorerRemoteFs.Cli -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

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

# 扩展 DLL：能删就删（最干净）；被资源管理器映射着就改名 —— 改名对已映射文件是允许的。
$leftoverDll = $null
$dll = Join-Path $InstallDir 'ExplorerDataProviderFtp.dll'
if (Test-Path -LiteralPath $dll) {
    try { Remove-Item -LiteralPath $dll -Force -ErrorAction Stop }
    catch {
        $old = "$dll.old"
        $n = 1
        while ((Test-Path -LiteralPath $old) -and $n -lt 20) {
            try { Remove-Item -LiteralPath $old -Force -ErrorAction Stop } catch { }
            if (Test-Path -LiteralPath $old) { $n++; $old = "$dll.old$n" }
        }
        if (Move-Item -LiteralPath $dll -Destination $old -Force -ErrorAction SilentlyContinue) {
            $leftoverDll = $old
            Write-Host "==> 扩展 DLL 正被资源管理器占用，已改名为 $(Split-Path -Leaf $old)"
        }
    }
}

# install dir (DLL / CLI / client)
if (Test-Path $InstallDir) {
    # 重试 + 报告：DLL 被映射时删不掉是硬事实，静默失败会留下"卸载了但文件还在"的假象。
    for ($i = 0; $i -lt 5 -and (Test-Path $InstallDir); $i++) {
        Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $InstallDir) { Start-Sleep -Milliseconds 600 }
    }
    if (Test-Path $InstallDir) {
        Write-Warning "部分文件仍被占用，未能删除：$InstallDir（重启后可以再删一次）"
        Get-ChildItem $InstallDir -Recurse -File -ErrorAction SilentlyContinue |
            Select-Object -First 5 | ForEach-Object { Write-Warning ("  残留：" + $_.FullName) }
    } else {
        Write-Host "==> Install dir removed."
    }
}

# 改名后的旧 DLL 排进"下次登录"清理。RunOnce 的值是整条命令行，必须自己带上 cmd.exe；
# rd 故意不带 /s：只删空目录，万一用户在下次登录前又装回来了，这里绝不会误删新装的文件。
if ($leftoverDll) {
    $runOnce = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
    New-Item -Path $runOnce -Force | Out-Null
    Set-ItemProperty -Path $runOnce -Name ErfCleanup -Type String `
        -Value ('"{0}" /c del /f /q "{1}" & rd "{2}" 2>nul' -f $env:ComSpec, $leftoverDll, $InstallDir)
    Write-Host "==> 残留文件已排入下次登录清理：$leftoverDll"
}

# optional: site config (connections.json) — passwords live in Windows Credential Manager
if ($RemoveConfig) {
    Remove-Item "$env:APPDATA\ExplorerRemoteFs" -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Removed site config. Note: passwords in Credential Manager (ExplorerRemoteFs/*) were NOT touched."
} else {
    Write-Host "Kept site config at $env:APPDATA\ExplorerRemoteFs (re-run with -RemoveConfig to delete)."
}

Write-Host "==> DONE. The navigation-pane entry has been removed."
