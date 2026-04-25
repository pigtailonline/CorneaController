# TCP 遠端 API（中文版）

CorneaWidget 內建 TCP server，供其他應用程式遠端控制設備。

---

## 1. 啟用 TCP Server

在設定檔中加入：

```json
{
  "tcp": {
    "enabled": true,
    "port": 5566
  }
}
```

## 2. 通訊協定

- **傳輸層**：TCP
- **Port**：5566（可設定）
- **格式**：JSON，每行一個指令（以 `\n` 結尾）
- **請求**：`{"cmd": "指令名稱", ...參數}\n`
- **回應**：`{"success": true/false, "cmd": "指令名稱", "serial": "...", "data": {...}, "error": "..."}\n`

> **v1.0.6+**：每個回應都會帶回原始請求的 `cmd` 和 `serial` 欄位。這讓客戶端在並行發送多個指令時（例如同時控制 6 塊 panel）能精確匹配回應，不依賴 FIFO 順序。

## 3. 指令列表

### 3.1 設備控制

| 指令 | 參數 | 回應 | 說明 |
|------|------|------|------|
| `powerOn` | `serial`、`variant`（選填） | `success` | 開啟設備（可指定 variant） |
| `powerOff` | `serial` | `success` | 關閉設備 |
| `setBrightness` | `serial`、`level` | `success` | 設定亮度（0.0-1.0），自動啟動溫度保護 |
| `setFlip` | `serial`、`x`、`y` | `success` | 設定 X/Y 翻轉 |

### 3.2 圖片

| 指令 | 參數 | 回應 | 說明 |
|------|------|------|------|
| `sendImage` | `serial`、`path` | `success` | 用檔案路徑發送圖片（含 APL 檢查） |
| `sendImageByName` | `serial`、`name` | `success` | 用 TestChart 資料夾中的圖片名稱發送（含 APL 檢查） |
| `listImages` | （無） | `images[]`、`count` | 列出所有可用的 TestChart 圖片名稱 |

### 3.3 查詢

| 指令 | 參數 | 回應 | 說明 |
|------|------|------|------|
| `isConnected` | `serial` | `connected` | 檢查連線狀態 |
| `getPanelId` | `serial` | `panelId` | 取得 Panel ID |
| `getTemperature` | `serial` | `temperature` | 取得 RJ1 溫度 |
| `getStatus` | （無） | `devices[]` | 取得所有設備狀態 |
| `listDevices` | （無） | `devices[]` | 列出所有設備序號 |
| `refreshDevices` | （無） | `deviceCount` | 重新掃描設備 |

## 4. 使用範例

**開啟設備：**
```json
{"cmd": "powerOn", "serial": "LITE20240101"}
→ {"success": true, "cmd": "powerOn", "serial": "LITE20240101"}
```

**開啟設備並指定 variant：**
```json
{"cmd": "powerOn", "serial": "LITE20240101", "variant": "F33R"}
→ {"success": true, "cmd": "powerOn", "serial": "LITE20240101"}
```
支援的 variant：`standard`、`F33R`、`F33L`、`F33LP`

**關閉設備：**
```json
{"cmd": "powerOff", "serial": "LITE20240101"}
→ {"cmd": "powerOff", "serial": "LITE20240101", "success": true}
```

**用路徑發送圖片：**
```json
{"cmd": "sendImage", "serial": "LITE20240101", "path": "D:/test/image.png"}
→ {"cmd": "sendImage", "serial": "LITE20240101", "success": true}
```

**列出可用圖片：**
```json
{"cmd": "listImages"}
→ {"cmd": "listImages", "success": true, "data": {"images": ["720x720_Black.png", "720x720_White.png", ...], "count": 44}}
```

**用名稱發送圖片：**
```json
{"cmd": "sendImageByName", "serial": "LITE20240101", "name": "720x720_Black.png"}
→ {"cmd": "sendImageByName", "serial": "LITE20240101", "success": true}
```

**設定亮度：**
```json
{"cmd": "setBrightness", "serial": "LITE20240101", "level": 0.05}
→ {"cmd": "setBrightness", "serial": "LITE20240101", "success": true}
```
> 立即回傳成功。背景會監控溫度 5 秒，若偵測到過熱會自動關閉設備。

