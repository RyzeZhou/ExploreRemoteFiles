# ExplorerRemoteFs 安装脚本（当前用户，无需管理员）
# 用法：右键"使用 PowerShell 运行"，或
#   powershell -ExecutionPolicy Bypass -File install.ps1
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ExplorerRemoteFs"
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

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

# 3. Register the namespace extension
$folder='{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx='{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$props='{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'
$dll = "$InstallDir\ExplorerDataProviderFtp.dll"
$hk='HKCU:\Software\Classes'

# --- CLSID\folder: the NSE itself ---
New-Item -Path "$hk\CLSID\$folder" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder" -Name '(default)' -Value 'FTP'
New-Item -Path "$hk\CLSID\$folder\InprocServer32" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty "$hk\CLSID\$folder\InprocServer32" -Name ThreadingModel -Value 'Apartment'
New-Item -Path "$hk\CLSID\$folder\DefaultIcon" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\DefaultIcon" -Name '(default)' -Value 'shell32.dll,-42'
New-Item -Path "$hk\CLSID\$folder\ShellFolder" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$folder\ShellFolder" -Name Attributes -Value 0xA0000020 -Type DWord
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

# --- CLSID\props: property sheet (Permissions / Connection pages) ---
New-Item -Path "$hk\CLSID\$props" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$props" -Name '(default)' -Value 'FTP Microsoft-Core Control'
New-Item -Path "$hk\CLSID\$props\InprocServer32" -Force | Out-Null
Set-ItemProperty "$hk\CLSID\$props\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty "$hk\CLSID\$props\InprocServer32" -Name ThreadingModel -Value 'Apartment'
New-Item -Path "$hk\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\$props" -Force | Out-Null
Set-ItemProperty "$hk\RemoteFsMicrosoftCoreType\shellex\PropertySheetHandlers\$props" -Name '(default)' -Value $props

# --- junction: Desktop\NameSpace + hide the desktop icon ---
New-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder" -Force | Out-Null
Set-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder" -Name '(default)' -Value 'FTP'
New-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" -Force | Out-Null
Set-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" -Name $folder -Value 1 -Type DWord

Write-Host "==> Registration done. Restarting Explorer..."
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Start-Process explorer.exe
Start-Sleep -Seconds 1

Write-Host ""
Write-Host "DONE. FTP should appear as a top-level entry in the navigation pane."
Write-Host "Next: add sites via the GUI client (client\RemoteFsClient.exe),"
Write-Host "or create %APPDATA%\ExplorerRemoteFs\connections.json manually (see README.txt)."
