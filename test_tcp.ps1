# Cornea Controller TCP Test Script
# Usage: .\test_tcp.ps1

$host_ip = "127.0.0.1"
$port = 5566

function Send-TcpCommand {
    param(
        [string]$Command
    )

    try {
        $client = New-Object System.Net.Sockets.TcpClient($host_ip, $port)
        $stream = $client.GetStream()
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

Write-Host "=== Cornea Controller TCP Test ===" -ForegroundColor Cyan
Write-Host "Target: $host_ip`:$port"
Write-Host ""

# Test 1: List Devices
Write-Host "[Test 1] listDevices" -ForegroundColor Yellow
$cmd = '{"cmd": "listDevices"}'
Write-Host "  Send: $cmd"
$response = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $response" -ForegroundColor Green
Write-Host ""

# Test 2: Get Status
Write-Host "[Test 2] getStatus" -ForegroundColor Yellow
$cmd = '{"cmd": "getStatus"}'
Write-Host "  Send: $cmd"
$response = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $response" -ForegroundColor Green
Write-Host ""

# Test 3: Refresh Devices
Write-Host "[Test 3] refreshDevices" -ForegroundColor Yellow
$cmd = '{"cmd": "refreshDevices"}'
Write-Host "  Send: $cmd"
$response = Send-TcpCommand -Command $cmd
Write-Host "  Recv: $response" -ForegroundColor Green
Write-Host ""

# Interactive mode
Write-Host "=== Interactive Mode ===" -ForegroundColor Cyan
Write-Host "Enter JSON command (or 'quit' to exit):"
Write-Host "Examples:"
Write-Host '  {"cmd": "listDevices"}'
Write-Host '  {"cmd": "powerOn", "serial": "LITE20240121"}'
Write-Host '  {"cmd": "setBrightness", "serial": "LITE20240121", "level": 0.03}'
Write-Host '  {"cmd": "sendImage", "serial": "LITE20240121", "path": "C:/test.png"}'
Write-Host '  {"cmd": "powerOff", "serial": "LITE20240121"}'
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
