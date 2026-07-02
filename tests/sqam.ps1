# SQAM corpus test: the EBU Sound Quality Assessment Material is the standard
# codec-stress reference set (transients, dynamics, solo instruments). Decode
# each FLAC, encode at full -t0 across modes, and flag any repacker desync.
# Runs against BOTH the fixed and the proven-buggy binary so we can see whether
# this corpus discriminates them (= real coverage for the fix).
param([string]$which = "fixed")   # "fixed" | "buggy" | "both"
$ErrorActionPreference = "Continue"
# Repo root holding the built exe + test corpora. Override with SUPERLAME_ROOT.
$Root = if ($env:SUPERLAME_ROOT) { $env:SUPERLAME_ROOT } else { Split-Path -Parent $PSScriptRoot }
$ff    = "$Root\ffmpeg.exe"
$fixed = "$Root\build\final\superlame-mt.exe"
$buggy = "$Root\build\prefix-build\superlame-prefix.exe"
$sqam  = "$Root\SQAM"
$work  = "$Root\build\sqam"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$modes = @(@("-V0"),@("-V2"),@("-b","320"),@("-b","128"),@("--abr","192"))
# Note: the unary comma forces an array-of-arrays even for a single entry
# (PowerShell otherwise flattens @(@(...)) into a flat array).
if ($which -eq "both") { $bins = @( ,@("fixed",$fixed) ) + @( ,@("buggy",$buggy) ) }
elseif ($which -eq "buggy") { $bins = @( ,@("buggy",$buggy) ) }
else { $bins = @( ,@("fixed",$fixed) ) }

$flacs = Get-ChildItem "$sqam\*.flac" | Sort-Object Name
Write-Host ("SQAM test: {0} tracks x {1} modes x {2} binary(ies)`n" -f $flacs.Count, $modes.Count, $bins.Count)

$results = @{}
foreach ($b in $bins) { $results[$b[0]] = @{ total=0; desync=0; cases=@() } }

foreach ($flac in $flacs) {
    $wav = "$work\cur.wav"
    & $ff -hide_banner -loglevel error -i $flac.FullName -c:a pcm_s16le $wav -y 2>$null
    if (-not (Test-Path $wav)) { Write-Host "  decode failed: $($flac.Name)"; continue }
    foreach ($b in $bins) {
        $bname = $b[0]; $bexe = $b[1]
        foreach ($m in $modes) {
            $results[$bname].total++
            $margs = @($m) + @("-t16", $wav, "$work\out.mp3", "--quiet")
            $err = & $bexe @margs 2>&1
            if ("$err" -match "re-encoding single-threaded") {
                $results[$bname].desync++
                $lbl = "$($flac.Name) $($m -join ' ')"
                $results[$bname].cases += $lbl
                Copy-Item $wav "$work\FAIL_$($bname)_$($flac.BaseName)_$(($m -join '') -replace '[^a-zA-Z0-9]','').wav" -Force
                Write-Host ("  [{0}] DESYNC: {1}" -f $bname, $lbl) -ForegroundColor Red
            }
        }
    }
    Write-Host ("  done {0}" -f $flac.Name) -ForegroundColor DarkGray
}

Write-Host "`n===== SQAM RESULTS ====="
foreach ($b in $bins) {
    $r = $results[$b[0]]
    Write-Host ("{0,-6}: {1} encodes, {2} desyncs" -f $b[0], $r.total, $r.desync) -ForegroundColor $(if($r.desync -eq 0){"Green"}else{"Yellow"})
    if ($r.desync -gt 0) { $r.cases | ForEach-Object { Write-Host "    $_" } }
}
