# superlame-mt Phase 5 regression suite.
# Validates CBR/ABR/VBR across thread counts and both engines:
#   - encode succeeds, output decodes with ZERO errors
#   - duration preserved
#   - single-thread == multi-thread (audio-equivalent)
#   - znver3 engine == sse2 engine (audio-equivalent)
#   - LAME/Xing info tag present in MT output
$ErrorActionPreference = "Continue"
# Repo root holding the built exe + test corpora. Override with SUPERLAME_ROOT.
$Root = if ($env:SUPERLAME_ROOT) { $env:SUPERLAME_ROOT } else { Split-Path -Parent $PSScriptRoot }
$exe = "$Root\build\final\superlame-mt.exe"
$ff  = "$Root\ffmpeg.exe"
$fp  = "$Root\ffprobe.exe"
$work = "$Root\build\regress"
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

Write-Host "`n=== F) Tiny-input MT robustness (flush/overlap regression) ===" -ForegroundColor Cyan
# Inputs holding 1..overlap-1 full MP3 frames used to corrupt the heap in MT
# mode (EncodeFrames' flush accounting went negative). 1152/2304/3456 samples
# cover the MPEG-1 window (overlap 4); 2880 @ 22.05 kHz covers MPEG-2 (overlap 8).
$tinyCases = @(
    @{n="1frame@44k";  rate=44100; samples=1152},
    @{n="2frames@44k"; rate=44100; samples=2304},
    @{n="3frames@44k"; rate=44100; samples=3456},
    @{n="5frames@22k"; rate=22050; samples=2880}
)
foreach ($t in $tinyCases) {
    $w = "$work\tiny_$($t.n).wav"
    & $ff -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=$($t.rate)" `
        -af "atrim=end_sample=$($t.samples)" -ac 2 -c:a pcm_s16le $w -y 2>$null
    $o = "$work\tiny_$($t.n).mp3"
    & $exe --quiet -t 8 $w $o 2>$null | Out-Null
    $ok = ($LASTEXITCODE -eq 0) -and (Test-Path $o) -and ((DecodeErrors $o) -eq 0)
    Check "$($t.n): MT encode of $($t.samples)-sample input succeeds + decodes clean" $ok
}

Write-Host "`n=== G) Non-integer-ratio resample seam integrity ===" -ForegroundColor Cyan
# The chunked r8brain resampler must align every chunk's output grid exactly.
# Misalignment (any non-integer ratio, e.g. 44.1k->48k) shifts whole chunks by
# a sub-sample offset: ~-30 dB RMS vs a reference resample. Fixed builds read
# encoder noise only (~-95 dB on a pure tone at CBR 320).
$tone8 = "$work\tone8s_44k.wav"
& $ff -hide_banner -loglevel error -f lavfi -i "sine=frequency=997:sample_rate=44100:duration=8" `
    -ac 2 -c:a pcm_s16le $tone8 -y 2>$null
$o48   = "$work\seam_48.mp3"
& $exe --quiet -t 8 -b 320 --resample 48 $tone8 $o48 2>$null | Out-Null
$ref48 = "$work\seam_ref48.wav"
& $ff -hide_banner -loglevel error -i $tone8 -af "aresample=48000" -c:a pcm_s16le $ref48 -y 2>$null
$d = DiffRMS $o48 $ref48
$val = if ($d -eq "-inf") { -999 } else { [double]$d }
Check "44.1k->48k MT resample matches reference <= -50dB (got $d dB)" ($val -le -50)

Write-Host "`n=== H) Auto-rate decision honors channel count ===" -ForegroundColor Cyan
# LAME keeps mono at 44.1 kHz at CBR 96 but downsamples stereo to 32 kHz; the
# probe must see the real channel count (a defaulted-stereo probe downsampled
# mono files needlessly).
function OutRate($mp3) { return [int](& $fp -hide_banner -loglevel error -show_entries stream=sample_rate -of csv=p=0 $mp3 2>$null) }
$om = "$work\rate_mono96.mp3"; $os = "$work\rate_stereo96.mp3"
& $exe --quiet -t 8 -b 96 $wavs["mono"]    $om 2>$null | Out-Null
& $exe --quiet -t 8 -b 96 $wavs["complex"] $os 2>$null | Out-Null
Check "mono   CBR96 keeps 44100 Hz (got $(OutRate $om))"        ((OutRate $om) -eq 44100)
Check "stereo CBR96 downsamples to 32000 Hz (got $(OutRate $os))" ((OutRate $os) -eq 32000)

Write-Host "`n=== I) --bench mode (null sink, dual clocks, repeat aggregate) ===" -ForegroundColor Cyan
# Guardrail under test: every bench line must carry BOTH labeled clocks
# (play/wall + play/cpu) -- never a bare per-core "x" (BENCHPLAN.md).
$benchOut = "$work\bench_none.mp3"
if (Test-Path $benchOut) { Remove-Item $benchOut }
$b1 = (& $exe --bench -b 128 -t 4 $wavs["complex"] 2>&1 | Out-String)
Check "--bench: exit 0, no outfile written" (($LASTEXITCODE -eq 0) -and -not (Test-Path $benchOut))
Check "--bench: line has play/wall + play/cpu + par-eff" `
    (($b1 -match "play/wall=\d+x") -and ($b1 -match "play/cpu=[\d.]+x") -and ($b1 -match "par-eff="))
Check "--bench: stage split present" ($b1 -match "bench: stages\s+read=.*encode.*total=")

$b3 = (& $exe --bench=3 -b 128 -t 4 $wavs["complex"] 2>&1 | Out-String)
Check "--bench=3: aggregate wall[min med mean] + warmup discard" `
    (($b3 -match "wall\[min [\d.]+ med [\d.]+ mean [\d.]+\]") -and ($b3 -match "warmup, discarded"))

# bench-and-keep: giving an outfile must produce the identical bytes to a
# normal encode of the same config.
$bk = "$work\bench_keep.mp3"; $nk = "$work\bench_norm.mp3"
& $exe --quiet --bench -b 128 -t 4 $wavs["complex"] $bk 2>$null | Out-Null
& $exe --quiet -b 128 -t 4 $wavs["complex"] $nk 2>$null | Out-Null
$same = (Get-FileHash $bk -Algorithm SHA256).Hash -eq (Get-FileHash $nk -Algorithm SHA256).Hash
Check "--bench outfile == normal encode (byte-identical)" $same

& $exe --bench=0 -b 128 $wavs["complex"] 2>$null | Out-Null
Check "--bench=0 rejected (exit 1)" ($LASTEXITCODE -eq 1)

# Pipe input: read stage must be labeled n/a (it would time the producer).
$bp = (cmd /c "type `"$($wavs["complex"])`" | `"$exe`" --bench -b 128 -t 4 - 2>&1" | Out-String)
Check "--bench from pipe: read labeled n/a (pipe)" ($bp -match "read=n/a \(pipe\)")

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host ("RESULT: $pass passed, $fail failed") -ForegroundColor $(if ($fail -eq 0) {"Green"} else {"Red"})
exit $fail
