# Register RemoteFsShell namespace extension to HKCU (no admin needed).
# Also removes the previous "FolderView SDK Sample" registration.
param()

$ErrorActionPreference = "Stop"

$dllPath = "D:\tools\explorer-remote-fs\src-cpp\RemoteFsShell\RemoteFsShell.dll"
$folderClsid = "{BB7CB9B5-4CD1-4C2E-B585-BF95B141CD15}"
$ctxClsid = "{D3FD7C50-BF7E-4A0C-BA6F-E3694B91ABC8}"
$title = "Remote"
# 0xA0000020 = SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_CANDELETE
# NOTE: do NOT add SFGAO_BROWSABLE (RESEARCH_LOG): it makes explorer request
# the private view interface 93F81976 instead of falling back to IShellView,
# breaking deep-folder view creation.
$attrs = 0xA0000020

# Keep the original Microsoft sample registered as an independent navigation
# control. Its separate CLSID/DLL lets us compare shell behavior against both
# real FTP and the in-memory static-control hierarchy in RemoteFsShell.
# --- helper to write default value ---
function WriteReg($path, $name, $value, $isDword = $false) {
    $registryPath = "Registry::$path"
    # New-Item -Force on an existing registry key clears its default value on
    # this Windows build.  Only create missing keys; subsequent named-value
    # writes must preserve the InprocServer32 default DLL path.
    if (-not (Test-Path $registryPath)) {
        $null = New-Item -Path $registryPath -Force
    }
    if ($name -eq "(default)") {
        # Set-Item on the Registry provider does not reliably write the
        # (default) value of an existing key; use reg.exe instead.
        & reg.exe add $path /ve /t REG_SZ /d $value /f | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "reg add failed for $path" }
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
# Control experiment: detach the rich handler so native folder Open is the
# only navigation path. The COM class remains registered for later restoration.
$menuRoot = "Registry::HKEY_CURRENT_USER\Software\Classes\RemoteFsShellType\shellex\ContextMenuHandlers"
if (Test-Path $menuRoot) { Remove-Item $menuRoot -Recurse -Force }
$mayChange = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$ctxClsid\ShellEx\MayChangeDefaultMenu"
if (Test-Path $mayChange) { Remove-Item $mayChange -Recurse -Force }

# 3. junction point under This PC
WriteReg "$hkcu\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folderClsid" "(default)" $title

# 4. verify
$k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes\CLSID\$folderClsid")
"CLSID default: '$($k.GetValue(''))'"
$i = $k.OpenSubKey("InprocServer32")
$registeredDll = [string]$i.GetValue('')
"  InprocServer32: '$registeredDll'"
"  ThreadingModel: '$($i.GetValue('ThreadingModel'))'"
if (-not $registeredDll.Equals($dllPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "InprocServer32 verification failed: expected '$dllPath', got '$registeredDll'"
}
$j = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$folderClsid")
"junction default: '$($j.GetValue(''))'"
"REGISTERED OK"
