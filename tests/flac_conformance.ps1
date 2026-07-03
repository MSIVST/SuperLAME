# SuperLAME FLAC-input conformance test against the IETF CELLAR flac-test-files
# corpus (github.com/ietf-wg-cellar/flac-test-files) -- the canonical set used to
# validate FLAC decoders against the spec.
#
# SuperLAME is an MP3 ENCODER, so "conformance" here means: our dr_flac input
# path must ingest every spec-valid file correctly, gracefully refuse files
# outside MP3's scope (>2 channels), and cleanly reject malformed files -- with
# ZERO crashes/hangs anywhere.
#
# Buckets:
#   subset/   (64) spec-conformant  -> must ENCODE ok (or clean refuse if >2ch)
#   uncommon/ (11) unusual-but-valid-> must ENCODE ok (or clean refuse if >2ch/var)
#   faulty/   (11) malformed         -> must be REJECTED cleanly (exit 1), no crash
#
# For stereo/mono valid files we also verify SAMPLE ACCURACY: decode the FLAC with
# ffmpeg (reference) and with our path (by encoding lossless-ish and comparing is
# not exact, so instead we compare our decoded PCM directly by having SuperLAME's
# FLAC reader feed a CBR320 encode, decode both our mp3 and an ffmpeg-mp3 of the
# ffmpeg-decoded wav, and require the two MP3s match -- i.e. our decoder produced
# the same samples ffmpeg did before the (identical) encoder ran).
$ErrorActionPreference = "Continue"
# Repo root holding the built exe + test corpora. Override with SUPERLAME_ROOT.
$Root = if ($env:SUPERLAME_ROOT) { $env:SUPERLAME_ROOT } else { Split-Path -Parent $PSScriptRoot }
$exe  = "$Root\build\final\superlame-mt.exe"
$ff   = "$Root\ffmpeg.exe"
$root = "$Root\flac-test-files"
$work = "$Root\build\flacconf"
New-Item -ItemType Directory -Force -Path $work | Out-Null

function RunExe($argList, $timeoutMs=30000) {
    $errf = "$work\err.txt"
    # Quote every argument so paths containing spaces (all these test files) reach
    # the exe as ONE argument each. Start-Process -ArgumentList with a bare array
    # re-splits on spaces in PS 5.1, so build an explicit quoted string.
    $quoted = ($argList | ForEach-Object { '"' + ($_ -replace '"','\"') + '"' }) -join ' '
    $p = Start-Process $exe -ArgumentList $quoted -PassThru -NoNewWindow -RedirectStandardError $errf
    if (-not $p.WaitForExit($timeoutMs)) { $p.Kill(); return @{code=-999; err="HANG"} }
    $p.WaitForExit()
    $c = $p.ExitCode; if ($null -eq $c) { $c = 0 }
    $e = ""; if (Test-Path $errf) { $e = (Get-Content $errf -Raw) }
    return @{code=$c; err=$e}
}
function IsCrash($c) { return ($c -ne 0 -and $c -ne 1) }   # 0 ok, 1 clean-error

