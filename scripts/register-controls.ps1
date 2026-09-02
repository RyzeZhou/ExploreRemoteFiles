$ErrorActionPreference = 'Stop'
$sampleDll = 'D:\tools\explorer-remote-fs\src-cpp\ExplorerDataProvider\ExplorerDataProvider.dll'
$folder = '{BA16CE0E-728C-4FC9-98E5-D0B35B384597}'
$ctx = '{BB8F539D-3B97-4473-9E07-C8248C53248E}'
function Ensure-Key([string]$path) {
    if (-not (Test-Path "Registry::$path")) {
        & reg.exe add $path /f | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "reg create $path" }
    }
}
function Set-Default([string]$path, [string]$value) {
    Ensure-Key $path
    & reg.exe add $path /ve /t REG_SZ /d $value /f | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg add $path" }
}
$hk = 'HKEY_CURRENT_USER\Software\Classes'
Set-Default "$hk\CLSID\$folder" 'FolderView SDK Sample Control'
Set-Default "$hk\CLSID\$folder\InprocServer32" $sampleDll
New-ItemProperty "Registry::$hk\CLSID\$folder\InprocServer32" -Name ThreadingModel -Value Apartment -PropertyType String -Force | Out-Null
Set-Default "$hk\CLSID\$folder\DefaultIcon" 'shell32.dll,-42'
Ensure-Key "$hk\CLSID\$folder\ShellFolder"
& reg.exe add "$hk\CLSID\$folder\ShellFolder" /v Attributes /t REG_DWORD /d 0xA0000020 /f | Out-Null
if ($LASTEXITCODE -ne 0) { throw "reg Attributes" }
Set-Default "$hk\CLSID\$ctx" 'FolderView SDK Sample Control'
Set-Default "$hk\CLSID\$ctx\InprocServer32" $sampleDll
New-ItemProperty "Registry::$hk\CLSID\$ctx\InprocServer32" -Name ThreadingModel -Value Apartment -PropertyType String -Force | Out-Null
Set-Default "$hk\FolderViewSampleType\shellex\ContextMenuHandlers\$ctx" $ctx
Set-Default "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folder" 'FolderView SDK Sample Control'
Write-Output 'CONTROL SAMPLE REGISTERED'
