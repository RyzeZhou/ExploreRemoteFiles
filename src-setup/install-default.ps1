# Install (or upgrade in place) to the default per-user directory by running the packaged Setup.exe.
# ASCII only: PowerShell 5.1 reads BOM-less files as ANSI.
# Run it detached -- install.ps1 kills explorer.exe on purpose (the extension DLL stays mapped):
#   powershell -NoProfile -Command "Start-Process pwsh -ArgumentList '-NoProfile','-File','src-setup\install-default.ps1' -WindowStyle Hidden"
param([string]$Dist = (Join-Path $PSScriptRoot '..\dist\ExplorerRemoteFs-win-x64'))
$ErrorActionPreference = 'Continue'
$setup = Join-Path $Dist 'Setup.exe'
if (-not (Test-Path $setup)) { throw "missing $setup -- run scripts\build-release.ps1 first" }
$log = Join-Path $PSScriptRoot 'install-default.log'
Set-Content -LiteralPath $log -Value ("### install-default " + (Get-Date -Format 'HH:mm:ss')) -Encoding UTF8
$p = Start-Process -FilePath $setup -ArgumentList '--silent' -PassThru
$deadline = (Get-Date).AddMinutes(10)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
Add-Content -LiteralPath $log -Value ("setup exit=" + $(if ($p.HasExited) { $p.ExitCode } else { 'TIMEOUT' })) -Encoding UTF8
$dir = Join-Path $env:LOCALAPPDATA 'ExplorerRemoteFs'
foreach ($item in @('ExplorerDataProviderFtp.dll', 'Setup.exe', 'Uninstall.exe', 'install.ps1', 'uninstall.ps1', 'cli\ExplorerRemoteFs.Cli.exe', 'client\RemoteFsClient.exe')) {
    Add-Content -LiteralPath $log -Value ("  {0,-40} {1}" -f $item, (Test-Path (Join-Path $dir $item))) -Encoding UTF8
}
$arp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs'
$name = (Get-ItemProperty -Path $arp -Name DisplayName -ErrorAction SilentlyContinue).DisplayName
$cli = (Get-ItemProperty -Path 'HKCU:\Software\ExplorerRemoteFs' -Name CliPath -ErrorAction SilentlyContinue).CliPath
Add-Content -LiteralPath $log -Value ("ARP:  " + $name) -Encoding UTF8
Add-Content -LiteralPath $log -Value ("CliPath: " + $cli) -Encoding UTF8
Add-Content -LiteralPath $log -Value ("client running: " + ($null -ne (Get-Process -Name RemoteFsClient -ErrorAction SilentlyContinue))) -Encoding UTF8
