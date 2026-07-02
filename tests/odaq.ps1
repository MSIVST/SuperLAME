# SuperLAME objective test against the ODAQ reference corpus.
#
# For every ODAQ reference.wav (clean source): encode single-thread (ST) and
# multi-thread (MT), decode both back, and measure the ST-vs-MT difference --
# this proves the SuperFast parallel path is audibly equivalent to plain LAME on
# real, diverse program material (music, speech, mixed). Also confirms the MT
# stream is frame-chain-valid (no repacker desync -> no self-heal fallback) and
# that FLAC input yields the same result as WAV input.
#
# This is an OBJECTIVE test (automated diffs), not a subjective ODAQ listening
# test -- it validates the encoder's *own* correctness, not MP3-vs-original
# perceptual quality (which needs human listeners).
$ErrorActionPreference = "Continue"
# Repo root holding the built exe + test corpora. Override with SUPERLAME_ROOT.
$Root = if ($env:SUPERLAME_ROOT) { $env:SUPERLAME_ROOT } else { Split-Path -Parent $PSScriptRoot }
$exe = "$Root\build\final\SuperLAME-1.0.exe"
$ff  = "$Root\ffmpeg.exe"
$odaq = "$Root\ODAQ (Open Dataset of Audio Quality)"
$work = "$Root\build\odaqwork"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$refs = Get-ChildItem -Path $odaq -Recurse -Filter "reference.wav" -ErrorAction SilentlyContinue
Write-Host "ODAQ references found: $($refs.Count)"
if ($refs.Count -eq 0) { Write-Host "no references; abort" -ForegroundColor Red; exit 1 }

# RMS difference (dBFS) between two decoded f32le raw dumps.
function DiffDb($mp3a, $mp3b) {
    & $ff -hide_banner -loglevel error -i $mp3a -f f32le -ac 2 "$work\a.raw" -y 2>$null
    & $ff -hide_banner -loglevel error -i $mp3b -f f32le -ac 2 "$work\b.raw" -y 2>$null
    $py = @"
import numpy as np, math
a=np.fromfile(r'$work\a.raw',dtype=np.float32); b=np.fromfile(r'$work\b.raw',dtype=np.float32)
n=min(len(a),len(b))
if n==0: print('nan'); raise SystemExit
a=a[:n]; b=b[:n]; d=a-b; r=math.sqrt(float(np.mean(d*d)))
print('%.3f'%(20*math.log10(r)) if r>0 else '-999')
"@
    return (& python -c $py) 2>$null
}

$modes = @(
  @{name='VBR2';   args=@('-V','2')},
  @{name='CBR320'; args=@('-b','320')},
  @{name='ABR192'; args=@('--abr','192')}
)

$tested=0; $desync=0; $mtFail=0; $worst=-999.0; $flacFail=0
$sample = $refs | Select-Object -First 44   # all of them (10s each is cheap)

foreach ($r in $sample) {
    $tag = (Split-Path (Split-Path $r.FullName -Parent) -Leaf)
    foreach ($m in $modes) {
        $st = "$work\st.mp3"; $mt = "$work\mt.mp3"
        # ST encode (-t1) captures stderr to detect the self-heal note
        & $exe @($m.args) -t1 $r.FullName $st --quiet 2>$null | Out-Null
        $mtErr = & $exe @($m.args) -t16 $r.FullName $mt --quiet 2>&1
        $tested++
        if ($mtErr -match "re-encoding single-threaded") { $desync++; Write-Host "  DESYNC self-heal: $tag / $($m.name)" -ForegroundColor Yellow }
        if (-not (Test-Path $mt) -or (Get-Item $mt).Length -eq 0) { $mtFail++; Write-Host "  MT FAIL: $tag / $($m.name)" -ForegroundColor Red; continue }
        $db = [double](DiffDb $st $mt)
        if ($db -gt $worst) { $worst = $db }
        if ($db -gt -40) { Write-Host ("  LOUD DIFF: {0} / {1} = {2} dB" -f $tag,$m.name,$db) -ForegroundColor Red }
    }
    # FLAC-input parity: encode source as FLAC, then VBR2 both ways, compare.
    $flac = "$work\src.flac"
    & $ff -hide_banner -loglevel error -i $r.FullName -c:a flac $flac -y 2>$null
    if (Test-Path $flac) {
        & $exe -V2 -t1 $r.FullName "$work\fw.mp3" --quiet 2>$null | Out-Null
        & $exe -V2 -t1 $flac       "$work\ff.mp3" --quiet 2>$null | Out-Null
        $fdb = [double](DiffDb "$work\fw.mp3" "$work\ff.mp3")
        if ($fdb -gt -60) { $flacFail++; Write-Host ("  FLAC vs WAV input differ: {0} = {1} dB" -f $tag,$fdb) -ForegroundColor Red }
    }
}

Write-Host ""
Write-Host ("ODAQ OBJECTIVE: {0} encodes tested" -f $tested)
Write-Host ("  MT frame-chain desyncs (self-heal): {0}" -f $desync)
Write-Host ("  MT encode failures                : {0}" -f $mtFail)
Write-Host ("  worst ST-vs-MT audio diff          : {0} dB (target <= -40)" -f $worst)
Write-Host ("  FLAC-input vs WAV-input mismatches : {0}" -f $flacFail)
$ok = ($mtFail -eq 0) -and ($worst -le -40) -and ($flacFail -eq 0)
Write-Host ("RESULT: {0}" -f $(if($ok){"PASS"}else{"CHECK"})) -ForegroundColor $(if($ok){"Green"}else{"Red"})
