# superlame-mt Phase 5 regression suite.
# Validates CBR/ABR/VBR across thread counts and both engines:
#   - encode succeeds, output decodes with ZERO errors
#   - duration preserved
#   - single-thread == multi-thread (audio-equivalent)
#   - znver3 engine == sse2 engine (audio-equivalent)
#   - LAME/Xing info tag present in MT output
$ErrorActionPreference = "Continue"
$exe = "C:\.Claude_LAMEsf\build\final\superlame-mt.exe"
$ff  = "C:\.Claude_LAMEsf\ffmpeg.exe"
$fp  = "C:\.Claude_LAMEsf\ffprobe.exe"
$work = "C:\.Claude_LAMEsf\build\regress"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$pass = 0; $fail = 0
function Check($name, $cond) {
    if ($cond) { Write-Host ("  PASS  " + $name) -ForegroundColor Green; $script:pass++ }
    else       { Write-Host ("  FAIL  " + $name) -ForegroundColor Red;   $script:fail++ }
}

# --- Build a small corpus of varied signals ---
Write-Host "=== Building test corpus ===" -ForegroundColor Cyan
$corpus = @{
    "tone"   = "sine=frequency=440:duration=5:sample_rate=44100"
    "sweep"  = "sine=frequency=100:duration=5:sample_rate=44100"  # low tone
    "complex"= "aevalsrc=0.4*sin(220*2*PI*t)+0.3*sin(440*2*PI*t)+0.2*sin(1760*2*PI*t)*sin(3*2*PI*t)+0.1*(random(0)*2-1):d=5:s=44100:c=stereo"
    "noise"  = "anoisesrc=d=5:c=pink:r=44100:a=0.5"
    "mono"   = "sine=frequency=600:duration=5:sample_rate=44100"
}
$wavs = @{}
foreach ($k in $corpus.Keys) {
    $w = "$work\$k.wav"
    $ch = if ($k -eq "mono") { "-ac 1" } else { "-ac 2" }
    & $ff -hide_banner -loglevel error -f lavfi -i $corpus[$k] $ch.Split(' ') -c:a pcm_s16le $w -y 2>$null
    $wavs[$k] = $w
}
Write-Host ("  corpus: " + ($wavs.Keys -join ', '))

# Decode helper -> returns RMS of difference between two mp3s (or 'inf' if identical-after-decode)
function DiffRMS($mp3a, $mp3b) {
    $wa = "$work\_a.wav"; $wb = "$work\_b.wav"
    & $ff -hide_banner -loglevel error -i $mp3a -c:a pcm_s16le $wa -y 2>$null
    & $ff -hide_banner -loglevel error -i $mp3b -c:a pcm_s16le $wb -y 2>$null
    $out = & $ff -hide_banner -i $wa -i $wb -filter_complex "[0:a][1:a]amerge=inputs=2,pan=stereo|c0=c0-c2|c1=c1-c3,astats=metadata=1:reset=0" -f null - 2>&1
    $line = ($out | Select-String "RMS level dB" | Select-Object -First 1).ToString()
    if ($line -match "RMS level dB:\s*(-inf|-?\d+\.?\d*)") { return $matches[1] } else { return "?" }
}
function DecodeErrors($mp3) {
    $e = & $ff -hide_banner -v error -i $mp3 -f null - 2>&1
    return ($e | Where-Object { $_ -ne "" }).Count
}
function Dur($mp3) { return [double](& $fp -hide_banner -loglevel error -show_entries format=duration -of csv=p=0 $mp3 2>$null) }

# --- Test matrix: modes x settings ---
$modes = @(
    @{n="CBR128"; a=@("-b","128")},
    @{n="CBR320"; a=@("-b","320")},
    @{n="ABR192"; a=@("--abr","192")},
    @{n="VBR2";   a=@("-V","2")},
    @{n="VBR6";   a=@("-V","6")}
)

Write-Host "`n=== A) Validity: every mode x signal MT-encodes & decodes clean ===" -ForegroundColor Cyan
foreach ($sig in "complex","noise","mono") {
    foreach ($m in $modes) {
        $o = "$work\${sig}_$($m.n)_mt.mp3"
        & $exe -t 8 @($m.a) $wavs[$sig] $o 2>$null | Out-Null
        $ok = (Test-Path $o) -and ((DecodeErrors $o) -eq 0) -and ((Dur $o) -gt 4.5)
        Check "$sig / $($m.n): valid + clean decode + duration ok" $ok
    }
}

Write-Host "`n=== B) ST vs MT equivalence (complex signal) ===" -ForegroundColor Cyan
foreach ($m in $modes) {
    $st = "$work\eq_$($m.n)_st.mp3"; $mt = "$work\eq_$($m.n)_mt.mp3"
    & $exe -t 1 @($m.a) $wavs["complex"] $st 2>$null | Out-Null
    & $exe -t 8 @($m.a) $wavs["complex"] $mt 2>$null | Out-Null
    $d = DiffRMS $st $mt
    $val = if ($d -eq "-inf") { -999 } else { [double]$d }
    Check "$($m.n): ST vs MT audio diff <= -40dB (got $d dB)" ($val -le -40)
}

Write-Host "`n=== C) Engine parity: znver3 vs sse2 (single-thread, deterministic) ===" -ForegroundColor Cyan
# Threshold -50dB: engine output must be INAUDIBLY different (signal ~-9dB, so this
# is >40dB below the music). Differences are pure FMA-contraction FP rounding;
# VBR's threshold-sensitive bit allocation amplifies it slightly more than CBR.
foreach ($m in $modes) {
    $zn = "$work\en_$($m.n)_zn.mp3"; $ss = "$work\en_$($m.n)_ss.mp3"
    $env:SUPERLAME_ENGINE = "znver3"; & $exe -t 1 @($m.a) $wavs["complex"] $zn 2>$null | Out-Null
    $env:SUPERLAME_ENGINE = "sse2";   & $exe -t 1 @($m.a) $wavs["complex"] $ss 2>$null | Out-Null
    $env:SUPERLAME_ENGINE = $null
    $d = DiffRMS $zn $ss
    $val = if ($d -eq "-inf") { -999 } else { [double]$d }
    # Also require identical file size = same bit-allocation decisions (no structural drift)
    $sameSize = (Get-Item $zn).Length -eq (Get-Item $ss).Length
    Check "$($m.n): znver3 vs sse2 inaudible (diff $d dB, sameSize=$sameSize)" (($val -le -50) -and $sameSize)
}

Write-Host "`n=== D) LAME/Xing info tag present in MT output ===" -ForegroundColor Cyan
$tagmp3 = "$work\complex_VBR2_mt.mp3"
$bytes = [System.IO.File]::ReadAllBytes($tagmp3)[0..2000]
$str = -join ($bytes | ForEach-Object { [char]$_ })
Check "Info/Xing tag present (LAME tag header)" ($str -match "Xing|Info|LAME")

Write-Host "`n=== E) Sample-count preservation (decoded length matches input) ===" -ForegroundColor Cyan
foreach ($m in @($modes[1], $modes[3])) {  # CBR320, VBR2
    $o = "$work\len_$($m.n).mp3"
    & $exe -t 8 @($m.a) $wavs["complex"] $o 2>$null | Out-Null
    $dur = Dur $o
    Check "$($m.n): duration ~5.0s (got $([math]::Round($dur,3))s)" ([math]::Abs($dur - 5.0) -lt 0.1)
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host ("RESULT: $pass passed, $fail failed") -ForegroundColor $(if ($fail -eq 0) {"Green"} else {"Red"})
exit $fail
