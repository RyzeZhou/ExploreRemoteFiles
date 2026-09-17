# ExplorerRemoteFs installer cycle test: install -> verify -> uninstall -> verify.
# ASCII only on purpose (PowerShell 5.1 reads BOM-less files as ANSI; Chinese comments
# in a BOM-less script have already broken one release build).
#
# install.ps1/uninstall.ps1 kill explorer.exe on purpose (the extension DLL stays mapped
# otherwise), which can take the calling console down with it -- so run this detached and
# watch the log file:
#   powershell -NoProfile -Command "Start-Process pwsh -ArgumentList '-NoProfile','-File','<this>' -WindowStyle Hidden"
# (the script writes its own log next to itself; Write-Host is not reliable under redirection)
param(
    [string]$Dist = (Join-Path $PSScriptRoot '..\dist\ExplorerRemoteFs-win-x64'),
    [string]$TestDir = "$env:LOCALAPPDATA\ExplorerRemoteFs-Test",
    [string]$LogFile = (Join-Path $PSScriptRoot 'cycle-test.log')
)
$ErrorActionPreference = 'Continue'
$fails = 0
Set-Content -LiteralPath $LogFile -Value ("### cycle-test " + (Get-Date -Format 'HH:mm:ss') + " dist=$Dist test=$TestDir") -Encoding UTF8

