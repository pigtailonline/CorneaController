#include "pythonbridge.h"

// Qt defines 'slots' as a macro, but Python/NumPy headers use 'slots' as a member name
// We need to temporarily undefine it before including Python headers
#ifdef slots
#undef slots
#endif

// Force release Python library even in debug builds (python312_d.lib is not included in standard Python)
#ifdef _DEBUG
#undef _DEBUG
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define _DEBUG
#else
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

// Restore Qt slots macro
#define slots Q_SLOTS

#include <QDebug>
#include <QDir>
#include <QFileInfo>

PythonBridge::PythonBridge(QObject *parent)
    : QObject(parent)
    , m_initialized(false)
    , m_module(nullptr)
    , m_corneaClass(nullptr)
    , m_numpyModule(nullptr)
    , m_nextInstanceId(0)
{
}

PythonBridge::~PythonBridge()
{
    shutdown();
}

bool PythonBridge::initialize(const QString &venvPath)
{
    // Call overloaded method with default values for backward compatibility
    QStringList defaultDllPaths;
    defaultDllPaths << "C:/Python312" << "D:/projects/deps/dll";
    return initialize(venvPath, "C:/Python312", defaultDllPaths, "C:/google_cal/hdf5_files");
}

bool PythonBridge::initialize(const QString &venvPath, const QString &pythonHome,
                               const QStringList &dllPaths, const QString &calPath)
{
    if (m_initialized) {
        return true;
    }

    m_venvPath = venvPath;
    m_pythonHome = pythonHome;
    m_dllPaths = dllPaths;
    m_calPath = calPath;

    // Set PYTHONHOME and PYTHONPATH environment variables BEFORE initializing Python
    if (!venvPath.isEmpty() && QDir(venvPath).exists()) {
        // Set environment variables for the venv
        QString sitePackages = venvPath + "/Lib/site-packages";
        qputenv("PYTHONHOME", m_pythonHome.toUtf8());
        qputenv("PYTHONPATH", sitePackages.toUtf8());

        // Add cv2 and configured DLL paths to PATH for DLL loading
        QString currentPath = qEnvironmentVariable("PATH");
        QString cv2Path = sitePackages + "/cv2";
        QStringList pathParts;
        pathParts << m_pythonHome << cv2Path << m_dllPaths << currentPath;
        QString newPath = pathParts.join(";");
        qputenv("PATH", newPath.toUtf8());

        QString pythonPath = venvPath + "/Scripts/python.exe";
        if (QFileInfo::exists(pythonPath)) {
            std::wstring wpath = pythonPath.toStdWString();
            Py_SetProgramName(wpath.c_str());
        }
    }

    // Initialize Python interpreter
    Py_Initialize();
    if (!Py_IsInitialized()) {
        setError("Failed to initialize Python interpreter");
        return false;
    }

    // Add venv site-packages to path BEFORE importing numpy
    if (!venvPath.isEmpty()) {
        QString sitePackages = venvPath + "/Lib/site-packages";
        PyObject *sysPath = PySys_GetObject("path");
        if (sysPath) {
            // Insert at the beginning of sys.path for priority
            PyObject *path = PyUnicode_FromString(sitePackages.toUtf8().constData());
            PyList_Insert(sysPath, 0, path);
            Py_DECREF(path);
        }
    }

    // Import numpy AFTER setting up paths
    import_array();
    if (PyErr_Occurred()) {
        setError("Failed to import numpy");
        PyErr_Print();
        return false;
    }

    // Add OpenCV DLL directory for Windows (required for cv2 to load properly)
    if (!venvPath.isEmpty()) {
        QString cv2Path = venvPath + "/Lib/site-packages/cv2";
        QString addDllCode = QString(
            "import os\n"
            "if hasattr(os, 'add_dll_directory'):\n"
            "    os.add_dll_directory(r'%1')\n"
        ).arg(cv2Path);

        // Add configured DLL paths
        for (const QString &dllPath : m_dllPaths) {
            addDllCode += QString("    os.add_dll_directory(r'%1')\n").arg(dllPath);
        }

        PyRun_SimpleString(addDllCode.toUtf8().constData());
    }

    // Configure Python output capture BEFORE importing any modules
    // This ensures we capture all log output from ar_display_lab_lib
    // Matches CLI behavior by using same log format and capturing all loggers
    PyRun_SimpleString(
        "import sys\n"
        "import io\n"
        "import logging\n"
        "\n"
        "# Create a buffer to capture stdout/stderr\n"
        "class OutputCapture:\n"
        "    def __init__(self):\n"
        "        self.buffer = []\n"
        "        self._noise_starts = ['^', '\\x1b', '\\r', '|', '/', '\\\\',\n"
        "                              'Traceback', 'File \"', 'TypeError', 'AttributeError',\n"
        "                              'AllK', 'Dynamically', 'pmic_mux']\n"
        "        self._noise_contains = ['HotK Handles', 'LstK Handles', 'LstInfoK', 'UsbK Handles',\n"
        "                                'DevK Handles', 'OvlK Handles', 'OvlPoolK', 'StmK Handles',\n"
        "                                'IsochK Handles', 'KLST_DEVINFO', 'HandleSize', 'PoolSize',\n"
        "                                'contiguous memory', 'bytes each', 'unexpected keyword argument',\n"
        "                                'Pcal6534.get_pin_level']\n"
        "    def write(self, text):\n"
        "        stripped = text.strip()\n"
        "        # Filter out noise: short strings, control chars\n"
        "        if not stripped or len(stripped) < 3:\n"
        "            return\n"
        "        # Skip control sequences and progress indicators\n"
        "        if any(stripped.startswith(p) for p in self._noise_starts):\n"
        "            return\n"
        "        # Skip libusbK memory allocation noise\n"
        "        if any(n in stripped for n in self._noise_contains):\n"
        "            return\n"
        "        # Skip lines that are just repeated single character\n"
        "        if len(set(stripped)) == 1 and len(stripped) > 3:\n"
        "            return\n"
        "        self.buffer.append(text.rstrip())\n"
        "    def flush(self):\n"
        "        pass\n"
        "    def get_and_clear(self):\n"
        "        result = self.buffer[:]\n"
        "        self.buffer.clear()\n"
        "        return result\n"
        "\n"
        "_qt_output_capture = OutputCapture()\n"
        "sys.stdout = _qt_output_capture\n"
        "sys.stderr = _qt_output_capture\n"
        "\n"
        "# Create a custom logging handler that captures to our buffer\n"
        "# Match CLI log format exactly (with milliseconds)\n"
        "class QtLogHandler(logging.Handler):\n"
        "    def emit(self, record):\n"
        "        msg = self.format(record)\n"
        "        _qt_output_capture.buffer.append(msg)\n"
        "\n"
        "_qt_log_handler = QtLogHandler()\n"
        "_qt_log_handler.setLevel(logging.INFO)\n"
        "_qt_log_handler.setFormatter(logging.Formatter(\n"
        "    '%(asctime)s,%(msecs)03d [%(levelname)-5.5s] : %(message)s',\n"
        "    datefmt='%Y-%m-%d %H:%M:%S'))\n"
        "\n"
        "# Install handler on root logger\n"
        "root_logger = logging.getLogger()\n"
        "root_logger.setLevel(logging.INFO)\n"
        "root_logger.addHandler(_qt_log_handler)\n"
        "\n"
        "# Silence noisy internal loggers\n"
        "for noisy in ['urllib3', 'PIL', 'matplotlib', 'numba', 'h5py', 'usb', 'pyftdi', 'ftdi']:\n"
        "    logging.getLogger(noisy).setLevel(logging.WARNING)\n"
        "\n"
        "# Intercept coloredlogs.install to prevent it from adding its own handlers\n"
        "# This ensures all log output goes through our handler\n"
        "def _intercept_coloredlogs():\n"
        "    try:\n"
        "        import coloredlogs\n"
        "        _original_install = coloredlogs.install\n"
        "        def _custom_install(level=logging.INFO, **kwargs):\n"
        "            # Set level but don't add coloredlogs handler\n"
        "            # Our QtLogHandler will handle all output\n"
        "            logging.getLogger().setLevel(level)\n"
        "        coloredlogs.install = _custom_install\n"
        "    except ImportError:\n"
        "        pass\n"
        "_intercept_coloredlogs()\n"
    );

    // Import ar_display_lab_lib
    if (!importModule("ar_display_lab_lib.control_boards.cornea_rax720")) {
        return false;
    }

    m_initialized = true;
    emit logMessage("Python bridge initialized successfully");
    return true;
}

