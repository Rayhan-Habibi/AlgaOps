<#
.SYNOPSIS
  Local DEMO / SYNTHETIC run: development MQTT broker, the gateway with its
  dashboard on http://127.0.0.1:8080 and three simulated pumps published in
  real time (healthy, stuck accelerometer, rising vibration).  Ctrl+C stops all.

  Data lands in var\demo\ (database, checkpoints, exports).  Production uses
  Mosquitto with authentication (deploy\mosquitto) instead of the dev broker.
#>
[CmdletBinding()]
param([int]$ApiPort = 8080, [int]$MqttPort = 1883)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Py = Join-Path $Root ".venv\Scripts\python.exe"
$Work = Join-Path $Root "var\demo"
New-Item -ItemType Directory -Force $Work | Out-Null
if (-not (Test-Path (Join-Path $Root "dashboard\dist\index.html"))) {
    Write-Warning "dashboard\dist missing: run .\scripts\build_all.ps1 first (API works without it)"
}
$env:PYTHONPATH = "$Root\gateway;$Root\simulator"
while (Get-NetTCPConnection -LocalPort $ApiPort -State Listen -ErrorAction SilentlyContinue) {
    Write-Host "port $ApiPort is in use by another program: trying $($ApiPort + 1)"
    $ApiPort++
}

$cfg = Join-Path $Work "gateway.toml"
$rootFwd = $Root -replace '\\', '/'
@"
gateway_id = "demo-gw"
site = "lab (demo)"
[mqtt]
port = $MqttPort
client_id = "algaops-gateway-demo"
[storage]
db_path = "algaops.db"
checkpoint_file = "checkpoints.jsonl"
export_dir = "exports"
[api]
port = $ApiPort
dashboard_dir = "$rootFwd/dashboard/dist"
[engine]
policy_file = "$rootFwd/gateway/config/policy.default.toml"
"@ | ForEach-Object { [System.IO.File]::WriteAllText($cfg, $_, (New-Object System.Text.UTF8Encoding($false))) }

$procs = @()
function Start-Bg([string]$Name, [string[]]$ArgList) {
    $p = Start-Process -FilePath $Py -ArgumentList $ArgList -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $Work "$Name.out.log") `
        -RedirectStandardError (Join-Path $Work "$Name.err.log")
    Write-Host "started $Name (pid $($p.Id))"
    return $p
}

try {
    $busy = Get-NetTCPConnection -LocalPort $MqttPort -State Listen -ErrorAction SilentlyContinue
    if ($busy) {
        Write-Host "port $MqttPort already has a broker: using it"
    } else {
        $procs += Start-Bg "broker" @("-m", "algaops_gateway", "devbroker", "--port", "$MqttPort")
        Start-Sleep -Seconds 2
    }
    $procs += Start-Bg "gateway" @("-m", "algaops_gateway", "--config", "`"$cfg`"", "run")
    $api = "http://127.0.0.1:$ApiPort"
    $ok = $false
    for ($i = 0; $i -lt 60 -and -not $ok; $i++) {
        try { $ok = (Invoke-RestMethod "$api/api/health" -TimeoutSec 2).status -eq "ok" } catch { Start-Sleep 1 }
    }
    if (-not $ok) { throw "gateway did not start: see $Work\gateway.err.log" }

    # A plain .NET string: Get-Content adds properties that ConvertTo-Json would serialise.
    $csv = [System.IO.File]::ReadAllText((Join-Path $Root "algaops-assurance\data\v2\baseline_modes.csv"))
    $nodes = [ordered]@{ "sim-pump-01" = "normal_steady"; "sim-pump-02" = "stuck_vibration";
                         "sim-pump-03" = "equipment_vibration_anomaly" }
    foreach ($node in $nodes.Keys) {
        $body = @{ node_id = $node; csv = $csv; activate = $true } | ConvertTo-Json
        Invoke-RestMethod -Method Post "$api/api/baselines/import" -ContentType "application/json" -Body $body | Out-Null
        $procs += Start-Bg "sim-$node" @("-m", "algaops_sim", "publish", "--scenario", $nodes[$node],
                                         "--node-id", $node, "--port", "$MqttPort")
    }
    Write-Host ""
    Write-Host "Dashboard: $api   (DEMO / SYNTHETIC data; Ctrl+C to stop)" -ForegroundColor Green
    while ($true) { Start-Sleep -Seconds 5 }
} finally {
    foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    Write-Host "stopped"
}
