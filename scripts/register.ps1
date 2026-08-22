# Register ExplorerRemoteFs as an Explorer namespace extension.
# Usage:  powershell -ExecutionPolicy Bypass -File register.ps1 [-BinDir <path>]
# Must run as Administrator.

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

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "This script must run as Administrator."
}

Write-Host "Registering comhost: $comhost"
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s `"$comhost`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }

# Enable SharpShell file logging (diagnostics for namespace view issues).
$sharpShellCfg = "HKLM:\Software\SharpShell"
if (-not (Test-Path $sharpShellCfg)) { New-Item -Path $sharpShellCfg -Force | Out-Null }
Set-ItemProperty -Path $sharpShellCfg -Name "LoggingMode" -Value 2 -Type DWord
Set-ItemProperty -Path $sharpShellCfg -Name "LogPath" -Value "$env:ProgramData\ExplorerRemoteFs\sharpshell.log"

# Class registration: default name, tooltip, ShellFolder attributes.
$clsid = "HKLM:\Software\Classes\CLSID\$guid"
if (-not (Test-Path $clsid)) { New-Item -Path $clsid -Force | Out-Null }
Set-ItemProperty -Path $clsid -Name "(default)" -Value $name
Set-ItemProperty -Path $clsid -Name "InfoTip" -Value $tooltip

$shellFolder = "$clsid\ShellFolder"
if (-not (Test-Path $shellFolder)) { New-Item -Path $shellFolder -Force | Out-Null }
# Attributes: SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_SLOW
Set-ItemProperty -Path $shellFolder -Name "Attributes" -Value 0xA8004000 -Type DWord

# Junction point under This PC (MyComputer).
$junction = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace\$guid"
if (-not (Test-Path $junction)) { New-Item -Path $junction -Force | Out-Null }
Set-ItemProperty -Path $junction -Name "(default)" -Value $name

Write-Host ""
Write-Host "Registered OK. Restart Explorer to see 'Servers' under This PC:"
Write-Host "    taskkill /f /im explorer.exe"
Write-Host "    start explorer.exe"