void PythonBridge::shutdown()
{
    // Destroy all device instances
    QList<int> instanceIds = m_deviceInstances.keys();
    for (int id : instanceIds) {
        destroyDeviceInstance(id);
    }

    if (m_module) {
        Py_DECREF(m_module);
        m_module = nullptr;
    }
    if (m_corneaClass) {
        Py_DECREF(m_corneaClass);
        m_corneaClass = nullptr;
    }
    if (m_numpyModule) {
        Py_DECREF(m_numpyModule);
        m_numpyModule = nullptr;
    }

    if (m_initialized) {
        Py_Finalize();
        m_initialized = false;
    }
}

bool PythonBridge::importModule(const QString &moduleName)
{
    PyObject *name = PyUnicode_FromString(moduleName.toUtf8().constData());
    m_module = PyImport_Import(name);
    Py_DECREF(name);

    if (!m_module) {
        setError(QString("Failed to import module: %1").arg(moduleName));
        PyErr_Print();
        return false;
    }

    // Get CorneaRax720 class
    m_corneaClass = PyObject_GetAttrString(m_module, "CorneaRax720");
    if (!m_corneaClass) {
        setError("Failed to get CorneaRax720 class");
        PyErr_Print();
        return false;
    }

    return true;
}

QList<DeviceInfo> PythonBridge::getAvailableDevicesInfo()
{
    QMutexLocker locker(&m_apiMutex);

    QList<DeviceInfo> result;

    if (!m_corneaClass) {
        return result;
    }

    // Force USB device re-enumeration before querying
    // pyftdi caches USB device list, need to flush it
    PyRun_SimpleString(
        "try:\n"
        "    from pyftdi.usbtools import UsbTools\n"
        "    UsbTools.flush_cache()\n"
        "except Exception as e:\n"
        "    print(f'[pyftdi] Cache flush failed: {e}')\n"
    );
    PyErr_Clear();

    PyObject *method = PyObject_GetAttrString(m_corneaClass, "get_available_corneas");
    if (!method) {
        PyErr_Print();
        return result;
    }

    PyObject *pyResult = PyObject_CallNoArgs(method);
    Py_DECREF(method);

    if (pyResult && PyTuple_Check(pyResult) && PyTuple_Size(pyResult) >= 2) {
        PyObject *indices = PyTuple_GetItem(pyResult, 0);
        PyObject *serials = PyTuple_GetItem(pyResult, 1);

        if (PyList_Check(indices) && PyList_Check(serials)) {
            Py_ssize_t size = PyList_Size(indices);
            for (Py_ssize_t i = 0; i < size; ++i) {
                DeviceInfo info;
                PyObject *idxItem = PyList_GetItem(indices, i);
                PyObject *serialItem = PyList_GetItem(serials, i);

                info.index = static_cast<int>(PyLong_AsLong(idxItem));
                if (PyUnicode_Check(serialItem)) {
                    info.serial = QString::fromUtf8(PyUnicode_AsUTF8(serialItem));
                }
                info.displayName = QString("Index %1: %2").arg(info.index).arg(info.serial);
                result.append(info);
            }
        }
    }

    Py_XDECREF(pyResult);
    return result;
}