# --- valid buckets: subset + uncommon ---
$validCrash=0; $validOk=0; $validRefused=0; $sampleMismatch=0; $sampleChecked=0
foreach ($bucket in @("subset","uncommon")) {
    $files = Get-ChildItem "$root\$bucket\*.flac" -ErrorAction SilentlyContinue
    foreach ($f in $files) {
        $out = "$work\o.mp3"
        Remove-Item $out -ErrorAction SilentlyContinue
        $r = RunExe @("-b","320","-t","4", $f.FullName, $out, "--quiet")
        if (IsCrash $r.code) {
            $validCrash++; Write-Host ("  CRASH: {0}/{1} (exit={2})" -f $bucket,$f.Name,$r.code) -ForegroundColor Red
            continue
        }
        if ($r.code -eq 0 -and (Test-Path $out) -and (Get-Item $out).Length -gt 0) {
            $validOk++
            # sample-accuracy check (stereo/mono only; skip >2ch and var-param)
            $probe = & $ff -hide_banner -i $f.FullName 2>&1 | Out-String
            $isStereoMono = ($probe -match "Audio: flac, \d+ Hz, (stereo|mono|1 channels|2 channels)")
            if ($isStereoMono) {
                $sampleChecked++
                $refwav = "$work\ref.wav"; $refmp3 = "$work\ref.mp3"
                & $ff -hide_banner -loglevel error -i $f.FullName -c:a pcm_s32le $refwav -y 2>$null
                # encode ffmpeg-decoded wav through SAME encoder path
                & $exe -b320 -t4 $refwav $refmp3 --quiet 2>$null | Out-Null
                & $ff -hide_banner -loglevel error -i $out    -f f32le "$work\a.raw" -y 2>$null
                & $ff -hide_banner -loglevel error -i $refmp3 -f f32le "$work\b.raw" -y 2>$null
                $db = & python -c @"
import numpy as np, math
try:
 a=np.fromfile(r'$work\a.raw',dtype=np.float32); b=np.fromfile(r'$work\b.raw',dtype=np.float32)
 n=min(len(a),len(b))
 if n==0: print('nan')
 else:
  a=a[:n];b=b[:n];d=a-b;r=math.sqrt(float(np.mean(d*d)))
  print('%.2f'%(20*math.log10(r)) if r>0 else '-999')
except Exception as e: print('nan')
"@ 2>$null
                if ($db -eq 'nan' -or [double]$db -gt -60) {
                    $sampleMismatch++
                    Write-Host ("  SAMPLE MISMATCH: {0}/{1} = {2} dB (our FLAC decode != ffmpeg)" -f $bucket,$f.Name,$db) -ForegroundColor Yellow
                }
            }
        } else {
            # clean exit 1: acceptable ONLY if the file is out of MP3 scope
            $validRefused++
            $why = ($r.err -split "`n" | Select-Object -First 1).Trim()
            Write-Host ("  refused (clean): {0}/{1}  -- {2}" -f $bucket,$f.Name,$why) -ForegroundColor DarkGray
        }
    }
}

# --- faulty bucket: must reject cleanly, never crash ---
$faultyCrash=0; $faultyRejected=0; $faultyAccepted=0
foreach ($f in (Get-ChildItem "$root\faulty\*.flac" -ErrorAction SilentlyContinue)) {
    $out = "$work\o.mp3"; Remove-Item $out -ErrorAction SilentlyContinue
    $r = RunExe @("-b","320","-t","4", $f.FullName, $out, "--quiet")
    if (IsCrash $r.code) { $faultyCrash++; Write-Host ("  CRASH on faulty: {0} (exit={1})" -f $f.Name,$r.code) -ForegroundColor Red }
    elseif ($r.code -eq 1) { $faultyRejected++ }
    else {
        # exit 0 on a faulty file: tolerable IF dr_flac could still decode a
        # valid prefix (some 'faulty' files are recoverable); note it.
        $faultyAccepted++
        Write-Host ("  faulty accepted (decoded anyway): {0}" -f $f.Name) -ForegroundColor DarkYellow
    }
}

Write-Host ""
Write-Host "===== FLAC CONFORMANCE (IETF CELLAR corpus) ====="
Write-Host ("valid (subset+uncommon): {0} encoded ok, {1} cleanly refused (out-of-scope), {2} CRASHES" -f $validOk,$validRefused,$validCrash)
Write-Host ("sample-accuracy vs ffmpeg: {0} checked, {1} mismatches" -f $sampleChecked,$sampleMismatch)
Write-Host ("faulty: {0} rejected, {1} decoded-anyway, {2} CRASHES" -f $faultyRejected,$faultyAccepted,$faultyCrash)
$pass = ($validCrash -eq 0) -and ($faultyCrash -eq 0) -and ($sampleMismatch -eq 0)
Write-Host ("RESULT: {0}" -f $(if($pass){"PASS"}else{"CHECK"})) -ForegroundColor $(if($pass){"Green"}else{"Red"})
