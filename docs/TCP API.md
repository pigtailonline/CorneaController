

TCP Remote API

The CorneaWidget includes a built-in TCP server for remote control from other applications.

### Enable TCP Server

**Method 1: Via Configuration File (Recommended)**

```json
{
  "tcp": {
    "enabled": true,
    "port": 5566
  }
}

```

### 2 Protocol

- **Transport**: TCP
- **Port**: 5566 (configurable)
- **Format**: JSON, one command per line (terminated with `\n`)
- **Request**: `{"cmd": "command_name", ...params}\n`
- **Response**: `{"success": true/false, "data": {...}, "error": "..."}\n`

### 8.3 Commands

| Command | Parameters | Response | Description |
|---------|-----------|----------|-------------|
| `powerOn` | `serial` | `success` | Power on device |
| `powerOff` | `serial` | `success` | Power off device |
| `sendImage` | `serial`, `path` | `success` | Send image file (with APL check) |
| `setBrightness` | `serial`, `level` | `success` | Set brightness (0.0-1.0) + auto protection |
| `setFlip` | `serial`, `x`, `y` | `success` | Set X/Y flip |
| `isConnected` | `serial` | `connected` | Check connection status |
| `getPanelId` | `serial` | `panelId` | Get panel ID |
| `getTemperature` | `serial` | `temperature` | Get RJ1 temperature |
| `getStatus` | (none) | `devices[]` | Get all device status |
| `listDevices` | (none) | `devices[]` | List all device serials |
| `refreshDevices` | (none) | `deviceCount` | Refresh device list |

### 3 Examples

**Power on device:**
```json
{"cmd": "powerOn", "serial": "LITE20240101"}
```
Response:
```json
{"success": true}
```

**Send image:**
```json
{"cmd": "sendImage", "serial": "LITE20240101", "path": "D:/test/image.png"}
```

**Set brightness:**
```json
{"cmd": "setBrightness", "serial": "LITE20240101", "level": 0.05}
```
Response (immediate):
```json
{"success": true}
```
Note: Returns success immediately. Background protection monitors temperature for 5 seconds. If overheat detected, device auto powers off.

**Set flip:**
```json
{"cmd": "setFlip", "serial": "LITE20240101", "x": true, "y": false}
```

**List devices:**
```json
{"cmd": "listDevices"}
```
Response:
```json
{"success": true, "data": {"devices": ["LITE20240101", "LITE20240102"]}}
```

**Get full status:**
```json
{"cmd": "getStatus"}
```
Response:
```json
{
  "success": true,
  "data": {
    "deviceCount": 2,
    "devices": [
      {"serial": "LITE20240101", "connected": true, "panelId": "ABC123", "variant": "standard"},
      {"serial": "LITE20240102", "connected": false, "panelId": "", "variant": "F33R"}
    ]
  }
}
```

### 4 Python Client Example

```python
import socket
import json

def send_command(cmd, **params):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('localhost', 5566))

    request = {"cmd": cmd, **params}
    sock.send((json.dumps(request) + '\n').encode())

    response = sock.recv(4096).decode().strip()
    sock.close()

    return json.loads(response)

# Example usage
print(send_command("listDevices"))
print(send_command("powerOn", serial="LITE20240101"))
print(send_command("setBrightness", serial="LITE20240101", level=0.05))
print(send_command("sendImage", serial="LITE20240101", path="D:/test/image.png"))
print(send_command("powerOff", serial="LITE20240101"))
```

### 5 Error Handling

All errors return:
```json
{"success": false, "error": "Error message here"}
```

#### Common Errors

| Error | Description |
|-------|-------------|
| `Missing 'serial' parameter` | Required parameter not provided |
| `Device not found: XXX` | Serial number doesn't match any device |
| `Device not connected: XXX` | Device exists but is not powered on |
| `Unknown command: XXX` | Command name not recognized |

#### sendImage Specific Errors

| Error | Description |
|-------|-------------|
| `Failed to load image: path` | Image file not found or invalid format |
| `APL_EXCEEDED: Total APL X > limit Y` | Image brightness exceeds safety limit |

**APL Error Example:**
```json
{
  "success": false,
  "error": "APL_EXCEEDED: Total APL 0.0850 > limit 0.06 (pattern=0.8500, brightness=0.10)"
}
```

#### setBrightness Behavior

- Returns `success` immediately after setting brightness
- Background protection monitors temperature for 5 seconds
- If temperature > 55°C detected → device auto powers off
- Client can check device status with `isConnected` command

---

## 6 Safety Features

### 6.1 APL (Average Picture Level) Protection

Before sending any image, the system calculates Total APL:

```
Pattern APL = Σ[(R/255)^2.2 + (G/255)^2.2 + (B/255)^2.2] / (pixels × 3)
Total APL = Pattern APL × Brightness
```

**If Total APL > 0.06, image sending is BLOCKED.**

This prevents damage from sending bright images at high brightness settings.

### 6.2 Brightness Change Temperature Protection

When brightness is changed, the system **always** monitors temperature for 5 seconds:

| Check | Interval | Action if Temp > 55°C |
|-------|----------|----------------------|
| 1-5 | 1 second | Emergency power off |

**Behavior:**
- Returns success immediately (non-blocking)
- Background monitoring runs automatically
- If overheat detected → device auto powers off
- Log shows protection status and temperature readings

**Log Example:**
```
Setting brightness to 0.50
Brightness protection: monitoring temp for 5 seconds...
Protection check 1/5: RJ1=45.2°C, DA9272=42.1°C
Protection check 2/5: RJ1=48.5°C, DA9272=44.3°C
...
Brightness protection: completed, temp OK
```

**Overheat Log:**
```
Protection check 3/5: RJ1=56.2°C, DA9272=52.1°C
EMERGENCY: OVERHEAT during brightness change!
Temp: 56.2°C > 55.0°C limit
Emergency power off!
```

### 6.3 Continuous Temperature Monitoring

While connected, temperature is checked every 5 seconds. If temperature exceeds 55°C, the device is automatically powered off.