int PythonBridge::createDeviceInstance(int deviceIndex, const QString &hardwareVariant)
{
    QMutexLocker locker(&m_apiMutex);

    if (!m_initialized) {
        setError("Python bridge not initialized");
        return -1;
    }

    int instanceId = m_nextInstanceId++;

    emit logMessage(QString("[Instance %1] Creating CorneaRax720 for device index %2 (variant: %3)...")
                    .arg(instanceId).arg(deviceIndex).arg(hardwareVariant));

    /*

    CorneaRax720(
        program_rj1_regs: bool = True,
        program_rj1_luts: bool = False,
        use_discrete_pmics: bool = False,
        program_rj1_from_cal_file: bool = True,
        cal_path: str = None,
        logger_path: str = None,
        use_existing_logger_instance: bool = True,
        console_log_level: int = 20,
        append_log: bool = True,
        force_rj1_init: bool = False,
        init_cornea: bool = True,
        init_rj1: bool = True,
        cornea_index: int = 0,
        cornea_serial: str = None,
        cal_revision: int = None,
        rj1_use_i2c: bool = True,
        rj1_use_spi: bool = True,
        allow_default_hdf5: bool = False,
        power_rails: ar_display_lab_lib.control_boards.cornea_rax720.Rj1AxPowerRails = None,
        use_cal_from_flash: bool = True,
        rj1_version: Literal['Ax', 'B0', 'B0b', 'B1'] = None,
        spi_clk_freq: float = 15000000.0,
        hardware_variant: str = 'standard'
    )

    HARDWARE_VARIANTS = {
      "standard": HardwareVariantConfig(
          variant_name="V53/H79/PSA",
          pmic=   I2cDevice(Da9272,     0x6A),
          imu=    I2cDevice(LSM6DSV32X, 0x6B),
          vled_gb=I2cDevice(Fan53745,   0x20),
          vled_r= I2cDevice(Fan53745,   0x30),
      ),
      "F33R": HardwareVariantConfig(
          variant_name="F33 Right",
          pmic=   I2cDevice(Da9272,     0x7A),
          imu=    I2cDevice(LSM6DSV32X, 0x6A),
          vled_gb=I2cDevice(Fan53745,   0x20),
          vled_r= I2cDevice(Fan53745,   0x30),
      ),
      "F33L": HardwareVariantConfig(
          variant_name="F33 Left",
          pmic=   I2cDevice(Da9272,     0x7B),
          imu=    I2cDevice(LSM6DSV32X, 0x6B),
          vled_gb=I2cDevice(Fan53745,   0x20),
          vled_r= I2cDevice(Fan53745,   0x32),
      ),
  }

     */
    // Create CorneaRax720 instance with kwargs (same as CLI defaults)
    PyObject *kwargs = PyDict_New();
    PyDict_SetItemString(kwargs, "cornea_index", PyLong_FromLong(deviceIndex));
    PyDict_SetItemString(kwargs, "init_cornea", Py_True);
    PyDict_SetItemString(kwargs, "init_rj1", Py_True);
    PyDict_SetItemString(kwargs, "cal_path", PyUnicode_FromString(m_calPath.toUtf8().constData()));
    PyDict_SetItemString(kwargs, "rj1_use_i2c", Py_True);
    PyDict_SetItemString(kwargs, "rj1_use_spi", Py_True);
    PyDict_SetItemString(kwargs, "allow_default_hdf5", Py_False);
    PyDict_SetItemString(kwargs, "cal_revision", Py_None);
    PyDict_SetItemString(kwargs, "cornea_serial", Py_None);
    PyDict_SetItemString(kwargs, "rj1_version", Py_None);
    PyDict_SetItemString(kwargs, "spi_clk_freq", Py_None);
    PyDict_SetItemString(kwargs, "console_log_level", PyLong_FromLong(20));  // 20=INFO
    PyDict_SetItemString(kwargs, "hardware_variant", PyUnicode_FromString(hardwareVariant.toUtf8().constData()));

    PyObject *args = PyTuple_New(0);
    PyObject *instance = PyObject_Call(m_corneaClass, args, kwargs);
    Py_DECREF(args);
    Py_DECREF(kwargs);

    if (!instance) {
        setError(QString("Failed to create CorneaRax720 instance for device %1").arg(deviceIndex));
        PyErr_Print();
        m_nextInstanceId--;  // Rollback
        return -1;
    }

    // Check init_ok
    PyObject *initOk = PyObject_GetAttrString(instance, "init_ok");
    bool initOkValue = initOk && PyObject_IsTrue(initOk);
    Py_XDECREF(initOk);

    if (!initOkValue) {
        setError(QString("Hardware initialization failed for device %1 (init_ok=False)").arg(deviceIndex));
        Py_DECREF(instance);
        m_nextInstanceId--;  // Rollback
        return -1;
    }

    // Get serial number
    QString serial;
    PyObject *serialObj = PyObject_GetAttrString(instance, "cornea_serial");
    if (serialObj && PyUnicode_Check(serialObj)) {
        serial = QString::fromUtf8(PyUnicode_AsUTF8(serialObj));
    }
    Py_XDECREF(serialObj);

    // Store instance
    m_deviceInstances[instanceId] = instance;
    m_initOkMap[instanceId] = true;
    m_serialMap[instanceId] = serial;

    emit logMessage(QString("[Instance %1] Connected to device %2 (Serial: %3)")
                    .arg(instanceId).arg(deviceIndex).arg(serial));

    return instanceId;
}

