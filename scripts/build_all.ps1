<#
.SYNOPSIS
  Build AlgaOps Assurance on Windows: Python venv and packages, the C11 core
  (CMake + Ninja + zig cc from the venv), the firmware host tests and the
  dashboard.  The ESP32 firmware itself needs ESP-IDF (scripts/build_firmware.sh).

.EXAMPLE
  .\scripts\build_all.ps1
  .\scripts\build_all.ps1 -SkipDashboard
#>
[CmdletBinding()]
param([switch]$SkipDashboard)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Venv = Join-Path $Root ".venv"
$Bin = Join-Path $Venv "Scripts"
$Py = Join-Path $Bin "python.exe"

function Step([string]$Msg) { Write-Host "==> $Msg" -ForegroundColor Cyan }
function Invoke-Checked([string]$Exe, [string[]]$ArgList) {
    & $Exe @ArgList
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($ArgList -join ' ') failed with exit code $LASTEXITCODE" }
}

if (-not (Test-Path $Py)) {
    Step "Creating the Python virtual environment (.venv)"
    Invoke-Checked "py" @("-3", "-m", "venv", $Venv)
}

Step "Python packages (pinned) and build tools"
Invoke-Checked $Py @("-m", "pip", "install", "--disable-pip-version-check", "-q",
    "-r", "$Root\gateway\requirements.lock", "-r", "$Root\simulator\requirements.lock",
    "cmake==4.4.4", "ninja==1.13.2", "ziglang==0.16.0")
Invoke-Checked $Py @("-m", "pip", "install", "--disable-pip-version-check", "-q", "--no-deps",
    "-e", "$Root\gateway", "-e", "$Root\simulator")

# zig as the C compiler: small wrappers CMake can call like gcc / ar / ranlib.
foreach ($tool in @(@("zcc.cmd", "cc"), @("zar.cmd", "ar"), @("zranlib.cmd", "ranlib"))) {
    $path = Join-Path $Bin $tool[0]
    if (-not (Test-Path $path)) {
        Set-Content -Path $path -Encoding ascii `
            -Value "@`"%~dp0..\Lib\site-packages\ziglang\zig.exe`" $($tool[1]) %*"
    }
}
$Toolchain = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=$Bin\zcc.cmd", "-DCMAKE_AR=$Bin\zar.cmd",
    "-DCMAKE_RANLIB=$Bin\zranlib.cmd", "-DCMAKE_MAKE_PROGRAM=$Bin\ninja.exe")

Step "C11 core (static + shared library, CLI, tests)"
$Core = Join-Path $Root "algaops-assurance"
Invoke-Checked "$Bin\cmake.exe" (@("-S", $Core, "-B", "$Core\build", "-DALGAOPS_WERROR=ON") + $Toolchain)
Invoke-Checked "$Bin\cmake.exe" @("--build", "$Core\build")

Step "Firmware host tests (node logic without hardware)"
$Host_ = Join-Path $Root "firmware\esp32-sensor-node\host_test"
Invoke-Checked "$Bin\cmake.exe" (@("-S", $Host_, "-B", "$Host_\build", "-DNODE_HOST_WERROR=ON") + $Toolchain)
Invoke-Checked "$Bin\cmake.exe" @("--build", "$Host_\build")

if (-not $SkipDashboard) {
    if (Get-Command npm -ErrorAction SilentlyContinue) {
        Step "Dashboard (Vite build, served by the gateway)"
        Push-Location (Join-Path $Root "dashboard")
        try {
            if (-not (Test-Path "node_modules")) { Invoke-Checked "npm" @("ci", "--no-audit", "--no-fund") }
            Invoke-Checked "npm" @("run", "build")
        } finally { Pop-Location }
    } else {
        Write-Warning "npm not found: dashboard not built (the gateway API still works)"
    }
}
Step "Done"
