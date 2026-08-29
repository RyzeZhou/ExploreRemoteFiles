$ErrorActionPreference='Stop'
$dll='D:\tools\explorer-remote-fs\src-cpp\ExplorerDataProviderFtp\ExplorerDataProviderFtp.dll'
$folder='{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx='{CB8F539D-3B97-4473-9E07-C8248C53248E}'
$display='FTP'
function Set-Default([string]$p,[string]$v){& reg.exe add $p /ve /t REG_SZ /d $v /f|Out-Null;if($LASTEXITCODE-ne 0){throw $p}}
$hk='HKEY_CURRENT_USER\Software\Classes'
Set-Default "$hk\CLSID\$folder" $display
Set-Default "$hk\CLSID\$folder\InprocServer32" $dll
& reg.exe add "$hk\CLSID\$folder\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f|Out-Null
Set-Default "$hk\CLSID\$folder\DefaultIcon" 'shell32.dll,-42'
& reg.exe add "$hk\CLSID\$folder\ShellFolder" /v Attributes /t REG_DWORD /d 0xA0000020 /f|Out-Null
# Pin this namespace to the Explorer navigation pane as a top-level entry
# (sibling of This PC / OneDrive / Linux), see WSL "Linux" node for reference.
& reg.exe add "$hk\CLSID\$folder" /v System.IsPinnedToNameSpaceTree /t REG_DWORD /d 1 /f|Out-Null
& reg.exe add "$hk\CLSID\$folder" /v SortOrderIndex /t REG_DWORD /d 0x42 /f|Out-Null
Set-Default "$hk\CLSID\$ctx" $display
Set-Default "$hk\CLSID\$ctx\InprocServer32" $dll
& reg.exe add "$hk\CLSID\$ctx\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f|Out-Null
Set-Default "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx" $ctx
# Deliberately do NOT register ShellEx\MayChangeDefaultMenu: default folder Open stays native.

# Junction point: Desktop namespace (top-level in navigation pane), NOT MyComputer.
$oldJunction="HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folder"
if(Test-Path "Registry::$oldJunction"){& reg.exe delete $oldJunction /f|Out-Null; Write-Output "REMOVED old MyComputer junction"}
Set-Default "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder" $display
# Hide the real desktop icon (namespace is pinned to nav pane only).
& reg.exe add "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" /v $folder /t REG_DWORD /d 1 /f|Out-Null
Write-Output "FTP NSE REGISTERED AS TOP-LEVEL NAV PANE ENTRY ('$display')"