void PythonBridge::destroyDeviceInstance(int instanceId)
{
    QMutexLocker locker(&m_apiMutex);

    if (!m_deviceInstances.contains(instanceId)) {
        return;
    }

    PyObject *instance = m_deviceInstances.take(instanceId);
    m_initOkMap.remove(instanceId);
    QString serial = m_serialMap.take(instanceId);

    // Power off before destroying
    PyObject *method = PyObject_GetAttrString(instance, "system_power_off");
    if (method) {
        PyObject *result = PyObject_CallNoArgs(method);
        Py_XDECREF(result);
        Py_DECREF(method);
    }
    PyErr_Clear();

    Py_DECREF(instance);
    emit logMessage(QString("[Instance %1] Disconnected (Serial: %2)").arg(instanceId).arg(serial));
}

bool PythonBridge::isDeviceConnected(int instanceId) const
{
    bool result = m_deviceInstances.contains(instanceId);
    if (!result) {
        qDebug() << "PythonBridge::isDeviceConnected: instanceId=" << instanceId
                 << "not found, available instances:" << m_deviceInstances.keys();
    }
    return result;
}

bool PythonBridge::isDeviceInitOk(int instanceId) const
{
    return m_initOkMap.value(instanceId, false);
}

QString PythonBridge::getDeviceSerial(int instanceId) const
{
    return m_serialMap.value(instanceId, QString());
}

PyObject* PythonBridge::getDeviceInstance(int instanceId) const
{
    return m_deviceInstances.value(instanceId, nullptr);
}

