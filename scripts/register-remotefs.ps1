# Register RemoteFsShell namespace extension to HKCU (no admin needed).
# Also removes the previous "FolderView SDK Sample" registration.
param()

$ErrorActionPreference = "Stop"

$dllPath = "D:\tools\explorer-remote-fs\src-cpp\RemoteFsShell\RemoteFsShell.dll"
$folderClsid = "{BB7CB9B5-4CD1-4C2E-B585-BF95B141CD15}"
$ctxClsid = "{D3FD7C50-BF7E-4A0C-BA6F-E3694B91ABC8}"
$title = "Remote"
$attrs = 0xA0000020   # SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_CANDELETE

# --- remove old SDK sample registration ---
$oldFolder = "{BA16CE0E-728C-4FC9-98E5-D0B35B384597}"
$oldCtx = "{BB8F539D-3B97-4473-9E07-C8248C53248E}"
foreach ($g in @($oldFolder, $oldCtx)) {
    $p = "HKCU:\Software\Classes\CLSID\$g"
    if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    $j = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$g"
    if (Test-Path $j) { Remove-Item $j -Recurse -Force }
}
$ft = "HKCU:\Software\Classes\FolderViewSampleType"
if (Test-Path $ft) { Remove-Item $ft -Recurse -Force }
"old sample registration removed"

# --- helper to write default value ---
function WriteReg($path, $name, $value, $isDword = $false) {
    $null = New-Item -Path "Registry::$path" -Force
    if ($name -eq "(default)") {
        Set-Item -Path "Registry::$path" -Value $value
    } else {
        if ($isDword) {
            Set-ItemProperty -Path "Registry::$path" -Name $name -Value $value -Type DWord
        } else {
            Set-ItemProperty -Path "Registry::$path" -Name $name -Value $value -Type String
        }
    }
}

$hkcu = "HKEY_CURRENT_USER"

# 1. main folder CLSID
WriteReg "$hkcu\Software\Classes\CLSID\$folderClsid" "(default)" $title
WriteReg "$hkcu\Software\Classes\CLSID\$folderClsid\InprocServer32" "(default)" $dllPath
WriteReg "$hkcu\Software\Classes\CLSID\$folderClsid\InprocServer32" "ThreadingModel" "Apartment"
WriteReg "$hkcu\Software\Classes\CLSID\$folderClsid\DefaultIcon" "(default)" "shell32.dll,-42"
WriteReg "$hkcu\Software\Classes\CLSID\$folderClsid\ShellFolder" "Attributes" $attrs $true

# 2. context menu CLSID
WriteReg "$hkcu\Software\Classes\CLSID\$ctxClsid" "(default)" $title
WriteReg "$hkcu\Software\Classes\CLSID\$ctxClsid\InprocServer32" "(default)" $dllPath
WriteReg "$hkcu\Software\Classes\CLSID\$ctxClsid\InprocServer32" "ThreadingModel" "Apartment"
WriteReg "$hkcu\Software\Classes\CLSID\$ctxClsid\ShellEx\MayChangeDefaultMenu" "(default)" ""
WriteReg "$hkcu\Software\Classes\RemoteFsShellType\shellex\ContextMenuHandlers\$ctxClsid" "(default)" $ctxClsid

# 3. junction point under This PC
WriteReg "$hkcu\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folderClsid" "(default)" $title

# 4. verify
$k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes\CLSID\$folderClsid")
"CLSID default: '$($k.GetValue(''))'"
$i = $k.OpenSubKey("InprocServer32")
"  InprocServer32: '$($i.GetValue(''))'"
"  ThreadingModel: '$($i.GetValue('ThreadingModel'))'"
$j = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folderClsid")
"junction default: '$($j.GetValue(''))'"
"REGISTERED OK"