function Log([string]$text) {
    Add-Content -LiteralPath $LogFile -Value $text -Encoding UTF8
}
function Check([string]$name, $ok, $detail) {
    if ($ok) { Log ("PASS  " + $name + "  " + $detail) }
    else { Log ("FAIL  " + $name + "  " + $detail); $script:fails++ }
}
function RegValue([string]$path, [string]$name) {
    try { return (Get-ItemProperty -Path $path -Name $name -ErrorAction Stop).$name } catch { return $null }
}
function RegDefault([string]$path) {
    # NOTE: `Get-ItemProperty -Name ''` does NOT read the default value (it throws), which
    # silently turned every "default value" assertion into a false FAIL. Use the registry API.
    try { return (Get-Item -LiteralPath $path -ErrorAction Stop).GetValue('') } catch { return $null }
}
function RunExe([string]$exe, [string[]]$exeArgs) {
    # Setup.exe/uninstaller exit codes: 0 ok, 2 user cancelled, 3 failure (see ErfSetup.cpp)
    #
    # Do NOT use `Start-Process -Wait`: PowerShell 7 waits for the whole process TREE, and
    # install.ps1 starts the resident RemoteFsClient which by design never exits -- the
    # first version of this test hung here for minutes with no output. Poll the process
    # object instead (measured 2026-09-17).
    #
    # stdout/stderr are captured: in --silent mode the installer's warnings (a failed
    # CopyFileW, for instance) are printed, never shown in a dialog, so losing them hides
    # real bugs.
    $outFile = Join-Path $PSScriptRoot ((Split-Path -Leaf $exe) + '.console.log')
    $setupLog = Join-Path $env:TEMP 'erf-setup.log'
    Remove-Item -LiteralPath $setupLog -Force -ErrorAction SilentlyContinue
    try {
        $p = Start-Process -FilePath $exe -ArgumentList $exeArgs -PassThru `
            -RedirectStandardOutput $outFile -RedirectStandardError ($outFile + '.err')
        $deadline = (Get-Date).AddMinutes(10)
        while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
        if (-not $p.HasExited) { Log ("   TIMEOUT after 10 min: " + $exe); return -1 }
        if (Test-Path $setupLog) {
            Log '   --- %TEMP%\erf-setup.log ---'
            Get-Content -LiteralPath $setupLog -Encoding UTF8 | ForEach-Object { Log ('   ' + $_) }
        }
        foreach ($f in @($outFile, ($outFile + '.err'))) {
            if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
                Log ("   --- " + (Split-Path -Leaf $f) + " ---")
                Get-Content -LiteralPath $f | ForEach-Object { Log ("   " + $_) }
            }
        }
        Log ("   exit=" + $p.ExitCode + "  (" + (Split-Path -Leaf $exe) + ")")
        return $p.ExitCode
    } catch { Log ("   launch error: " + $_.Exception.Message); return -1 }
}

$folder = '{C816CE0E-728C-4FC9-98E5-D0B35B384597}'
$clsids = @('{C816CE0E-728C-4FC9-98E5-D0B35B384597}', '{CB8F539D-3B97-4473-9E07-C8248C53248E}', '{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}')
$arp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\ExplorerRemoteFs'
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

Log '=== phase 1: install'
if (Test-Path $TestDir) { Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue }
Stop-Process -Name RemoteFsClient -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400
$code = RunExe (Join-Path $Dist 'Setup.exe') @('--silent', '--dir', $TestDir)
Check 'setup exit code 0' ($code -eq 0) ("exit=" + $code)

Check 'client exe' (Test-Path "$TestDir\client\RemoteFsClient.exe") "$TestDir\client\RemoteFsClient.exe"
Check 'cli exe' (Test-Path "$TestDir\cli\ExplorerRemoteFs.Cli.exe") "$TestDir\cli\ExplorerRemoteFs.Cli.exe"
Check 'extension dll' (Test-Path "$TestDir\ExplorerDataProviderFtp.dll") 'ExplorerDataProviderFtp.dll'
Check 'Uninstall.exe payload' (Test-Path "$TestDir\Uninstall.exe") 'Uninstall.exe'
Check 'install.ps1 payload' (Test-Path "$TestDir\install.ps1") 'install.ps1'
Check 'uninstall.ps1 payload' (Test-Path "$TestDir\uninstall.ps1") 'uninstall.ps1'
$srcDll = Join-Path $Dist 'ExplorerDataProviderFtp.dll'
if ((Test-Path "$TestDir\ExplorerDataProviderFtp.dll") -and (Test-Path $srcDll)) {
    $a = (Get-Item "$TestDir\ExplorerDataProviderFtp.dll").Length
    $b = (Get-Item $srcDll).Length
    Check 'dll size matches payload' ($a -eq $b) ("installed=$a payload=$b")
}

Check 'ARP DisplayName' ($null -ne (RegValue $arp 'DisplayName')) (RegValue $arp 'DisplayName')
Check 'ARP DisplayVersion' ($null -ne (RegValue $arp 'DisplayVersion')) (RegValue $arp 'DisplayVersion')
Check 'ARP EstimatedSize' ($null -ne (RegValue $arp 'EstimatedSize')) (RegValue $arp 'EstimatedSize')
$unstr = RegValue $arp 'UninstallString'
Check 'ARP UninstallString set' ($null -ne $unstr) $unstr
if ($unstr) {
    $unexe = ($unstr -replace '"', '').Trim()
    $unexe = $unexe.Substring(0, $unexe.IndexOf('.exe') + 4)
    Check 'UninstallString target exists' (Test-Path $unexe) $unexe
}
Check 'Run entry' ($null -ne (RegValue $run 'ExplorerRemoteFs')) (RegValue $run 'ExplorerRemoteFs')
$cliPath = RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath'
Check 'CliPath points into install dir' ($cliPath -like "$TestDir*") $cliPath
foreach ($c in $clsids) {
    $v = RegDefault "HKCU:\Software\Classes\CLSID\$c\InprocServer32"
    Check ("InprocServer32 " + $c.Substring(0, 9)) ($v -like "$TestDir*") $v
}
Check 'erf protocol handler' ($null -ne (RegDefault 'HKCU:\Software\Classes\erf\shell\open\command')) 'erf://'
Check 'namespace folder key' ($null -ne (RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder")) 'Desktop\NameSpace'
Check 'ARP InstallLocation' ((RegValue $arp 'InstallLocation') -eq $TestDir) (RegValue $arp 'InstallLocation')
Check 'ARP DisplayIcon' ((RegValue $arp 'DisplayIcon') -ne $null) (RegValue $arp 'DisplayIcon')
Check 'resident client started' ($null -ne (Get-Process -Name RemoteFsClient -ErrorAction SilentlyContinue)) 'RemoteFsClient.exe'

Log '=== phase 2: uninstall'
$code2 = RunExe (Join-Path $TestDir 'Uninstall.exe') @('--silent')
Check 'uninstall exit code 0' ($code2 -eq 0) ("exit=" + $code2)
# 卸载器是"自杀式"清理：先把自己复制到 %TEMP%，等自己退出后再删安装目录，
# 所以固定 sleep 500ms 一定太早（实测会假报"目录还在"）。轮询到 20 秒。
for ($i = 0; $i -lt 40 -and (Test-Path $TestDir); $i++) { Start-Sleep -Milliseconds 500 }
Check 'install dir removed' (-not (Test-Path $TestDir)) $TestDir
if (Test-Path $TestDir) {
    $left = Get-ChildItem $TestDir -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 5
    foreach ($f in $left) { Log ("   leftover: " + $f.FullName) }
}
Check 'ARP removed' ($null -eq (RegValue $arp 'DisplayName')) 'ARP key'
Check 'Run entry removed' ($null -eq (RegValue $run 'ExplorerRemoteFs')) 'Run'
Check 'CliPath removed' ($null -eq (RegValue 'HKCU:\Software\ExplorerRemoteFs' 'CliPath')) 'CliPath'
foreach ($c in $clsids) {
    Check ("CLSID removed " + $c.Substring(0, 9)) ($null -eq (RegDefault "HKCU:\Software\Classes\CLSID\$c\InprocServer32")) $c
}
Check 'erf protocol removed' ($null -eq (RegDefault 'HKCU:\Software\Classes\erf\shell\open\command')) 'erf://'
Check 'context menu progid removed' ($null -eq (RegDefault 'HKCU:\Software\Classes\RemoteFsMicrosoftCoreType\shellex\ContextMenuHandlers\{CB8F539D-3B97-4473-9E07-C8248C53248E}')) 'RemoteFsMicrosoftCoreType'
Check 'namespace key removed' ($null -eq (RegDefault "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$folder")) 'Desktop\NameSpace'
Check 'site config kept' (Test-Path "$env:APPDATA\ExplorerRemoteFs") "$env:APPDATA\ExplorerRemoteFs"

Log ("=== " + $(if ($fails -eq 0) { 'ALL PASS' } else { "$fails FAILED" }))
exit $fails
