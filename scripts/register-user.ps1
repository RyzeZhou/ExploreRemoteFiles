# Register ExplorerRemoteFs for the CURRENT USER only (no admin needed).
# Usage:  powershell -ExecutionPolicy Bypass -File register-user.ps1 [-BinDir <path>]
# This writes CLSID + junction point under HKCU\Software\Classes, so no UAC is required.
# Remove with: unregister-user.ps1

param(
    [string]$BinDir = ""
)

$ErrorActionPreference = "Stop"

$guid = "9EADE55A-2AB6-4087-B5FF-D090D38173E6"
$name = "Servers"
$tooltip = "SFTP / FTP / FTPS remote servers"

if (-not $BinDir) {
    $BinDir = Join-Path $PSScriptRoot "..\src\ExplorerRemoteFs\bin\Debug\net8.0-windows"
}
$BinDir = [System.IO.Path]::GetFullPath($BinDir)
$comhost = Join-Path $BinDir "ExplorerRemoteFs.comhost.dll"
if (-not (Test-Path $comhost)) {
    throw "comhost not found: $comhost (build the project first)"
}

# 1. CLSID registration under HKCU\Software\Classes (equivalent to HKCR for this user).
$clsid = "HKCU:\Software\Classes\CLSID\$guid"
if (-not (Test-Path $clsid)) { New-Item -Path $clsid -Force | Out-Null }
New-Item -Path "$clsid\InprocServer32" -Force | Out-Null
Set-ItemProperty -Path "$clsid\InprocServer32" -Name "(default)" -Value $comhost
Set-ItemProperty -Path "$clsid\InprocServer32" -Name "ThreadingModel" -Value "Both"
Set-ItemProperty -Path $clsid -Name "(default)" -Value $name
Set-ItemProperty -Path $clsid -Name "InfoTip" -Value $tooltip

$shellFolder = "$clsid\ShellFolder"
if (-not (Test-Path $shellFolder)) { New-Item -Path $shellFolder -Force | Out-Null }
# Attributes: SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_SLOW
Set-ItemProperty -Path $shellFolder -Name "Attributes" -Value 0xA8004000 -Type DWord

# 2. Junction point under This PC (MyComputer), HKCU variant.
$junction = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$guid"
if (-not (Test-Path $junction)) { New-Item -Path $junction -Force | Out-Null }
Set-ItemProperty -Path $junction -Name "(default)" -Value $name

Write-Host ""
Write-Host "Registered (current user). Restart Explorer to see 'Servers' under This PC:"
Write-Host "    taskkill /f /im explorer.exe"
Write-Host "    start explorer.exe"
