# Cornea Controller Integration Guide

This guide explains how to set up the development environment and integrate the CorneaWidget into your Qt project.

---

###  Step 1 Manual Setup

## Part 1: Environment Setup
D:\cornea\env\python-3.12.7-amd64

Install options:
- Check "Install for all users"
- Check "Add Python to PATH"
- Default path: `C:\Python312`


### Step 2 Quick Setup (Recommended)

Use the provided setup script:

```powershell
# Navigate to Sunny Release directory
cd "D:\cornea\env\Sunny Release 2025.11.28"

# Run setup script (will install Python, create venv, install packages, setup drivers)
powershell -ExecutionPolicy Bypass -File .\setup_station.ps1

```

The script will automatically:
1. Check/Install Python 3.12.7
2. Create virtual environment `station_venv`
3. Install required wheel packages (rj1lib, ar_display_lab_lib)
4. Setup USB drivers (requires admin privileges)

---


#### Step 3: Install USB Drivers

Connect the Cornea device, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_drivers.ps1
```


1. D:\cornea\env\Sunny Release 2025.11.28\zadig.exe
2. Check if device is connected (USB VID_0403 & PID_6011)
3. cn_devboard (Composite Parent) //  Composite !!!
3. Install libusb-win32 driver for cn_devboard (Composite )


### Step 4 Verify Installation

```powershell
# Activate venv
.\station_venv\Scripts\Activate.ps1

# Test import
python -c "from ar_display_lab_lib.control_boards.cornea_rax720 import CorneaRax720; print('OK')"
```

---