PyObject* PythonBridge::callInstanceMethod(int instanceId, const char *methodName, PyObject *args)
{
    QMutexLocker locker(&m_apiMutex);

    PyObject *instance = getDeviceInstance(instanceId);
    if (!instance) {
        setError(QString("Device instance %1 not found").arg(instanceId));
        return nullptr;
    }

    PyObject *method = PyObject_GetAttrString(instance, methodName);
    if (!method) {
        setError(QString("Method not found: %1").arg(methodName));
        PyErr_Print();
        return nullptr;
    }

    PyObject *result;
    if (args) {
        result = PyObject_CallObject(method, args);
    } else {
        result = PyObject_CallNoArgs(method);
    }

    Py_DECREF(method);

    if (!result) {
        setError(QString("Error calling method: %1").arg(methodName));
        PyErr_Print();
        return nullptr;
    }

    return result;
}

bool PythonBridge::systemPowerOn(int instanceId)
{
    PyObject *result = callInstanceMethod(instanceId, "system_power_on");
    if (result) {
        Py_DECREF(result);
        emit logMessage(QString("[Instance %1] System power ON").arg(instanceId));
        return true;
    }
    emit logMessage(QString("[Instance %1] System power ON FAILED: %2").arg(instanceId).arg(m_lastError));
    flushPythonOutput();
    return false;
}

bool PythonBridge::systemPowerOff(int instanceId)
{
    PyObject *result = callInstanceMethod(instanceId, "system_power_off");
    if (result) {
        Py_DECREF(result);
        emit logMessage(QString("[Instance %1] System power OFF").arg(instanceId));
        return true;
    }
    emit logMessage(QString("[Instance %1] System power OFF FAILED: %2").arg(instanceId).arg(m_lastError));
    flushPythonOutput();
    return false;
}

bool PythonBridge::enableVsys(int instanceId, bool enable)
{
    PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
    PyObject *result = callInstanceMethod(instanceId, "enable_vsys", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        emit logMessage(QString("[Instance %1] VSYS %2").arg(instanceId).arg(enable ? "enabled" : "disabled"));
        return true;
    }
    return false;
}

bool PythonBridge::setBrightness(int instanceId, double level)
{
    PyObject *args = PyTuple_Pack(1, PyFloat_FromDouble(level));
    PyObject *result = callInstanceMethod(instanceId, "set_brightness", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        emit logMessage(QString("[Instance %1] Brightness set to %2").arg(instanceId).arg(level));
        return true;
    }
    return false;
}

double PythonBridge::getBrightness(int instanceId)
{
    PyObject *result = callInstanceMethod(instanceId, "get_brightness");
    if (result) {
        double value = PyFloat_AsDouble(result);
        Py_DECREF(result);
        return value;
    }
    return -1.0;
}

bool PythonBridge::setXFlip(int instanceId, bool flip)
{
    // API: rj1_set_x_flip_offset(flip: bool, offset: int = None)
    // Pass flip value, offset=None (no change to offset)
    PyObject *args = PyTuple_Pack(2, flip ? Py_True : Py_False, Py_None);
    PyObject *result = callInstanceMethod(instanceId, "rj1_set_x_flip_offset", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        emit logMessage(QString("[Instance %1] X-Flip set to %2").arg(instanceId).arg(flip ? "true" : "false"));
        return true;
    }
    return false;
}

bool PythonBridge::setYFlip(int instanceId, bool flip)
{
    // API: rj1_set_y_flip_offset(flip: bool, offset: int = None)
    // Pass flip value, offset=None (no change to offset)
    PyObject *args = PyTuple_Pack(2, flip ? Py_True : Py_False, Py_None);
    PyObject *result = callInstanceMethod(instanceId, "rj1_set_y_flip_offset", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        emit logMessage(QString("[Instance %1] Y-Flip set to %2").arg(instanceId).arg(flip ? "true" : "false"));
        return true;
    }
    return false;
}

bool PythonBridge::getXFlip(int instanceId)
{
    // API: rj1_get_x_flip_offset() -> Tuple[bool, int]
    // Returns (flip, offset), we only need flip (index 0)
    PyObject *result = callInstanceMethod(instanceId, "rj1_get_x_flip_offset");
    if (result && PyTuple_Check(result) && PyTuple_Size(result) >= 1) {
        PyObject *flipObj = PyTuple_GetItem(result, 0);  // borrowed reference
        bool value = PyObject_IsTrue(flipObj);
        Py_DECREF(result);
        return value;
    }
    if (result) {
        Py_DECREF(result);
    }
    return false;
}

bool PythonBridge::getYFlip(int instanceId)
{
    // API: rj1_get_y_flip_offset() -> Tuple[bool, int]
    // Returns (flip, offset), we only need flip (index 0)
    PyObject *result = callInstanceMethod(instanceId, "rj1_get_y_flip_offset");
    if (result && PyTuple_Check(result) && PyTuple_Size(result) >= 1) {
        PyObject *flipObj = PyTuple_GetItem(result, 0);  // borrowed reference
        bool value = PyObject_IsTrue(flipObj);
        Py_DECREF(result);
        return value;
    }
    if (result) {
        Py_DECREF(result);
    }
    return false;
}

