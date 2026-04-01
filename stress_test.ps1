# Cornea Controller Stress Test
# Usage: .\stress_test.ps1 [-Seconds 60] [-Host 127.0.0.1] [-Port 5566]

param(
    [int]$Seconds = 60,
    [string]$TargetHost = "127.0.0.1",
    [int]$Port = 5566
)

$totalSent = 0
$totalSuccess = 0
$totalFail = 0
$totalTimeout = 0

function Send-TcpCommand {
    param([string]$Command, [int]$TimeoutMs = 10000)
    try {
        $client = New-Object System.Net.Sockets.TcpClient($TargetHost, $Port)
        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMs
        $writer = New-Object System.IO.StreamWriter($stream)
        $reader = New-Object System.IO.StreamReader($stream)
        $writer.WriteLine($Command)
        $writer.Flush()
        $response = $reader.ReadLine()
        $reader.Close()
        $writer.Close()
        $client.Close()
        return $response
    }
    catch [System.IO.IOException] {
        return "TIMEOUT"
    }
    catch {
        return "ERROR: $_"
    }
}

function Test-Command {
    param([string]$Name, [string]$Command, [int]$TimeoutMs = 10000)
    $script:totalSent++
    $r = Send-TcpCommand -Command $Command -TimeoutMs $TimeoutMs
    if ($r -eq "TIMEOUT") {
        $script:totalTimeout++
        Write-Host "  [TIMEOUT] $Name" -ForegroundColor Red
        return $false
    }
    elseif ($r -like "ERROR:*") {
        $script:totalFail++
        Write-Host "  [ERROR] $Name : $r" -ForegroundColor Red
        return $false
    }
    else {
        $json = $r | ConvertFrom-Json -ErrorAction SilentlyContinue
        if ($json -and $json.success -eq $true) {
            $script:totalSuccess++
            return $true
        }
        else {
            $script:totalFail++
            # Only log non-temperature failures
            if ($Name -notlike "*temp*") {
                Write-Host "  [FAIL] $Name : $r" -ForegroundColor Yellow
            }
            return $false
        }
    }
}

Write-Host "=== Cornea Controller Stress Test ===" -ForegroundColor Cyan
Write-Host "Target: ${Host}:${Port}"
Write-Host "Duration: ${Seconds}s"
Write-Host ""

# Step 1: Get device list
Write-Host "[Phase 1] Getting device list..." -ForegroundColor Yellow
$r = Send-TcpCommand '{"cmd":"getStatus"}'
$devices = @()
try {
    $json = $r | ConvertFrom-Json
    if ($json.data.devices) {
        foreach ($dev in $json.data.devices) {
            $devices += $dev.serial
        }
    }
} catch {}

if ($devices.Count -eq 0) {
    Write-Host "No devices found. Using fake serials for testing." -ForegroundColor Yellow
    $devices = @("FAKE_001", "FAKE_002", "FAKE_003", "FAKE_004", "FAKE_005", "FAKE_006")
}

Write-Host "Devices: $($devices -join ', ')" -ForegroundColor Green
Write-Host ""

# Step 2: Stress test
Write-Host "[Phase 2] Running stress test for ${Seconds}s..." -ForegroundColor Yellow
Write-Host ""

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$round = 0

while ($stopwatch.Elapsed.TotalSeconds -lt $Seconds) {
    $round++
    $elapsed = [math]::Floor($stopwatch.Elapsed.TotalSeconds)
    Write-Host "--- Round $round (${elapsed}s / ${Seconds}s) ---" -ForegroundColor Gray

    # Test 1: refreshDevices
    Test-Command "refreshDevices" '{"cmd":"refreshDevices"}' | Out-Null

    # Test 2: getStatus
    Test-Command "getStatus" '{"cmd":"getStatus"}' | Out-Null

    # Test 3: Rapid setBrightness to all devices
    foreach ($serial in $devices) {
        $level = [math]::Round((Get-Random -Minimum 1 -Maximum 50) / 100.0, 2)
        $cmd = "{`"cmd`":`"setBrightness`",`"serial`":`"$serial`",`"level`":$level}"
        Test-Command "setBrightness($serial)" $cmd 15000 | Out-Null
    }

    # Test 4: Rapid sendImageByName to all devices
    foreach ($serial in $devices) {
        $cmd = "{`"cmd`":`"sendImageByName`",`"serial`":`"$serial`",`"name`":`"720x720_Black.png`"}"
        Test-Command "sendImage($serial)" $cmd 15000 | Out-Null
    }

    # Test 5: getTemperature burst
    foreach ($serial in $devices) {
        $cmd = "{`"cmd`":`"getTemperature`",`"serial`":`"$serial`"}"
        Test-Command "getTemp($serial)" $cmd 5000 | Out-Null
    }

    # Test 6: listImages
    Test-Command "listImages" '{"cmd":"listImages"}' | Out-Null

    # Brief pause between rounds
    Start-Sleep -Milliseconds 500

    # Progress
    $rate = if ($totalSent -gt 0) { [math]::Round($totalSuccess / $totalSent * 100, 1) } else { 0 }
    Write-Host "  Sent:$totalSent OK:$totalSuccess Fail:$totalFail Timeout:$totalTimeout (${rate}%)" -ForegroundColor Cyan
}

$stopwatch.Stop()

# Summary
Write-Host ""
Write-Host "=== Stress Test Results ===" -ForegroundColor Cyan
Write-Host "  Duration:  $([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))s"
Write-Host "  Rounds:    $round"
Write-Host "  Total:     $totalSent commands"
Write-Host "  Success:   $totalSuccess" -ForegroundColor Green
Write-Host "  Failed:    $totalFail" -ForegroundColor $(if ($totalFail -gt 0) {"Yellow"} else {"Green"})
Write-Host "  Timeout:   $totalTimeout" -ForegroundColor $(if ($totalTimeout -gt 0) {"Red"} else {"Green"})
$rate = if ($totalSent -gt 0) { [math]::Round($totalSuccess / $totalSent * 100, 1) } else { 0 }
Write-Host "  Rate:      ${rate}%" -ForegroundColor $(if ($rate -ge 95) {"Green"} elseif ($rate -ge 80) {"Yellow"} else {"Red"})
Write-Host ""

if ($totalTimeout -gt 0) {
    Write-Host "WARNING: $totalTimeout timeouts detected - server may be unstable" -ForegroundColor Red
}
if ($totalFail -gt 0 -and $devices[0] -ne "FAKE_001") {
    Write-Host "NOTE: Failures on real devices may be normal (device not powered on, etc.)" -ForegroundColor Yellow
}
