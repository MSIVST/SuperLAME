# SuperFast core-scaling benchmark: encode a long real file at increasing thread
# counts, measure wall time + speedup. Uses the 2L DXD file (202s) so the work
# is large enough for clean measurements. Best-of-3 per thread count.
# Repo root holding the built exe + test corpora. Override with SUPERLAME_ROOT.
$Root = if ($env:SUPERLAME_ROOT) { $env:SUPERLAME_ROOT } else { Split-Path -Parent $PSScriptRoot }
$mt = "$Root\build\final\superlame-mt.exe"
$in = "$Root\2L-077-stereo-DXD_21.wav"
$d  = "$Root\build\2ltest"
$audioSec = 202.16

Write-Host "SuperFast core scaling (2L-077 DXD, 202s, -V2, best of 3):`n"
$base = $null
foreach ($t in 1,2,4,6,8,12,16) {
    $times = @()
    for ($i=0; $i -lt 3; $i++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $mt -V2 -t$t $in "$d\scale.mp3" --quiet 2>$null | Out-Null
        $sw.Stop(); $times += $sw.ElapsedMilliseconds
    }
    $best = ($times | Measure-Object -Minimum).Minimum
    if ($null -eq $base) { $base = $best }
    $speedup = [math]::Round($base / $best, 2)
    $rt = [math]::Round($audioSec * 1000 / $best, 0)
    $eff = [math]::Round($speedup / $t * 100, 0)
    Write-Host ("  -t{0,-2}  {1,6} ms   {2,5}x speedup   {3,4}x realtime   {4,3}% efficiency" -f $t, $best, $speedup, $rt, $eff)
}
Write-Host "`n(speedup vs -t1; efficiency = speedup/threads. Host: Ryzen 7 5800X3D, 8C/16T)"
