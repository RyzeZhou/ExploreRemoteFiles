<#
  ERF 状态诊断 / 洁净清理

  默认只诊断，不改任何东西：
      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\clean-state.ps1

  确认要清理时再加 -Clean（会先自动备份注册表到桌面）：
      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\clean-state.ps1 -Clean

  ⚠ 清理后**必须重启系统**：安装包默认不结束 explorer.exe，内存里可能还映射着旧版 DLL，
    不重启的话下一次安装仍可能跑在旧代码上（这正是"改了没效果/行为混乱"的常见原因）。

  ⚠ 脚本**不碰站点配置与凭据**（%APPDATA%\ExplorerRemoteFs 下的站点/密码），只清注册项、
    安装目录、缓存与日志。要连配置一起清就加 -PurgeUserData。
#>
param(
    [switch]$Clean,
    [switch]$PurgeUserData
)

$ErrorActionPreference = 'Continue'

$Clsid = [ordered]@{
    NsFolder = '{C816CE0E-728C-4FC9-98E5-D0B35B384597}'   # 命名空间（易远传）
    Ctx      = '{CB8F539D-3B97-4473-9E07-C8248C53248E}'   # 右键菜单
    Props    = '{5DD84779-FEF1-46A3-8FCF-9F1A9603BB8F}'   # 属性页
    Protocol = '{A970407D-FE36-4C49-A433-61E605D9DDEA}'   # erf: 协议处理器
}
$InstallDirFallback = 'D:\Program\ExplorerRemoteFs'
# 安装目录不写死：从四个 CLSID 的 InprocServer32 反推（四者应一致），取不到才回退旧路径。
$InstallDir = $InstallDirFallback
$fromReg = @()
foreach ($c in $Clsid.Values) {
    $v = (Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\CLSID\$c\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
    if ($v) { $fromReg += Split-Path -Parent $v }
}
$fromReg = @($fromReg | Sort-Object -Unique)
if ($fromReg.Count -eq 1) { $InstallDir = $fromReg[0] }
elseif ($fromReg.Count -gt 1) { Write-Host "  注意：四个 CLSID 指向不同目录：$($fromReg -join ' ; ')" -ForegroundColor Yellow }
$RegKeys = @(
    'HKCU:\Software\Classes\erf',
    'HKCU:\Software\Classes\RemoteFsMicrosoftCoreType',
    'HKCU:\Software\Classes\RemoteFsFileType',
    'HKCU:\Software\ExplorerRemoteFs',
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$($Clsid.NsFolder)"
)
foreach ($k in $Clsid.Values) { $RegKeys += "HKCU:\Software\Classes\CLSID\$k" }

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

Section '1. explorer.exe 里正在运行的我们的 DLL（最关键）'
$loaded = @()
Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object {
    $p = $_
    $p.Modules | Where-Object { $_.ModuleName -match 'ExplorerDataProvider|RemoteFsShell' } | ForEach-Object {
        $f = $null; try { $f = Get-Item $_.FileName -ErrorAction SilentlyContinue } catch {}
        $loaded += [pscustomobject]@{
            Pid       = $p.Id
            Loaded    = $_.FileName
            DiskTime  = if ($f) { $f.LastWriteTime } else { $null }
            DiskSize  = if ($f) { $f.Length } else { $null }
        }
    }
}
if ($loaded) { $loaded | Format-List } else { Write-Host '  （explorer 未加载我们的 DLL）' }

Section '2. 安装目录里的 DLL（与上面比对时间戳）'
if (Test-Path $InstallDir) {
    Get-ChildItem $InstallDir -Recurse -Include *.dll,*.old -ErrorAction SilentlyContinue |
        Select-Object @{n='File';e={$_.FullName}}, Length, LastWriteTime | Format-Table -AutoSize
} else { Write-Host "  （$InstallDir 不存在）" }

Section '3. 相关进程'
Get-Process RemoteFsClient, ExplorerRemoteFs.Cli -ErrorAction SilentlyContinue |
    Select-Object Name, Id, StartTime, Responding | Format-Table -AutoSize

Section '4. 注册项残留'
foreach ($k in $RegKeys) {
    if (Test-Path $k) { Write-Host "  存在   $k" -ForegroundColor Yellow } else { Write-Host "  无     $k" }
}
$de = (Get-ItemProperty 'HKCU:\Software\Classes\erf\shell\open\command' -Name DelegateExecute -ErrorAction SilentlyContinue).DelegateExecute
Write-Host "  DelegateExecute = $(if ($de) { $de } else { '(无)' })"
$cmd = (Get-Item 'HKCU:\Software\Classes\erf\shell\open\command' -ErrorAction SilentlyContinue).GetValue('')
Write-Host "  erf command     = $(if ($cmd) { $cmd } else { '(无)' })"

Section '5. 数据 / 缓存 / 日志'
foreach ($d in @("$env:LOCALAPPDATA\ExplorerRemoteFs", "$env:APPDATA\ExplorerRemoteFs")) {
    if (Test-Path $d) {
        $sz = (Get-ChildItem $d -Recurse -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        Write-Host ("  {0}  ({1:N1} MB)" -f $d, ($sz / 1MB))
    } else { Write-Host "  无     $d" }
}

if (-not $Clean) {
    Write-Host "`n（以上仅为诊断。要清理请加 -Clean，清理前会自动备份注册表到桌面。）" -ForegroundColor Green
    exit 0
}

Section '清理'
$backup = Join-Path ([Environment]::GetFolderPath('Desktop')) ("erf-registry-backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $backup | Out-Null
foreach ($k in $RegKeys) {
    if (Test-Path $k) {
        $safe = ($k -replace '[:\]', '_')
        & reg export ($k -replace '^HKCU:', 'HKCU') (Join-Path $backup "$safe.reg") /y 2>&1 | Out-Null
    }
}
Write-Host "  注册表已备份到 $backup"

Get-Process RemoteFsClient, ExplorerRemoteFs.Cli -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Write-Host '  已停止常驻服务 / CLI'

foreach ($k in $RegKeys) {
    if (Test-Path $k) { Remove-Item -Recurse -Force $k -ErrorAction SilentlyContinue; Write-Host "  已删 $k" }
}

if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
    Write-Host "  已删 $InstallDir（删不掉说明 explorer 仍映射着 DLL —— 重启后再删一次）"
}
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\ExplorerRemoteFs" -ErrorAction SilentlyContinue
Remove-Item -Force "$env:TEMP\rfs-*.log","$env:TEMP\erf-*.log","$env:TEMP\remotefs-*.log" -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "$env:TEMP\rfs-dataobj" -ErrorAction SilentlyContinue
if ($PurgeUserData) {
    Remove-Item -Recurse -Force "$env:APPDATA\ExplorerRemoteFs" -ErrorAction SilentlyContinue
    Write-Host '  已删站点配置与凭据（-PurgeUserData）'
} else {
    Write-Host '  保留站点配置与凭据（%APPDATA%\ExplorerRemoteFs）'
}

Write-Host "`n完成。**请重启系统**，然后重新安装，确保 explorer 不会继续跑旧 DLL。" -ForegroundColor Yellow