bool PythonBridge::writeFrame(int instanceId, const QImage &image)
{
    QMutexLocker locker(&m_apiMutex);

    PyObject *instance = getDeviceInstance(instanceId);
    if (!instance) {
        setError(QString("Device instance %1 not found").arg(instanceId));
        return false;
    }

    PyObject *pyArray = qimageToPyArray(image);
    if (!pyArray) {
        return false;
    }

    // Create kwargs
    PyObject *kwargs = PyDict_New();
    PyDict_SetItemString(kwargs, "opencv_frame", Py_True);

    PyObject *args = PyTuple_Pack(1, pyArray);

    PyObject *method = PyObject_GetAttrString(instance, "write_rj1_frame");
    PyObject *result = PyObject_Call(method, args, kwargs);

    Py_DECREF(method);
    Py_DECREF(args);
    Py_DECREF(kwargs);
    Py_DECREF(pyArray);

    if (result) {
        bool success = PyObject_IsTrue(result);
        Py_DECREF(result);
        if (success) {
            emit logMessage(QString("[Instance %1] Frame written successfully").arg(instanceId));
        }
        return success;
    }

    PyErr_Print();
    return false;
}

bool PythonBridge::writeFrameFromPath(int instanceId, const QString &imagePath)
{
    QImage image(imagePath);
    if (image.isNull()) {
        setError(QString("Failed to load image: %1").arg(imagePath));
        return false;
    }
    return writeFrame(instanceId, image);
}

PyObject* PythonBridge::qimageToPyArray(const QImage &image)
{
    // Convert to RGB888 format first
    QImage rgbImage = image.convertToFormat(QImage::Format_RGB888);

    // Create numpy array
    npy_intp dims[3] = {rgbImage.height(), rgbImage.width(), 3};
    PyObject *array = PyArray_SimpleNew(3, dims, NPY_UINT8);

    if (!array) {
        setError("Failed to create numpy array");
        return nullptr;
    }

    // Copy image data with RGB to BGR conversion (OpenCV format)
    // QImage RGB888 is R-G-B, but opencv_frame expects B-G-R
    uint8_t *data = static_cast<uint8_t*>(PyArray_DATA(reinterpret_cast<PyArrayObject*>(array)));
    for (int y = 0; y < rgbImage.height(); ++y) {
        const uchar *scanLine = rgbImage.constScanLine(y);
        for (int x = 0; x < rgbImage.width(); ++x) {
            int srcIdx = x * 3;
            int dstIdx = y * rgbImage.width() * 3 + x * 3;
            // Swap R and B channels: RGB -> BGR
            data[dstIdx + 0] = scanLine[srcIdx + 2];  // B <- R
            data[dstIdx + 1] = scanLine[srcIdx + 1];  // G <- G
            data[dstIdx + 2] = scanLine[srcIdx + 0];  // R <- B
        }
    }

    return array;
}

QVariantMap PythonBridge::getChipInfo(int instanceId)
{
    QVariantMap result;
    PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_chip_info");
    if (pyResult) {
        result = pyObjectToVariant(pyResult).toMap();
        Py_DECREF(pyResult);
    }
    return result;
}

QVariantMap PythonBridge::getChipInfoDecoded(int instanceId)
{
    QVariantMap result;
    PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
    if (pyResult) {
        result = pyObjectToVariant(pyResult).toMap();
        Py_DECREF(pyResult);
    }
    return result;
}

QString PythonBridge::getChipRevision(int instanceId)
{
    // Use get_rj1_chip_info_decoded() to get revision
    // Note: get_rj1_chip_revision() has a bug in the Python lib
    PyObject *result = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
    if (result && PyDict_Check(result)) {
        PyObject *revision = PyDict_GetItemString(result, "revision");
        if (revision) {
            QString rev;
            if (PyLong_Check(revision)) {
                rev = QString("B%1").arg(PyLong_AsLong(revision));  // Format as "B0", "B1", etc.
            } else if (PyUnicode_Check(revision)) {
                rev = QString::fromUtf8(PyUnicode_AsUTF8(revision));
            }
            Py_DECREF(result);
            if (!rev.isEmpty()) {
                return rev;
            }
        }
        Py_DECREF(result);
    }
    Py_XDECREF(result);
    PyErr_Clear();

    return QString();
}

