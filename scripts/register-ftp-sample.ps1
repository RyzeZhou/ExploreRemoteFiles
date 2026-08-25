$ErrorActionPreference='Stop'
$dll='D:\tools\explorer-remote-fs\src-cpp\ExplorerDataProviderFtp\ExplorerDataProviderFtp.dll'
$folder='{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx='{CB8F539D-3B97-4473-9E07-C8248C53248E}'
function Set-Default([string]$p,[string]$v){& reg.exe add $p /ve /t REG_SZ /d $v /f|Out-Null;if($LASTEXITCODE-ne 0){throw $p}}
$hk='HKEY_CURRENT_USER\Software\Classes'
Set-Default "$hk\CLSID\$folder" 'FTP Microsoft-Core Control'
Set-Default "$hk\CLSID\$folder\InprocServer32" $dll
& reg.exe add "$hk\CLSID\$folder\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f|Out-Null
Set-Default "$hk\CLSID\$folder\DefaultIcon" 'shell32.dll,-42'
& reg.exe add "$hk\CLSID\$folder\ShellFolder" /v Attributes /t REG_DWORD /d 0xA0000020 /f|Out-Null
Set-Default "$hk\CLSID\$ctx" 'FTP Microsoft-Core Control'
Set-Default "$hk\CLSID\$ctx\InprocServer32" $dll
& reg.exe add "$hk\CLSID\$ctx\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f|Out-Null
Set-Default "$hk\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\$ctx" $ctx
# Deliberately do NOT register ShellEx\MayChangeDefaultMenu: default folder Open stays native.
Set-Default "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folder" 'FTP Microsoft-Core Control'
Write-Output 'FTP MICROSOFT CORE REGISTERED'