**設定翻轉：**
```json
{"cmd": "setFlip", "serial": "LITE20240101", "x": true, "y": false}
→ {"cmd": "setFlip", "serial": "LITE20240101", "success": true}
```

**檢查連線：**
```json
{"cmd": "isConnected", "serial": "LITE20240101"}
→ {"cmd": "isConnected", "serial": "LITE20240101", "success": true, "data": {"connected": true}}
```

**取得 Panel ID：**
```json
{"cmd": "getPanelId", "serial": "LITE20240101"}
→ {"cmd": "getPanelId", "serial": "LITE20240101", "success": true, "data": {"panelId": "ABC123"}}
```

**取得溫度：**
```json
{"cmd": "getTemperature", "serial": "LITE20240101"}
→ {"cmd": "getTemperature", "serial": "LITE20240101", "success": true, "data": {"temperature": 42.5}}
```

**列出設備：**
```json
{"cmd": "listDevices"}
→ {"cmd": "listDevices", "success": true, "data": {"devices": ["LITE20240101", "LITE20240102"]}}
```

**取得完整狀態：**
```json
{"cmd": "getStatus"}
→ {"cmd": "getStatus", "success": true, "data": {"deviceCount": 2, "devices": [{"serial": "LITE20240101", "connected": true, "panelId": "ABC123", "variant": "standard"}, {"serial": "LITE20240102", "connected": false, "panelId": "", "variant": "F33R"}]}}
```

**重新掃描設備：**
```json
{"cmd": "refreshDevices"}
→ {"cmd": "refreshDevices", "success": true, "data": {"deviceCount": 2}}
```

## 5. Python 客戶端範例

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

# 使用範例
print(send_command("listDevices"))                                              # 列出設備
print(send_command("powerOn", serial="LITE20240101"))                           # 開啟設備
print(send_command("powerOn", serial="LITE20240101", variant="F33R"))           # 指定 variant 開啟
print(send_command("setBrightness", serial="LITE20240101", level=0.05))         # 設定亮度
print(send_command("listImages"))                                               # 列出可用圖片
print(send_command("sendImageByName", serial="LITE20240101", name="720x720_Black.png"))  # 用名稱發送
print(send_command("sendImage", serial="LITE20240101", path="D:/test/image.png"))        # 用路徑發送
print(send_command("powerOff", serial="LITE20240101"))                          # 關閉設備
```

## 6. 錯誤處理

所有錯誤回傳格式：
```json
{"success": false, "error": "錯誤訊息"}
```

### 6.1 通用錯誤

| 錯誤訊息 | 說明 |
|----------|------|
| `Missing 'serial' parameter` | 缺少必要參數 |
| `Device not found: XXX` | 序號不存在 |
| `Device not connected: XXX` | 設備未開啟 |
| `Unknown command: XXX` | 不認識的指令 |

### 6.2 sendImage / sendImageByName 錯誤

| 錯誤訊息 | 說明 |
|----------|------|
| `Failed to load image: path` | 圖片檔案不存在或格式不支援 |
| `Image not found: name` | 圖片名稱不在已載入的 TestChart 中 |
| `APL_EXCEEDED: Total APL X > limit Y` | 圖片亮度超過安全限制 |

APL 錯誤範例：
```json
{"success": false, "error": "APL_EXCEEDED: Total APL 0.0850 > limit 0.06 (pattern=0.8500, brightness=0.10)"}
```

## 7. 安全機制

### 7.1 APL（平均亮度等級）保護

發送圖片前，系統會計算 Total APL：

```
Pattern APL = Σ[(R/255)^2.2 + (G/255)^2.2 + (B/255)^2.2] / (像素數 × 3)
Total APL = Pattern APL × 亮度
```

**若 Total APL > 0.06，圖片發送會被阻擋。**

### 7.2 亮度變更溫度保護

變更亮度後，系統自動監控溫度 5 秒：

| 檢查次數 | 間隔 | 溫度 > 60°C 時的動作 |
|----------|------|---------------------|
| 1-5 | 1 秒 | 緊急關閉設備 |

- 指令立即回傳成功（非阻塞）
- 背景自動執行溫度監控
- 偵測到過熱 → 自動關閉設備

### 7.3 持續溫度監控

設備連線期間，每 5 秒檢查一次溫度。若溫度超過 55°C，設備會自動關閉。
