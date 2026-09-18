# ExplorerRemoteFs 安装脚本（当前用户，无需管理员）
# 正式发布走 Inno Setup 安装包（src-setup/erf.iss -> dist/Erf-*-Setup.exe）；本脚本保留给
# "不装安装包、直接脚本注册"的开发/排障场景，注册语义两边要保持一致，改动时都看一眼。
# 用法：右键"使用 PowerShell 运行"，或
#   powershell -ExecutionPolicy Bypass -File install.ps1
#
# 2026-09-18：不再终止资源管理器（旧版会 Stop-Process explorer，桌面黑 1–3 秒）。
# 扩展 DLL 被 explorer 映射着时删不掉/覆盖不了，用"改名成 .old"绕开 —— 见 Move-DllAside。
# 代价：新版本扩展要等资源管理器下次启动才生效（注销/重启，或任务管理器里重启 explorer）。
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ExplorerRemoteFs"
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# 已被映射的文件既不能删也不能覆盖，但**可以改名**（只是改目录项，文件对象还在内存里）。
# 所以：先试着直接删（explorer 没加载过它时能删掉，最干净），删不掉就改名 .old，
# 新文件随后写到原路径上。.old 交给下次登录的 RunOnce 清掉。
function Move-DllAside([string]$DllPath, [string]$InstallDir) {
    if (-not (Test-Path -LiteralPath $DllPath)) { return $null }
    try { Remove-Item -LiteralPath $DllPath -Force -ErrorAction Stop; return $null } catch { }
    $old = "$DllPath.old"
    $n = 1
    while ((Test-Path -LiteralPath $old) -and $n -lt 20) {
        try { Remove-Item -LiteralPath $old -Force -ErrorAction Stop } catch { }
        if (Test-Path -LiteralPath $old) { $n++; $old = "$DllPath.old$n" }
    }
    Move-Item -LiteralPath $DllPath -Destination $old -Force
    Write-Host "==> 扩展 DLL 正被资源管理器占用，已改名为 $(Split-Path -Leaf $old)（下次登录自动清理）"
    # RunOnce 的值是**整条命令行**，必须自己带上 cmd.exe；rd 不带 /s，绝不会误删用户重装的目录
    $runOnce = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
    New-Item -Path $runOnce -Force | Out-Null
    Set-ItemProperty -Path $runOnce -Name ErfCleanup -Type String `
        -Value ('"{0}" /c del /f /q "{1}" & rd "{2}" 2>nul' -f $env:ComSpec, $old, $InstallDir)
    return $old
}

try {
# The resident tray service keeps the CLI/GUI exes locked (named-pipe bridge
# host); terminate it FIRST or file copies below fail with access denied.
# CLI 子进程一样会锁住 cli\*.dll（实测 2026-09-17：卸载后整个 cli 目录删不掉）。
#
# 注意这里**没有** Stop-Process explorer：扩展 DLL 的占用改用"改名"绕开，
# 桌面不黑屏、也不动 AutoRestartShell（旧版把它写成 0 且中途失败就再也回不来）。
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Stop-Process -Name ExplorerRemoteFs.Cli -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

Write-Host "==> Installing ExplorerRemoteFs to $InstallDir"

# 1. Copy DLL + CLI + GUI client
New-Item -ItemType Directory -Force -Path "$InstallDir\cli"  | Out-Null
New-Item -ItemType Directory -Force -Path "$InstallDir\client" | Out-Null
Move-DllAside -DllPath (Join-Path $InstallDir 'ExplorerDataProviderFtp.dll') -InstallDir $InstallDir | Out-Null
Copy-Item "$here\ExplorerDataProviderFtp.dll" "$InstallDir\" -Force
Copy-Item "$here\cli\*"  "$InstallDir\cli\"  -Force -Recurse
Copy-Item "$here\client\*" "$InstallDir\client\" -Force -Recurse

# 2. CLI path -> registry (read by the C++ GetCliPath())
New-Item -Path "HKCU:\Software\ExplorerRemoteFs" -Force | Out-Null
Set-ItemProperty "HKCU:\Software\ExplorerRemoteFs" -Name CliPath -Value "$InstallDir\cli\ExplorerRemoteFs.Cli.exe" -Type String
Set-ItemProperty "HKCU:\Software\ExplorerRemoteFs" -Name ClientPath -Value "$InstallDir\client\RemoteFsClient.exe" -Type String

# Register the public ERF address scheme. The handler belongs to the resident
# client, which turns erf:<site>:/path into a private Shell parsing name.
$erfProtocol = 'HKCU:\Software\Classes\erf'
$erfRegistered = 'Registry::HKEY_CLASSES_ROOT\erf'
$existingErf = (Get-ItemProperty -LiteralPath $erfRegistered -ErrorAction SilentlyContinue).'ERF.HandlerOwner'
if ((Test-Path -LiteralPath $erfRegistered) -and $existingErf -ne 'ExplorerRemoteFs') {
    throw 'The ERF address protocol is already owned by another application; installation stopped without overwriting it.'
}
New-Item -Path "$erfProtocol\shell\open\command" -Force | Out-Null
Set-ItemProperty -LiteralPath $erfProtocol -Name '(default)' -Value 'URL: Explorer Remote Files' -Type String
Set-ItemProperty -LiteralPath $erfProtocol -Name 'URL Protocol' -Value '' -Type String
Set-ItemProperty -LiteralPath $erfProtocol -Name 'ERF.HandlerOwner' -Value 'ExplorerRemoteFs' -Type String
Set-ItemProperty -LiteralPath "$erfProtocol\shell\open\command" -Name '(default)' -Value ('"{0}" --open-erf "%1"' -f "$InstallDir\client\RemoteFsClient.exe") -Type String
# Keep an editable translation template in roaming profile; upgrades never overwrite it.
$configDir = Join-Path $env:APPDATA 'ExplorerRemoteFs'
New-Item -ItemType Directory -Force -Path $configDir | Out-Null
$translationFile = Join-Path $configDir 'explorer-translations.yaml'
if (-not (Test-Path -LiteralPath $translationFile)) {
    Copy-Item "$here\explorer-translations.yaml" $translationFile -Force
}
$translationExample = Join-Path $configDir 'explorer-translations.example.yaml'
if (-not (Test-Path -LiteralPath $translationExample)) {
    Copy-Item "$here\explorer-translations.example.yaml" $translationExample -Force
}

# 3. Register the namespace extension
$folder='{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx='{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$props='{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'
$dll = "$InstallDir\ExplorerDataProviderFtp.dll"
$hk='HKCU:\Software\Classes'

# Attach the namespace root to Desktop. Explorer resolves navigation-pane roots
# and site:/ parsing through this namespace registration.
$desktopNs = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder"
New-Item -Path $desktopNs -Force | Out-Null
Set-ItemProperty $desktopNs -Name '(default)' -Value '易远传'

# Hide only this extension's own desktop icon: we write ONE value named by our
# CLSID. Never `New-Item -Force` on this key -- measured 2026-09-14: before the
# run the key held {C816CE0E-...} plus Windows' own
# {20D04FE0-3AEA-1069-A2D8-08002B30309D} = 0 (the "show This PC" flag), and after
# install.ps1 ran with -Force only our value survived. -Force rebuilds a key that
# belongs to Windows and destroys every sibling value, i.e. it silently resets the
# user's desktop icons. Rule: create keys we own freely, but for a key we do NOT
# own, create it only when absent and then touch only our own value.
$hideIcons = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel"
if (-not (Test-Path $hideIcons)) { New-Item -Path $hideIcons | Out-Null }
Set-ItemProperty $hideIcons -Name $folder -Value 1 -Type DWord

# --- CLSID\folder: the NSE itself ---
New-Item -Path "$hk\CLSID\$folder" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder" -Name '(default)' -Value '易远传 (Explorer Remote Files)'
New-Item -Path "$hk\CLSID\$folder\InprocServer32" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty "$hk\CLSID\$folder\InprocServer32" -Name ThreadingModel -Value 'Apartment'
New-Item -Path "$hk\CLSID\$folder\DefaultIcon" -Force | Out-Null
# 命名空间图标：用扩展 DLL 里的图标资源（id 101，见 ExplorerDataProvider.rc 的 IDI_ERF）。
# 以前是 shell32.dll,-42（一把文件夹图标），与我们自己的产品图标对不上。
Set-ItemProperty "$hk\CLSID\$folder\DefaultIcon" -Name '(default)' -Value ('"{0}",-101' -f $dll)
New-Item -Path "$hk\CLSID\$folder\ShellFolder" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\ShellFolder" -Name Attributes -Value 0xA8000020 -Type DWord
# pin to navigation pane (top-level entry, sibling of This PC)
Set-ItemProperty "$hk\CLSID\$folder" -Name 'System.IsPinnedToNameSpaceTree' -Value 1 -Type DWord
Set-ItemProperty "$hk\CLSID\$folder" -Name 'SortOrderIndex' -Value 0x42 -Type DWord

# --- CLSID\ctx: WinSCP-style context menu handler ---
New-Item -Path "$hk\CLSID\$ctx" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$ctx" -Name '(default)' -Value 'FTP Microsoft-Core Control'
New-Item -Path "$hk\CLSID\$ctx\InprocServer32" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$ctx\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty "$hk\CLSID\$ctx\InprocServer32" -Name ThreadingModel -Value 'Apartment'
New-Item -Path "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx" -Name '(default)' -Value $ctx
# Legacy builds registered this key, which lets a third-party context menu override
# Explorer's folder default action. Remove it so double-click always navigates.
Remove-Item -LiteralPath "$hk\CLSID\$ctx\ShellEx\MayChangeDefaultMenu" -Recurse -Force -ErrorAction SilentlyContinue

# --- CLSID\props: property sheet (Permissions / Connection pages) ---
New-Item -Path "$hk\CLSID\$props" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$props" -Name '(default)' -Value 'FTP Microsoft-Core Control'
New-Item -Path "$hk\CLSID\$props\InprocServer32" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$props\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty "$hk\CLSID\$props\InprocServer32" -Name ThreadingModel -Value 'Apartment'
New-Item -Path "$hk\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\$props" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\$props" -Name '(default)' -Value $props



# Start the per-user resident control center and its reusable Provider connection pool.
Start-Process -FilePath "$InstallDir\client\RemoteFsClient.exe" -ArgumentList '--background' -WindowStyle Hidden

Write-Host "==> Registration done. Restarting Explorer..."

Write-Host ""
Write-Host "DONE. 易远传 should appear as a top-level entry in the navigation pane."
Write-Host "Next: add sites via the GUI client (client\RemoteFsClient.exe),"
Write-Host "or create %APPDATA%\ExplorerRemoteFs\connections.json manually (see README.txt)."

}
finally {
    # 没有需要还原的全局设置：本脚本不碰 explorer，也不碰 AutoRestartShell。
    # （旧版在这里把 AutoRestartShell 恢复原值并重新拉起 explorer —— 现在两者都不需要了；
    #   顺带说明为什么旧写法危险：一旦脚本在中途被杀，那个 0 就留在用户机器上，
    #   此后资源管理器崩了 Windows 再也不会自动把它拉回来，桌面就一直黑着。）
}


# NOTE (2026-09-12): do NOT register shell\paste (or any sole verb) under the
# CONTAINER ProgID. A lone verb under the shell key becomes the DEFAULT verb,
# so double-clicking a folder executed paste instead of navigating (folder
# items could not be opened at all). Explorer's native paste is instead hooked
# through the folder background context menu's canonical "paste" verb
# (IContextMenu::GetCommandString/GCS_VERBW) in the extension DLL.

# --- RemoteFsFileType: FILE-only ProgID (level >= 1 non-directory items) ---
# Context-menu/property-sheet handlers mirror the container type so file items
# keep the WinSCP-style commands and the Permissions page.
New-Item -Path "$hk\RemoteFsFileType\shellex\ContextMenuHandlers\$ctx" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsFileType\shellex\ContextMenuHandlers\$ctx" -Name '(default)' -Value $ctx
New-Item -Path "$hk\RemoteFsFileType\shellex\PropertySheetHandlers\$props" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsFileType\shellex\PropertySheetHandlers\$props" -Name '(default)' -Value $props
# shell\open verb: native double-click / Enter / top-bar Open on REMOTE FILES.
# The shell resolves the default verb via IQueryAssociations -> this ProgID; the
# command receives the FORPARSING name (normally "::{CLSID}\<site>:/<path>")
# as %1; the CLI strips the namespace prefix before resolving the provider.
$openCmd = '"' + (Join-Path $InstallDir 'cli\ExplorerRemoteFs.Cli.exe') + '" open "%1"'
New-Item -Path "$hk\RemoteFsFileType\shell\open\command" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsFileType\shell\open\command" -Name '(default)' -Value $openCmd
