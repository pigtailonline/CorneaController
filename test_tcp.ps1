# Cornea Controller TCP Test Script
# Usage: .\test_tcp.ps1
# Validates: cmd/serial echo, error response format, concurrent commands

$host_ip = "127.0.0.1"
$port = 5566
$pass = 0
$fail = 0

function Send-TcpCommand {
    param(
        [string]$Command,
        [int]$TimeoutMs = 10000
    )

    try {
        $client = New-Object System.Net.Sockets.TcpClient($host_ip, $port)
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
    catch {
        return "ERROR: $_"
    }
}

function Assert-Contains {
    param(
        [string]$TestName,
        [string]$Response,
        [string]$Expected
    )
    if ($Response -like "*$Expected*") {
        Write-Host "  [PASS] $TestName" -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host "  [FAIL] $TestName" -ForegroundColor Red
        Write-Host "    Expected to contain: $Expected" -ForegroundColor Red
        Write-Host "    Got: $Response" -ForegroundColor Red
        $script:fail++
    }
}

function Assert-JsonHasField {
    param(
        [string]$TestName,
        [string]$Response,
        [string]$Field
    )
    try {
        $json = $Response | ConvertFrom-Json
        $value = $json.$Field
        if ($null -ne $value) {
            Write-Host "  [PASS] $TestName ($Field=$value)" -ForegroundColor Green
            $script:pass++
        } else {
            Write-Host "  [FAIL] $TestName - missing field '$Field'" -ForegroundColor Red
            Write-Host "    Response: $Response" -ForegroundColor Red
            $script:fail++
        }
    } catch {
        Write-Host "  [FAIL] $TestName - invalid JSON" -ForegroundColor Red
        Write-Host "    Response: $Response" -ForegroundColor Red
        $script:fail++
    }
}

Write-Host "=== Cornea Controller TCP Test ===" -ForegroundColor Cyan
Write-Host "Target: $host_ip`:$port"
Write-Host ""

# ============================================================
# Test 1: Basic commands
# ============================================================
Write-Host "[Test 1] listDevices" -ForegroundColor Yellow
$cmd = '{"cmd":"listDevices"}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Response has 'cmd'" $r "cmd"
Assert-JsonHasField "Response has 'success'" $r "success"
Assert-Contains "cmd echoed back" $r '"cmd":"listDevices"'
Write-Host ""

# ============================================================
# Test 2: getStatus
# ============================================================
Write-Host "[Test 2] getStatus" -ForegroundColor Yellow
$cmd = '{"cmd":"getStatus"}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Response has 'cmd'" $r "cmd"
Assert-Contains "cmd echoed back" $r '"cmd":"getStatus"'
Write-Host ""

# ============================================================
# Test 3: refreshDevices
# ============================================================
Write-Host "[Test 3] refreshDevices" -ForegroundColor Yellow
$cmd = '{"cmd":"refreshDevices"}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Response has 'cmd'" $r "cmd"
Assert-Contains "cmd echoed back" $r '"cmd":"refreshDevices"'
Write-Host ""

# ============================================================
# Test 4: Invalid command - error response should have cmd
# ============================================================
Write-Host "[Test 4] Invalid command - error should have cmd" -ForegroundColor Yellow
$cmd = '{"cmd":"fakeCommand123"}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Error response has 'cmd'" $r "cmd"
Assert-Contains "success=false" $r '"success":false'
Assert-Contains "cmd echoed back" $r '"cmd":"fakeCommand123"'
Write-Host ""

# ============================================================
# Test 5: setBrightness with invalid serial - error should have cmd+serial
# ============================================================
Write-Host "[Test 5] setBrightness invalid serial - error should have cmd+serial" -ForegroundColor Yellow
$cmd = '{"cmd":"setBrightness","serial":"FAKE_SERIAL","level":0.5}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd -TimeoutMs 15000
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Error response has 'cmd'" $r "cmd"
Assert-JsonHasField "Error response has 'serial'" $r "serial"
Assert-Contains "cmd echoed" $r '"cmd":"setBrightness"'
Assert-Contains "serial echoed" $r '"serial":"FAKE_SERIAL"'
Assert-Contains "success=false" $r '"success":false'
Write-Host ""

# ============================================================
# Test 6: powerOn with invalid serial - error should have cmd+serial
# ============================================================
Write-Host "[Test 6] powerOn invalid serial - error should have cmd+serial" -ForegroundColor Yellow
$cmd = '{"cmd":"powerOn","serial":"FAKE_SERIAL"}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd -TimeoutMs 30000
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Error response has 'cmd'" $r "cmd"
Assert-JsonHasField "Error response has 'serial'" $r "serial"
Assert-Contains "cmd echoed" $r '"cmd":"powerOn"'
Assert-Contains "serial echoed" $r '"serial":"FAKE_SERIAL"'
Write-Host ""

# ============================================================
# Test 7: Missing parameters - error should have cmd
# ============================================================
Write-Host "[Test 7] setBrightness missing serial - error should have cmd" -ForegroundColor Yellow
$cmd = '{"cmd":"setBrightness","level":0.5}'
Write-Host "  Send: $cmd"
$r = Send-TcpCommand -Command $cmd -TimeoutMs 15000
Write-Host "  Recv: $r" -ForegroundColor Gray
Assert-JsonHasField "Error response has 'cmd'" $r "cmd"
Assert-Contains "success=false" $r '"success":false'
Write-Host ""

# ============================================================
# Test 8: refreshDevices multiple times - should not accumulate tabs
# ============================================================
Write-Host "[Test 8] Multiple refreshDevices" -ForegroundColor Yellow
for ($i = 1; $i -le 3; $i++) {
    $cmd = '{"cmd":"refreshDevices"}'
    $r = Send-TcpCommand -Command $cmd
    Write-Host "  Refresh $i`: $r" -ForegroundColor Gray
}
$cmd = '{"cmd":"getStatus"}'
$r = Send-TcpCommand -Command $cmd
Write-Host "  Final getStatus: $r" -ForegroundColor Gray
Assert-JsonHasField "Response has 'cmd'" $r "cmd"
Write-Host ""

# ============================================================
# Summary
# ============================================================
Write-Host ""
Write-Host "=== Results ===" -ForegroundColor Cyan
Write-Host "  PASS: $pass" -ForegroundColor Green
Write-Host "  FAIL: $fail" -ForegroundColor $(if ($fail -gt 0) { "Red" } else { "Green" })
Write-Host ""

if ($fail -gt 0) {
    Write-Host "Key issues to check:" -ForegroundColor Yellow
    Write-Host "  - Every response MUST have 'cmd' field (echo back the request cmd)"
    Write-Host "  - Error responses with serial MUST echo 'serial' field"
    Write-Host "  - refreshDevices should not accumulate 'No Devices' tabs"
    Write-Host ""
}

# Interactive mode
Write-Host "=== Interactive Mode ===" -ForegroundColor Cyan
Write-Host "Enter JSON command (or 'quit' to exit):"
Write-Host "Examples:"
Write-Host '  {"cmd":"listDevices"}'
Write-Host '  {"cmd":"powerOn","serial":"LITE20240121"}'
Write-Host '  {"cmd":"powerOn","serial":"LITE20240121","variant":"F33R"}'
Write-Host '  {"cmd":"setBrightness","serial":"LITE20240121","level":0.03}'
Write-Host '  {"cmd":"sendImage","serial":"LITE20240121","path":"C:/test.png"}'
Write-Host '  {"cmd":"powerOff","serial":"LITE20240121"}'
Write-Host ""

while ($true) {
    $input_cmd = Read-Host "CMD"

    if ($input_cmd -eq "quit" -or $input_cmd -eq "exit" -or $input_cmd -eq "q") {
        break
    }

    if ($input_cmd -ne "") {
        $response = Send-TcpCommand -Command $input_cmd
        Write-Host "  -> $response" -ForegroundColor Green
    }
}

Write-Host "Done." -ForegroundColor Cyan
