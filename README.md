# Cornea Controller Integration Guide

This guide explains how to set up the development environment and integrate the CorneaWidget into your Qt project.

---

## Part 1: Environment Setup

### 1.1 Quick Setup (Recommended)

Use the provided setup script:

```powershell
# Navigate to Sunny Release directory
cd "D:\projects\src\google driver\Sunny Release 2025.11.28"

# Run setup script (will install Python, create venv, install packages, setup drivers)
.\setup_station.ps1
```

The script will automatically:
1. Check/Install Python 3.12.7
2. Create virtual environment `station_venv`
3. Install required wheel packages (rj1lib, ar_display_lab_lib)
4. Setup USB drivers (requires admin privileges)

---

### 1.2 Manual Setup

If you prefer manual installation:

#### Step 1: Install Python 3.12

Download and install Python 3.12.7:
```
https://www.python.org/ftp/python/3.12.7/python-3.12.7-amd64.exe
```

Install options:
- Check "Install for all users"
- Check "Add Python to PATH"
- Default path: `C:\Python312`

#### Step 2: Install Qt 5.15.2

1. Download Qt Online Installer from [qt.io](https://www.qt.io/download)
2. Install Qt 5.15.2 with MSVC 2019 64-bit compiler
3. Default path: `C:\Qt\5.15.2\msvc2019_64`

#### Step 3: Create Virtual Environment

```powershell
cd "D:\projects\src\google driver\Sunny Release 2025.11.28"

# Install virtualenv
python -m pip install virtualenv

# Create virtual environment
python -m venv station_venv

# Upgrade pip
python -m pip install --upgrade pip

# Activate virtual environment
.\station_venv\Scripts\Activate.ps1
```

#### Step 4: Install Python Packages

```powershell
# With virtual environment activated
# Install wheel files in the directory
Get-ChildItem -Path *.whl | ForEach-Object { python -m pip install $_.FullName }
```

The wheel files include:
- `rj1lib-*.whl` - RJ1 driver library
- `ar_display_lab_lib-*.whl` - Google AR Display Lab library

#### Step 5: Install USB Drivers

Connect the Cornea device, then run:

```powershell
.\setup_drivers.ps1
```

This script will:
1. Check if device is connected (USB VID_0403 & PID_6011)
2. Download Zadig if needed
3. Install libusb-win32 driver for cn_devboard

Driver location: `C:\usb_driver\cn_devboard_(Composite_Parent).inf`

### 1.3 Prepare Calibration Files

Place HDF5 calibration files in:
```
C:\google_cal\hdf5_files
```

### 1.4 Verify Installation

```powershell
# Activate venv
.\station_venv\Scripts\Activate.ps1

# Test import
python -c "from ar_display_lab_lib.control_boards.cornea_rax720 import CorneaRax720; print('OK')"
```

---

## Part 2: Integration Guide

### 2.1 Required Files

Copy these files to your project:

```
your_project/
├── cornea/
│   ├── corneawidget.h
│   ├── corneawidget.cpp
│   ├── corneawidget.ui
│   ├── corneaconfig.h
│   ├── corneaconfig.cpp
│   ├── pythonbridge.h
│   ├── pythonbridge.cpp
│   ├── corneacontroller.h
│   ├── corneacontroller.cpp
│   ├── devicecontrolpanel.h
│   ├── devicecontrolpanel.cpp
│   ├── imageloader.h
│   └── imageloader.cpp
├── cornea_config.json
└── your_main.cpp
```

### 2.2 Update Your .pro File

```pro
QT += core gui widgets

CONFIG += c++17

# Python Configuration
PYTHON_PATH = C:/Python312
VENV_PATH = "D:/your_project/station_venv"

INCLUDEPATH += $$PYTHON_PATH/include
INCLUDEPATH += $$VENV_PATH/Lib/site-packages/numpy/_core/include

LIBS += -L$$PYTHON_PATH/libs -lpython312

# Add Cornea source files
SOURCES += \
    cornea/corneawidget.cpp \
    cornea/corneaconfig.cpp \
    cornea/pythonbridge.cpp \
    cornea/corneacontroller.cpp \
    cornea/devicecontrolpanel.cpp \
    cornea/imageloader.cpp

HEADERS += \
    cornea/corneawidget.h \
    cornea/corneaconfig.h \
    cornea/pythonbridge.h \
    cornea/corneacontroller.h \
    cornea/devicecontrolpanel.h \
    cornea/imageloader.h

FORMS += \
    cornea/corneawidget.ui

# Copy Python DLL to output
win32 {
    QMAKE_POST_LINK += $$quote(cmd /c copy /Y \"$$PYTHON_PATH\\python312.dll\" \"$$OUT_PWD\")
}
```

### 2.3 Configuration File (cornea_config.json)

```json
{
  "tcp": {
    "enabled": true,
    "port": 5566
  },
  "python": {
    "venv_path": "D:/your_project/station_venv",
    "python_home": "C:/Python312",
    "dll_paths": [
      "C:/Python312",
      "D:/projects/deps/dll"
    ],
    "cal_path": "C:/google_cal/hdf5_files"
  },
  "devices": [
    { "serial": "LITE20240101", "variant": "standard", "brightness": 0.03 },
    { "serial": "LITE20240102", "variant": "F33R", "brightness": 0.02 },
    null,
    null,
    null,
    null,
    null,
    null,
    null,
    null,
    null,
    null
  ]
}
```

#### Configuration Fields

| Field | Description |
|-------|-------------|
| `tcp.enabled` | Enable TCP server on startup (default: false) |
| `tcp.port` | TCP server port (default: 5566) |
| `python.venv_path` | Path to Python virtual environment |
| `python.python_home` | Python installation directory |
| `python.dll_paths` | Additional DLL search paths |
| `python.cal_path` | Path to HDF5 calibration files |
| `devices` | Array of device configurations (max 12) |
| `devices[].serial` | Device serial number |
| `devices[].variant` | Hardware variant: `standard`, `F33R`, or `F33L` |
| `devices[].brightness` | Default brightness (0.0 - 1.0) |

**Note:** New devices are automatically added to the first `null` slot when discovered.

---

## Part 3: Usage Examples

### 3.1 Basic Usage (UI + Programmatic Control)

```cpp
#include "cornea/corneawidget.h"

class MyMainWindow : public QMainWindow
{
    Q_OBJECT

private:
    CorneaWidget *m_cornea;

public:
    MyMainWindow(QWidget *parent = nullptr) : QMainWindow(parent)
    {
        // Create CorneaWidget
        m_cornea = new CorneaWidget(this);

        // Load configuration
        m_cornea->loadConfig("cornea_config.json");

        // Add to layout (shows full UI with image testing)
        setCentralWidget(m_cornea);

        // Connect signals
        connect(m_cornea, &CorneaWidget::deviceConnected,
                this, &MyMainWindow::onDeviceConnected);
        connect(m_cornea, &CorneaWidget::deviceDisconnected,
                this, &MyMainWindow::onDeviceDisconnected);
    }

private slots:
    void onDeviceConnected(int index, const QString &serial)
    {
        qDebug() << "Device connected:" << index << serial;
    }

    void onDeviceDisconnected(int index, const QString &serial)
    {
        qDebug() << "Device disconnected:" << index << serial;
    }
};
```

### 3.2 Programmatic Control Only

```cpp
#include "cornea/corneawidget.h"

void runAutomatedTest()
{
    // Create widget (can be hidden)
    CorneaWidget cornea;
    cornea.loadConfig("cornea_config.json");
    cornea.hide();  // Optional: hide UI

    // Power on device at index 0
    if (cornea.powerOn(0)) {
        qDebug() << "Device 0 powered on";

        // Set brightness
        cornea.setBrightness(0, 0.05);

        // Send image from file path
        cornea.sendImage(0, "D:/test_images/pattern.png");

        // Or send QImage directly
        QImage img(720, 720, QImage::Format_RGB888);
        img.fill(Qt::red);
        cornea.sendImage(0, img);

        // Power off when done
        cornea.powerOff(0);
    }

    // Save config (includes any new devices found)
    cornea.saveConfig();
}
```

### 3.3 Multi-Device Control

```cpp
void controlMultipleDevices(CorneaWidget *cornea)
{
    // Power on multiple devices
    cornea->powerOn(0);  // First device in config
    cornea->powerOn(1);  // Second device in config

    // Send same image to all connected devices
    QString imagePath = "D:/test/image.png";

    for (int i = 0; i < CorneaWidget::MAX_DEVICES; i++) {
        if (cornea->isConnected(i)) {
            cornea->sendImage(i, imagePath);
        }
    }

    // Set different brightness for each device
    cornea->setBrightness(0, 0.03);
    cornea->setBrightness(1, 0.05);

    // Power off all
    for (int i = 0; i < CorneaWidget::MAX_DEVICES; i++) {
        if (cornea->isConnected(i)) {
            cornea->powerOff(i);
        }
    }
}
```

### 3.4 Embed in Existing UI

```cpp
// In your existing window's setup code
void MyExistingWindow::setupCorneaPanel()
{
    // Create cornea widget
    m_cornea = new CorneaWidget(this);
    m_cornea->loadConfig("cornea_config.json");

    // Add to a specific area in your UI
    ui->corneaContainer->layout()->addWidget(m_cornea);

    // Or add to a dock widget
    QDockWidget *dock = new QDockWidget("Cornea Controller", this);
    dock->setWidget(m_cornea);
    addDockWidget(Qt::RightDockWidgetArea, dock);
}
```

---

## Part 4: API Reference

### 4.1 CorneaWidget Public Methods

| Method | Return | Description |
|--------|--------|-------------|
| `loadConfig(const QString &path)` | `bool` | Load JSON configuration file |
| `saveConfig()` | `bool` | Save current configuration |
| `powerOn(int index)` | `bool` | Power on device at index (0-11) |
| `powerOff(int index)` | `bool` | Power off device at index |
| `sendImage(int index, const QString &path)` | `bool` | Send image from file path |
| `sendImage(int index, const QImage &image)` | `bool` | Send QImage directly |
| `setBrightness(int index, double level)` | `bool` | Set brightness (0.0-1.0) |
| `isConnected(int index)` | `bool` | Check if device is connected |
| `isConfigured(int index)` | `bool` | Check if device is configured |
| `getSerial(int index)` | `QString` | Get device serial number |
| `getConnectedCount()` | `int` | Get number of connected devices |
| `config()` | `CorneaConfig*` | Access configuration object |

### 4.2 Signals

```cpp
// Emitted when a device connects
void deviceConnected(int index, const QString &serial);

// Emitted when a device disconnects
void deviceDisconnected(int index, const QString &serial);

// Emitted for log messages
void logMessage(const QString &message);
```

### 4.3 Constants

```cpp
static const int MAX_DEVICES = 12;  // Maximum supported devices
```

---

## Part 5: Hardware Variants

The Cornea RAX720 supports three hardware variants with different I2C addresses:

| Variant | Description | Use Case |
|---------|-------------|----------|
| `standard` | V53/H79/PSA | Default configuration |
| `F33R` | F33 Right | F33 right eye module |
| `F33L` | F33 Left | F33 left eye module |

Set the variant in the configuration file or through the UI dropdown before powering on.

---

## Part 6: Troubleshooting

### Python Initialization Failed

1. Check `venv_path` points to a valid virtual environment
2. Verify `python_home` matches your Python installation
3. Ensure all required packages are installed in the venv

### Device Not Found

1. Check USB connection
2. Install FTDI drivers
3. Click "Refresh Devices" in the UI

### Image Send Failed

1. Ensure device is powered on (`isConnected()` returns true)
2. Check image dimensions (720x720 recommended)
3. Verify calibration files exist in `cal_path`

### DLL Load Errors

1. Verify all paths in `dll_paths` exist
2. Copy `python312.dll` to the executable directory
3. Check for missing Visual C++ Redistributable

---

## Part 7: File Structure

```
CorneaController/
├── src/
│   ├── main.cpp                 # Application entry point
│   ├── corneawidget.h/cpp/ui    # Main widget (reusable)
│   ├── corneaconfig.h/cpp       # JSON configuration
│   ├── pythonbridge.h/cpp       # Python C API bridge
│   ├── corneacontroller.h/cpp   # Device controller
│   ├── devicecontrolpanel.h/cpp # Per-device UI panel
│   ├── imageloader.h/cpp        # Image loading utilities
│   └── tcpserver.h/cpp          # TCP server for remote control
├── docs/
│   └── INTEGRATION_GUIDE.md     # This document
├── cornea_config.json           # Configuration file
├── CorneaController.pro         # qmake project file
└── CMakeLists.txt               # CMake project file
```

---

## Part 8: TCP Remote API

The CorneaWidget includes a built-in TCP server for remote control from other applications.

### 8.1 Enable TCP Server

**Method 1: Via Configuration File (Recommended)**

```json
{
  "tcp": {
    "enabled": true,
    "port": 5566
  }
}
```

The TCP server will automatically start when `loadConfig()` is called.

**Method 2: Programmatic Control**

```cpp
// In your application startup
CorneaWidget *cornea = new CorneaWidget();
cornea->loadConfig("cornea_config.json");

// Start TCP server on port 5566 (default)
cornea->startTcpServer(5566);

// Or with custom port
cornea->startTcpServer(8080);

// Stop server when done
cornea->stopTcpServer();
```

### 8.2 Protocol

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

### 8.4 Examples

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

### 8.5 Python Client Example

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

### 8.6 Error Handling

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

## Part 9: Safety Features

### 9.1 APL (Average Picture Level) Protection

Before sending any image, the system calculates Total APL:

```
Pattern APL = Σ[(R/255)^2.2 + (G/255)^2.2 + (B/255)^2.2] / (pixels × 3)
Total APL = Pattern APL × Brightness
```

**If Total APL > 0.06, image sending is BLOCKED.**

This prevents damage from sending bright images at high brightness settings.

### 9.2 Brightness Change Temperature Protection

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

### 9.3 Continuous Temperature Monitoring

While connected, temperature is checked every 5 seconds. If temperature exceeds 55°C, the device is automatically powered off.