double PythonBridge::getLeaTemperature(int instanceId)
{
    QMutexLocker locker(&m_apiMutex);

    // Call get_lea_temperature(sensor_sel="rax") to read RJ1 internal temp sensor
    PyObject *instance = getDeviceInstance(instanceId);
    if (!instance) {
        return -999.0;
    }

    PyObject *method = PyObject_GetAttrString(instance, "get_lea_temperature");
    if (!method) {
        PyErr_Clear();
        return -999.0;
    }

    // Create kwargs with sensor_sel="rax"
    PyObject *kwargs = PyDict_New();
    PyDict_SetItemString(kwargs, "sensor_sel", PyUnicode_FromString("rax"));

    PyObject *args = PyTuple_New(0);
    PyObject *result = PyObject_Call(method, args, kwargs);

    Py_DECREF(method);
    Py_DECREF(args);
    Py_DECREF(kwargs);

    if (result && result != Py_None) {
        double temp = -999.0;
        if (PyFloat_Check(result)) {
            temp = PyFloat_AsDouble(result);
        } else if (PyLong_Check(result)) {
            temp = static_cast<double>(PyLong_AsLong(result));
        }
        Py_DECREF(result);
        return temp;
    }
    Py_XDECREF(result);
    PyErr_Clear();
    return -999.0;
}

double PythonBridge::getDa9272Temperature(int instanceId)
{
    PyObject *result = callInstanceMethod(instanceId, "get_da9272_temperature");
    if (result && result != Py_None) {
        double temp = -999.0;
        if (PyFloat_Check(result)) {
            temp = PyFloat_AsDouble(result);
        } else if (PyLong_Check(result)) {
            temp = static_cast<double>(PyLong_AsLong(result));
        }
        Py_DECREF(result);
        return temp;
    }
    Py_XDECREF(result);
    PyErr_Clear();
    return -999.0;
}

QVariantMap PythonBridge::getPackageVersions(int instanceId)
{
    QVariantMap result;
    PyObject *pyResult = callInstanceMethod(instanceId, "get_pkg_versions");
    if (pyResult) {
        result = pyObjectToVariant(pyResult).toMap();
        Py_DECREF(pyResult);
    }
    return result;
}

QString PythonBridge::getPanelId(int instanceId)
{
    // Use get_rj1_chip_info_decoded() method which returns dict with "unique_chip_id_str"
    // This is the documented method per API docs
    PyObject *result = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
    if (result && PyDict_Check(result)) {
        PyObject *ucidStr = PyDict_GetItemString(result, "unique_chip_id_str");
        if (ucidStr && PyUnicode_Check(ucidStr)) {
            QString panelId = QString::fromUtf8(PyUnicode_AsUTF8(ucidStr));
            Py_DECREF(result);
            return panelId;
        }
        Py_DECREF(result);
    }
    Py_XDECREF(result);
    PyErr_Clear();

    return QString();
}

int PythonBridge::readRj1Register(int instanceId, int address)
{
    PyObject *args = PyTuple_Pack(1, PyLong_FromLong(address));
    PyObject *result = callInstanceMethod(instanceId, "read_rj1_reg", args);
    Py_DECREF(args);

    if (result) {
        int value = static_cast<int>(PyLong_AsLong(result));
        Py_DECREF(result);
        return value;
    }
    return -1;
}

bool PythonBridge::writeRj1Register(int instanceId, int address, int data)
{
    PyObject *args = PyTuple_Pack(2, PyLong_FromLong(address), PyLong_FromLong(data));
    PyObject *result = callInstanceMethod(instanceId, "write_rj1_reg", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        return true;
    }
    return false;
}

QVariantMap PythonBridge::getRj1Dacs(int instanceId)
{
    QVariantMap result;
    PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_dacs");
    if (pyResult) {
        result = pyObjectToVariant(pyResult).toMap();
        Py_DECREF(pyResult);
    }
    return result;
}

bool PythonBridge::setRj1Dacs(int instanceId, const QVariantMap &dacValues)
{
    PyObject *pyDict = PyDict_New();
    for (auto it = dacValues.begin(); it != dacValues.end(); ++it) {
        PyObject *key = PyUnicode_FromString(it.key().toUtf8().constData());
        PyObject *value = variantToPyObject(it.value());
        PyDict_SetItem(pyDict, key, value);
        Py_DECREF(key);
        Py_DECREF(value);
    }

    PyObject *args = PyTuple_Pack(1, pyDict);
    PyObject *result = callInstanceMethod(instanceId, "set_rj1_dacs", args);
    Py_DECREF(args);
    Py_DECREF(pyDict);

    if (result) {
        Py_DECREF(result);
        return true;
    }
    return false;
}

bool PythonBridge::setDemuraEnable(int instanceId, bool enable)
{
    PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
    PyObject *result = callInstanceMethod(instanceId, "rj1_demura_enable", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        return true;
    }
    return false;
}

bool PythonBridge::setRlutEnable(int instanceId, bool enable)
{
    PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
    PyObject *result = callInstanceMethod(instanceId, "rj1_rlut_enable", args);
    Py_DECREF(args);

    if (result) {
        Py_DECREF(result);
        return true;
    }
    return false;
}

