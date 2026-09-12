# ExplorerRemoteFs 安装脚本（当前用户，无需管理员）
# 用法：右键"使用 PowerShell 运行"，或
#   powershell -ExecutionPolicy Bypass -File install.ps1
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ExplorerRemoteFs"
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$explorerWasRunning = $null -ne (Get-Process -Name explorer -ErrorAction SilentlyContinue)
try {
# The resident tray service keeps the CLI/GUI exes locked (named-pipe bridge
# host); terminate it FIRST or file copies below fail with access denied.
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

Write-Host "==> Installing ExplorerRemoteFs to $InstallDir"

# 1. Copy DLL + CLI + GUI client
New-Item -ItemType Directory -Force -Path "$InstallDir\cli"  | Out-Null
New-Item -ItemType Directory -Force -Path "$InstallDir\client" | Out-Null
Copy-Item "$here\ExplorerDataProviderFtp.dll" "$InstallDir\" -Force
Copy-Item "$here\cli\*"  "$InstallDir\cli\"  -Force -Recurse
Copy-Item "$here\client\*" "$InstallDir\client\" -Force -Recurse

# 2. CLI path -> registry (read by the C++ GetCliPath())
New-Item -Path "HKCU:\Software\ExplorerRemoteFs" -Force | Out-Null
Set-ItemProperty "HKCU:\Software\ExplorerRemoteFs" -Name CliPath -Value "$InstallDir\cli\ExplorerRemoteFs.Cli.exe" -Type String
Set-ItemProperty "HKCU:\Software\ExplorerRemoteFs" -Name ClientPath -Value "$InstallDir\client\RemoteFsClient.exe" -Type String
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
Set-ItemProperty $desktopNs -Name '(default)' -Value 'FTP'

# Hide only this extension's own desktop icon. This value is named by our CLSID;
# it does not read or modify the separate This PC desktop-icon setting.
$hideIcons = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel"
New-Item -Path $hideIcons -Force | Out-Null
Set-ItemProperty $hideIcons -Name $folder -Value 1 -Type DWord

# --- CLSID\folder: the NSE itself ---
New-Item -Path "$hk\CLSID\$folder" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder" -Name '(default)' -Value 'FTP'
New-Item -Path "$hk\CLSID\$folder\InprocServer32" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty "$hk\CLSID\$folder\InprocServer32" -Name ThreadingModel -Value 'Apartment'
New-Item -Path "$hk\CLSID\$folder\DefaultIcon" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\DefaultIcon" -Name '(default)' -Value 'shell32.dll,-42'
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
Write-Host "DONE. FTP should appear as a top-level entry in the navigation pane."
Write-Host "Next: add sites via the GUI client (client\RemoteFsClient.exe),"
Write-Host "or create %APPDATA%\ExplorerRemoteFs\connections.json manually (see README.txt)."

}
finally {
    if ($explorerWasRunning) {
        Start-Process explorer.exe
        Start-Sleep -Seconds 1
    }
}


# --- shell\paste verb on the CONTAINER type: Explorer's native paste command
# (Ctrl+V / toolbar Paste / the native "Paste" context item) never calls a
# namespace extension's folder IDropTarget, so those paths did nothing. The
# verb routes them to the CLI, which reads the clipboard file list and uploads
# into the folder named by %V ("<site>:/<path>").
$pasteCmd = '"' + (Join-Path $InstallDir 'cli\ExplorerRemoteFs.Cli.exe') + '" paste "%V"'
New-Item -Path "$hk\RemoteFsMicrosoftCoreType\shell\paste\command" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsMicrosoftCoreType\shell\paste\command" -Name '(default)' -Value $pasteCmd

# --- RemoteFsFileType: FILE-only ProgID (level >= 1 non-directory items) ---
# Context-menu/property-sheet handlers mirror the container type so file items
# keep the WinSCP-style commands and the Permissions page.
New-Item -Path "$hk\RemoteFsFileType\shellex\ContextMenuHandlers\$ctx" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsFileType\shellex\ContextMenuHandlers\$ctx" -Name '(default)' -Value $ctx
New-Item -Path "$hk\RemoteFsFileType\shellex\PropertySheetHandlers\$props" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsFileType\shellex\PropertySheetHandlers\$props" -Name '(default)' -Value $props
# shell\open verb: native double-click / Enter / top-bar Open on REMOTE FILES.
# The shell resolves the default verb via IQueryAssociations -> this ProgID; the
# command receives the FORPARSING name ("<site>:/<path>") as %1.
$openCmd = '"' + (Join-Path $InstallDir 'cli\ExplorerRemoteFs.Cli.exe') + '" open "%1"'
New-Item -Path "$hk\RemoteFsFileType\shell\open\command" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsFileType\shell\open\command" -Name '(default)' -Value $openCmd