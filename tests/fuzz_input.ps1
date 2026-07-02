# Input fuzzer: throw malformed / truncated / random files at the parsers
# (WAV read, AIFF read, MP3 --decode) and check the program FAILS CLEANLY
# (exit 1, no crash/hang) rather than crashing (negative/huge exit = SEH crash).
$ErrorActionPreference = "Continue"
$exe = "C:\.Claude_LAMEsf\build\final\superlame-mt.exe"
$ff  = "C:\.Claude_LAMEsf\ffmpeg.exe"
$d   = "C:\.Claude_LAMEsf\build\fuzzin"
New-Item -ItemType Directory -Force -Path $d | Out-Null

# A valid seed WAV + MP3 to mutate from.
& $ff -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -ac 2 -c:a pcm_s16le "$d\seed.wav" -y 2>$null
& $ff -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -ac 2 -c:a pcm_s24be "$d\seed.aiff" -y 2>$null
& $ff -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=1:sample_rate=44100" -ac 2 -c:a flac "$d\seed.flac" -y 2>$null
& $exe -b 128 "$d\seed.wav" "$d\seed.mp3" --quiet 2>$null

function IsCrash($code) {
    # exit 1 = clean error; 0 = success. Large/negative = SEH crash (0xC0000005 etc).
    return ($code -ne 0 -and $code -ne 1)
}

$seedWav  = [System.IO.File]::ReadAllBytes("$d\seed.wav")
$seedAif  = [System.IO.File]::ReadAllBytes("$d\seed.aiff")
$seedMp3  = [System.IO.File]::ReadAllBytes("$d\seed.mp3")
$seedFlac = [System.IO.File]::ReadAllBytes("$d\seed.flac")
$rng = New-Object System.Random 12345

$total=0; $crashes=0; $crashFiles=@()

function RunCase($bytes, $ext, $decode, $label) {
    $script:total++
    $inp = "$d\case.$ext"
    [System.IO.File]::WriteAllBytes($inp, $bytes)
    $out = "$d\case_out"
    $args = if ($decode) { @("--decode", $inp, "$out.wav", "--quiet") } else { @("-b","128", $inp, "$out.mp3", "--quiet") }
    $p = Start-Process $exe -ArgumentList $args -PassThru -NoNewWindow -RedirectStandardError "$d\_err.txt"
    if (-not $p.WaitForExit(10000)) { $p.Kill(); Write-Host ("  HANG: $label") -ForegroundColor Magenta; $script:crashes++; $script:crashFiles += "$label (HANG)"; return }
    # After a timed WaitForExit, the PassThru object may not have populated
    # ExitCode yet; a blocking WaitForExit() forces it. Guard null == success.
    $p.WaitForExit()
    $code = $p.ExitCode
    if ($null -eq $code) { $code = 0 }
    if (IsCrash $code) {
        $script:crashes++
        $script:crashFiles += "$label (exit=$code)"
        Copy-Item $inp "$d\CRASH_$($script:total)_$label.$ext" -Force
        Write-Host ("  CRASH: $label exit=$code") -ForegroundColor Red
    }
}

Write-Host "=== Truncation fuzzing (cut files at many lengths) ==="
foreach ($seed in @(@($seedWav,"wav",$false),@($seedAif,"aiff",$false),@($seedMp3,"mp3",$true),@($seedFlac,"flac",$false))) {
    $b=$seed[0]; $ext=$seed[1]; $dec=$seed[2]
    for ($len=0; $len -le [Math]::Min(200,$b.Length); $len+=4) {
        RunCase ($b[0..([Math]::Max(0,$len-1))]) $ext $dec "trunc-$ext-$len"
    }
    # also truncate near the end
    foreach ($cut in 300, ($b.Length/2), ($b.Length-100), ($b.Length-1)) {
        $c=[int]$cut; if ($c -gt 0 -and $c -lt $b.Length) { RunCase ($b[0..($c-1)]) $ext $dec "trunc-$ext-$c" }
    }
}

Write-Host "=== Bit-flip / byte-corruption fuzzing (mutate headers) ==="
foreach ($seed in @(@($seedWav,"wav",$false),@($seedAif,"aiff",$false),@($seedMp3,"mp3",$true),@($seedFlac,"flac",$false))) {
    $b=$seed[0]; $ext=$seed[1]; $dec=$seed[2]
    for ($iter=0; $iter -lt 150; $iter++) {
        $m = $b.Clone()
        # corrupt 1-4 bytes in the first 64 (header region) where the dangerous fields live
        $nmut = 1 + $rng.Next(4)
        for ($k=0;$k -lt $nmut;$k++){ $pos=$rng.Next([Math]::Min(64,$m.Length)); $m[$pos]=[byte]$rng.Next(256) }
        RunCase $m $ext $dec "flip-$ext-$iter"
    }
}

Write-Host "=== Pure random garbage with valid magic ==="
foreach ($magic in @(@("RIFF","wav",$false),@("FORM","aiff",$false))) {
    for ($iter=0; $iter -lt 50; $iter++) {
        $sz = 44 + $rng.Next(4096)
        $m = New-Object byte[] $sz
        $rng.NextBytes($m)
        # stamp plausible magic so it gets past the first check into the dangerous parsing
        $mg=[System.Text.Encoding]::ASCII.GetBytes($magic[0]); for($k=0;$k -lt 4;$k++){$m[$k]=$mg[$k]}
        if ($magic[0] -eq "RIFF") { $w=[System.Text.Encoding]::ASCII.GetBytes("WAVE"); for($k=0;$k -lt 4;$k++){$m[8+$k]=$w[$k]} }
        else { $w=[System.Text.Encoding]::ASCII.GetBytes("AIFF"); for($k=0;$k -lt 4;$k++){$m[8+$k]=$w[$k]} }
        RunCase $m $magic[1] $magic[2] "rand-$($magic[0])-$iter"
    }
}

Write-Host ""
Write-Host ("INPUT FUZZ DONE: $total cases, $crashes crashes") -ForegroundColor $(if($crashes -eq 0){"Green"}else{"Red"})
if ($crashes -gt 0) { $crashFiles | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" } }