QVariantMap PythonBridge::getDemuraRlutState(int instanceId)
{
    QVariantMap result;
    PyObject *pyResult = callInstanceMethod(instanceId, "rj1_get_demura_rlut_state");
    if (pyResult) {
        result = pyObjectToVariant(pyResult).toMap();
        Py_DECREF(pyResult);
    }
    return result;
}

QVariant PythonBridge::pyObjectToVariant(PyObject *obj)
{
    if (!obj || obj == Py_None) {
        return QVariant();
    }

    if (PyBool_Check(obj)) {
        return QVariant(obj == Py_True);
    }
    if (PyLong_Check(obj)) {
        return QVariant(static_cast<qlonglong>(PyLong_AsLongLong(obj)));
    }
    if (PyFloat_Check(obj)) {
        return QVariant(PyFloat_AsDouble(obj));
    }
    if (PyUnicode_Check(obj)) {
        return QVariant(QString::fromUtf8(PyUnicode_AsUTF8(obj)));
    }
    if (PyDict_Check(obj)) {
        QVariantMap map;
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            QString keyStr;
            if (PyUnicode_Check(key)) {
                keyStr = QString::fromUtf8(PyUnicode_AsUTF8(key));
            } else {
                keyStr = QString::number(pos);
            }
            map[keyStr] = pyObjectToVariant(value);
        }
        return QVariant(map);
    }
    if (PyList_Check(obj)) {
        QVariantList list;
        Py_ssize_t size = PyList_Size(obj);
        for (Py_ssize_t i = 0; i < size; ++i) {
            list.append(pyObjectToVariant(PyList_GetItem(obj, i)));
        }
        return QVariant(list);
    }
    if (PyTuple_Check(obj)) {
        QVariantList list;
        Py_ssize_t size = PyTuple_Size(obj);
        for (Py_ssize_t i = 0; i < size; ++i) {
            list.append(pyObjectToVariant(PyTuple_GetItem(obj, i)));
        }
        return QVariant(list);
    }

    return QVariant();
}

PyObject* PythonBridge::variantToPyObject(const QVariant &var)
{
    switch (var.type()) {
    case QVariant::Bool:
        return PyBool_FromLong(var.toBool() ? 1 : 0);
    case QVariant::Int:
    case QVariant::LongLong:
        return PyLong_FromLongLong(var.toLongLong());
    case QVariant::Double:
        return PyFloat_FromDouble(var.toDouble());
    case QVariant::String:
        return PyUnicode_FromString(var.toString().toUtf8().constData());
    case QVariant::Map: {
        PyObject *dict = PyDict_New();
        QVariantMap map = var.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            PyObject *key = PyUnicode_FromString(it.key().toUtf8().constData());
            PyObject *value = variantToPyObject(it.value());
            PyDict_SetItem(dict, key, value);
            Py_DECREF(key);
            Py_DECREF(value);
        }
        return dict;
    }
    case QVariant::List: {
        QVariantList list = var.toList();
        PyObject *pyList = PyList_New(list.size());
        for (int i = 0; i < list.size(); ++i) {
            PyList_SetItem(pyList, i, variantToPyObject(list[i]));
        }
        return pyList;
    }
    default:
        Py_RETURN_NONE;
    }
}

void PythonBridge::setError(const QString &error)
{
    m_lastError = error;
    emit errorOccurred(error);
    qWarning() << "PythonBridge Error:" << error;
}

void PythonBridge::clearError()
{
    m_lastError.clear();
}

void PythonBridge::flushPythonOutput()
{
    QMutexLocker locker(&m_apiMutex);

    if (!m_initialized) {
        return;
    }

    // Get captured output from Python
    PyObject *mainModule = PyImport_AddModule("__main__");
    if (!mainModule) {
        return;
    }

    PyObject *captureObj = PyObject_GetAttrString(mainModule, "_qt_output_capture");
    if (!captureObj) {
        PyErr_Clear();
        return;
    }

    PyObject *getMethod = PyObject_GetAttrString(captureObj, "get_and_clear");
    if (!getMethod) {
        Py_DECREF(captureObj);
        PyErr_Clear();
        return;
    }

    PyObject *result = PyObject_CallNoArgs(getMethod);
    Py_DECREF(getMethod);
    Py_DECREF(captureObj);

    if (!result) {
        PyErr_Clear();
        return;
    }

    if (PyList_Check(result)) {
        Py_ssize_t size = PyList_Size(result);
        for (Py_ssize_t i = 0; i < size; ++i) {
            PyObject *item = PyList_GetItem(result, i);
            if (PyUnicode_Check(item)) {
                QString message = QString::fromUtf8(PyUnicode_AsUTF8(item));
                emit logMessage(message);
            }
        }
    }

    Py_DECREF(result);
}
