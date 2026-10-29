<#
.SYNOPSIS
  Run every test suite that does not need hardware: C core (CTest), firmware
  host tests, Python (gateway, simulator, tools and the live end-to-end slice),
  dashboard unit tests; optionally the Playwright browser tests.

.EXAMPLE
  .\scripts\test_all.ps1              # everything except the browser tests
  .\scripts\test_all.ps1 -Quick       # skip the ~2 min live e2e suite
  .\scripts\test_all.ps1 -Browser     # also Playwright (installed Edge)
#>
[CmdletBinding()]
param([switch]$Quick, [switch]$Browser)

$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root ".venv\Scripts"
$results = [ordered]@{}

function Stage([string]$Name, [scriptblock]$Body) {
    Write-Host "==> $Name" -ForegroundColor Cyan
    & $Body
    $results[$Name] = if ($LASTEXITCODE -eq 0) { "PASS" } else { "FAIL ($LASTEXITCODE)" }
}

Stage "C core (CTest)" {
    & "$Bin\ctest.exe" --test-dir "$Root\algaops-assurance\build" --output-on-failure
}
Stage "Firmware host tests (CTest)" {
    & "$Bin\ctest.exe" --test-dir "$Root\firmware\esp32-sensor-node\host_test\build" --output-on-failure
}
Stage "Python: gateway, simulator, tools$(if (-not $Quick) { ', live e2e' })" {
    Push-Location $Root
    try {
        if ($Quick) { & "$Bin\python.exe" -m pytest -m "not e2e" }
        else        { & "$Bin\python.exe" -m pytest }
    } finally { Pop-Location }
}
if (Get-Command npm -ErrorAction SilentlyContinue) {
    Stage "Dashboard unit tests (Vitest)" {
        Push-Location "$Root\dashboard"
        try { npm test --silent } finally { Pop-Location }
    }
    if ($Browser) {
        Stage "Dashboard browser tests (Playwright, Edge)" {
            Push-Location "$Root\dashboard"
            try { npm run e2e --silent } finally { Pop-Location }
        }
    }
}

Write-Host ""
$failed = $false
foreach ($k in $results.Keys) {
    $color = if ($results[$k] -eq "PASS") { "Green" } else { $failed = $true; "Red" }
    Write-Host ("{0,-55} {1}" -f $k, $results[$k]) -ForegroundColor $color
}
if ($failed) { exit 1 }
