# Verify that CANCEL really aborts an in-flight transfer.
# FETCHDIR a huge folder (97k files) -> CANCEL mid-flight -> state must become CANCELLED
# and the local file count must stop growing.
param(
    [string]$Site = 'WSL-SFTP',
    [string]$RemoteDir = '/home/zhou/AI_work/test/big-2',
    [string]$Batch = 'cancel-test-batch',
    [string]$Local = "$env:TEMP\erf-cancel-dir"
)

function New-Bridge {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'ExplorerRemoteFs.Bridge.v1', [System.IO.Pipes.PipeDirection]::InOut)
    try { $pipe.Connect(5000) } catch { Write-Output ("BRIDGE CONNECT FAILED: " + $_.Exception.Message); exit 2 }
    $w = New-Object System.IO.StreamWriter($pipe, (New-Object Text.UTF8Encoding($false)))
    $w.AutoFlush = $true
    $r = New-Object System.IO.StreamReader($pipe, (New-Object Text.UTF8Encoding($false)))
    return @{ Pipe = $pipe; W = $w; R = $r }
}

function Get-Status([string]$jobId) {
    $b = New-Bridge
    $b.W.WriteLine('FETCHSTATUS'); $b.W.WriteLine($jobId)
    $s = $b.R.ReadLine()
    $b.Pipe.Dispose()
    return $s
}

function Count-Local([string]$dir) {
    if (-not (Test-Path $dir)) { return 0 }
    return (Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue).Count
}

Remove-Item $Local -Recurse -Force -ErrorAction SilentlyContinue

# --- 1) start the folder download -----------------------------------------
$b = New-Bridge
$b.W.WriteLine('FETCHDIR'); $b.W.WriteLine($Site); $b.W.WriteLine($RemoteDir); $b.W.WriteLine($Local); $b.W.WriteLine($Batch)
$reply = $b.R.ReadLine()
$b.Pipe.Dispose()
Write-Output ("1) FETCHDIR reply = '{0}'" -f $reply)
if (-not $reply.StartsWith('STARTED ')) { Write-Output 'FAIL: no job id'; exit 1 }
$jobId = $reply.Substring(8).Trim()

# --- 2) let it actually run ------------------------------------------------
Start-Sleep -Seconds 6
Write-Output ("2) before cancel: '{0}'" -f (Get-Status $jobId))
$before = Count-Local $Local
Write-Output ("   local files so far: {0}" -f $before)

# --- 3) cancel the batch ---------------------------------------------------
$sw = [Diagnostics.Stopwatch]::StartNew()
$b3 = New-Bridge
$b3.W.WriteLine('CANCEL'); $b3.W.WriteLine($Batch)
$cr = $b3.R.ReadLine()
$b3.Pipe.Dispose()
Write-Output ("3) CANCEL reply = '{0}'  ({1} ms)" -f $cr, $sw.ElapsedMilliseconds)

# --- 4) poll until terminal state -----------------------------------------
$deadline = (Get-Date).AddSeconds(40)
$state = ''
$sw2 = [Diagnostics.Stopwatch]::StartNew()
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 700
    $state = Get-Status $jobId
    if ($state -match '^(DONE|FAILED|CANCELLED|UNKNOWN)') { break }
}
$sw2.Stop()
Write-Output ("4) final state = '{0}'  (after {1} ms)" -f $state, $sw2.ElapsedMilliseconds)

# --- 5) is the local tree still growing? ----------------------------------
$after = Count-Local $Local
Write-Output ("5) local files now: {0}  (was {1})" -f $after, $before)
Start-Sleep -Seconds 4
$later = Count-Local $Local
$verdict = 'stopped (cancel worked)'
if ($later -gt $after) { $verdict = 'STILL GROWING (cancel did NOT work)' }
Write-Output ("   after 4 more seconds: {0}  -> {1}" -f $later, $verdict)

Write-Output '--- rfs-tasks.log tail ---'
Get-Content (Join-Path $env:TEMP 'rfs-tasks.log') -Tail 8 -ErrorAction SilentlyContinue

Remove-Item $Local -Recurse -Force -ErrorAction SilentlyContinue
Write-Output '(cleaned local test dir)'
