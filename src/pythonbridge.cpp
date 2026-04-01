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

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <functional>

// ---------------------------------------------------------------------------
// Dispatch helpers: run work on the dedicated Python thread.
// BlockingQueuedConnection blocks the caller until the lambda finishes.
// If already on the Python thread, run directly to avoid deadlock.
// ---------------------------------------------------------------------------
template<typename F>
auto PythonBridge::dispatch(F &&f) -> decltype(f()) {
    if (QThread::currentThread() == &m_pythonThread) {
        return f();
    }
    decltype(f()) result{};
    QMetaObject::invokeMethod(m_pythonWorker, [&f, &result]() {
        result = f();
    }, Qt::BlockingQueuedConnection);
    return result;
}

void PythonBridge::dispatchVoid(std::function<void()> f) {
    if (QThread::currentThread() == &m_pythonThread) {
        f();
        return;
    }
    QMetaObject::invokeMethod(m_pythonWorker, std::move(f), Qt::BlockingQueuedConnection);
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
PythonBridge::PythonBridge(QObject *parent)
    : QObject(parent)
    , m_initialized(false)
    , m_module(nullptr)
    , m_corneaClass(nullptr)
    , m_numpyModule(nullptr)
    , m_executor(nullptr)
    , m_allowDefaultHdf5(false)
    , m_nextInstanceId(0)
{
    m_pythonWorker = new QObject();
    m_pythonWorker->moveToThread(&m_pythonThread);
    m_pythonThread.start();
}

PythonBridge::~PythonBridge()
{
    shutdown();
    m_pythonThread.quit();
    m_pythonThread.wait();
    delete m_pythonWorker;
    m_pythonWorker = nullptr;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

#ifdef QT_DEBUG
// Simulation sleep helper (debug builds only)
static void simSleep(int ms) { QThread::msleep(ms); }
#endif

bool PythonBridge::initialize(const QString &venvPath)
{
    QStringList defaultDllPaths;
    defaultDllPaths << "C:/Python312" << "D:/projects/deps/dll";
    return initialize(venvPath, "C:/Python312", defaultDllPaths, "C:/google_cal/hdf5_files", false);
}

bool PythonBridge::initialize(const QString &venvPath, const QString &pythonHome,
                               const QStringList &dllPaths, const QString &calPath,
                               bool allowDefaultHdf5)
{
#ifdef QT_DEBUG
    emit logMessage("[SIM] Debug simulation mode — skipping Python initialization");
    m_initialized = true;
    return true;
#endif
    // Dispatch entire initialization to the Python worker thread.
    // Python interpreter will be created on that thread.
    return dispatch([=]() -> bool {
        if (m_initialized) {
            return true;
        }

        m_venvPath = venvPath;
        m_pythonHome = pythonHome;
        m_dllPaths = dllPaths;
        m_calPath = calPath;
        m_allowDefaultHdf5 = allowDefaultHdf5;

        // Set PYTHONHOME and PYTHONPATH environment variables BEFORE initializing Python
        if (!venvPath.isEmpty() && QDir(venvPath).exists()) {
            QString sitePackages = venvPath + "/Lib/site-packages";
            qputenv("PYTHONHOME", m_pythonHome.toUtf8());
            qputenv("PYTHONPATH", sitePackages.toUtf8());

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

        Py_Initialize();
        if (!Py_IsInitialized()) {
            setError("Failed to initialize Python interpreter");
            return false;
        }

        if (!venvPath.isEmpty()) {
            QString sitePackages = venvPath + "/Lib/site-packages";
            PyObject *sysPath = PySys_GetObject("path");
            if (sysPath) {
                PyObject *path = PyUnicode_FromString(sitePackages.toUtf8().constData());
                PyList_Insert(sysPath, 0, path);
                Py_DECREF(path);
            }
        }

        if (_import_array() < 0) {
            setError("Failed to import numpy");
            PyErr_Print();
            return false;
        }

        if (!venvPath.isEmpty()) {
            QString cv2Path = venvPath + "/Lib/site-packages/cv2";
            QString addDllCode = QString(
                "import os\n"
                "if hasattr(os, 'add_dll_directory'):\n"
                "    os.add_dll_directory(r'%1')\n"
            ).arg(cv2Path);
            for (const QString &dllPath : m_dllPaths) {
                addDllCode += QString("    os.add_dll_directory(r'%1')\n").arg(dllPath);
            }
            PyRun_SimpleString(addDllCode.toUtf8().constData());
        }

        // Configure Python output capture
        PyRun_SimpleString(
            "import sys\n"
            "import io\n"
            "import logging\n"
            "\n"
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
            "        if not stripped or len(stripped) < 3:\n"
            "            return\n"
            "        if any(stripped.startswith(p) for p in self._noise_starts):\n"
            "            return\n"
            "        if any(n in stripped for n in self._noise_contains):\n"
            "            return\n"
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
            "root_logger = logging.getLogger()\n"
            "root_logger.setLevel(logging.INFO)\n"
            "root_logger.addHandler(_qt_log_handler)\n"
            "\n"
            "for noisy in ['urllib3', 'PIL', 'matplotlib', 'numba', 'h5py', 'usb', 'pyftdi', 'ftdi']:\n"
            "    logging.getLogger(noisy).setLevel(logging.WARNING)\n"
            "\n"
            "def _intercept_coloredlogs():\n"
            "    try:\n"
            "        import coloredlogs\n"
            "        _original_install = coloredlogs.install\n"
            "        def _custom_install(level=logging.INFO, **kwargs):\n"
            "            logging.getLogger().setLevel(level)\n"
            "        coloredlogs.install = _custom_install\n"
            "    except ImportError:\n"
            "        pass\n"
            "_intercept_coloredlogs()\n"
        );

        if (!importModule("ar_display_lab_lib.control_boards.cornea_rax720")) {
            return false;
        }

        // Monkey-patch CorneaRax720.__init__ to serialize construction.
        // rj1lib_external.config has shared module-level state (CHIP, CFGBits,
        // OPTBits, regs, subregs) that select_chip() modifies during init.
        // Concurrent constructors cause race conditions on this shared state.
        // The lock serializes constructors only; other operations (setBrightness,
        // writeFrame, etc.) are unaffected.
        PyRun_SimpleString(
            "def _patch_cornea_init():\n"
            "    try:\n"
            "        import threading\n"
            "        import ar_display_lab_lib.control_boards.cornea_rax720 as _mod\n"
            "        _init_lock = threading.Lock()\n"
            "        _orig_init = _mod.CorneaRax720.__init__\n"
            "        def _locked_init(self, *args, **kwargs):\n"
            "            with _init_lock:\n"
            "                _orig_init(self, *args, **kwargs)\n"
            "        _mod.CorneaRax720.__init__ = _locked_init\n"
            "    except Exception as e:\n"
            "        print(f'Warning: failed to patch CorneaRax720 init: {e}')\n"
            "_patch_cornea_init()\n"
        );

        // Create a ThreadPoolExecutor for parallel device creation.
        // Device I/O (USB) releases the GIL, so multiple creations can overlap.
        PyObject *cfModule = PyImport_ImportModule("concurrent.futures");
        if (cfModule) {
            PyObject *executorClass = PyObject_GetAttrString(cfModule, "ThreadPoolExecutor");
            if (executorClass) {
                PyObject *kwargs = PyDict_New();
                PyDict_SetItemString(kwargs, "max_workers", PyLong_FromLong(12));
                PyObject *args = PyTuple_New(0);
                m_executor = PyObject_Call(executorClass, args, kwargs);
                Py_DECREF(args);
                Py_DECREF(kwargs);
                Py_DECREF(executorClass);
            }
            Py_DECREF(cfModule);
        }
        if (!m_executor) {
            emit logMessage("Warning: ThreadPoolExecutor not available, device creation will be serial");
            PyErr_Clear();
        }

        m_initialized = true;
        emit logMessage("Python bridge initialized successfully");
        return true;
    });
}

void PythonBridge::shutdown()
{
#ifdef QT_DEBUG
    m_simDevices.clear();
    m_serialMap.clear();
    m_initOkMap.clear();
    m_initialized = false;
    return;
#endif
    dispatchVoid([this]() {
        QList<int> instanceIds = m_deviceInstances.keys();
        for (int id : instanceIds) {
            // destroyDeviceInstance dispatches, but we're already on Python thread → runs directly
            destroyDeviceInstance(id);
        }

        if (m_initialized) {
            // Shutdown ThreadPoolExecutor (wait for pending tasks)
            if (m_executor) {
                PyObject *shutdownKw = PyDict_New();
                PyDict_SetItemString(shutdownKw, "wait", Py_True);
                PyObject *shutdownMethod = PyObject_GetAttrString(m_executor, "shutdown");
                if (shutdownMethod) {
                    PyObject *r = PyObject_Call(shutdownMethod, PyTuple_New(0), shutdownKw);
                    Py_XDECREF(r);
                    Py_DECREF(shutdownMethod);
                }
                Py_DECREF(shutdownKw);
                Py_DECREF(m_executor);
                m_executor = nullptr;
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

            Py_Finalize();
            m_initialized = false;
        }
    });
}

// ---------------------------------------------------------------------------
// Internal helpers (always called on Python thread — no dispatch needed)
// ---------------------------------------------------------------------------
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

    m_corneaClass = PyObject_GetAttrString(m_module, "CorneaRax720");
    if (!m_corneaClass) {
        setError("Failed to get CorneaRax720 class");
        PyErr_Print();
        return false;
    }

    return true;
}

PyObject* PythonBridge::callInstanceMethod(int instanceId, const char *methodName, PyObject *args)
{
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

PyObject* PythonBridge::getDeviceInstance(int instanceId) const
{
    return m_deviceInstances.value(instanceId, nullptr);
}

PyObject* PythonBridge::qimageToPyArray(const QImage &image)
{
    QImage rgbImage = image.convertToFormat(QImage::Format_RGB888);

    npy_intp dims[3] = {rgbImage.height(), rgbImage.width(), 3};
    PyObject *array = PyArray_SimpleNew(3, dims, NPY_UINT8);

    if (!array) {
        setError("Failed to create numpy array");
        return nullptr;
    }

    uint8_t *data = static_cast<uint8_t*>(PyArray_DATA(reinterpret_cast<PyArrayObject*>(array)));
    for (int y = 0; y < rgbImage.height(); ++y) {
        const uchar *scanLine = rgbImage.constScanLine(y);
        for (int x = 0; x < rgbImage.width(); ++x) {
            int srcIdx = x * 3;
            int dstIdx = y * rgbImage.width() * 3 + x * 3;
            data[dstIdx + 0] = scanLine[srcIdx + 2];  // B <- R
            data[dstIdx + 1] = scanLine[srcIdx + 1];  // G <- G
            data[dstIdx + 2] = scanLine[srcIdx + 0];  // R <- B
        }
    }

    return array;
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

// ---------------------------------------------------------------------------
// Public API methods — each dispatches its entire body to the Python thread
// ---------------------------------------------------------------------------

QList<DeviceInfo> PythonBridge::getAvailableDevicesInfo()
{
#ifdef QT_DEBUG
    QList<DeviceInfo> result;
    for (int i = 0; i < SIM_DEVICE_COUNT; ++i) {
        DeviceInfo info;
        info.index = i;
        info.serial = QString("SIM%1").arg(i, 4, 10, QChar('0'));
        info.displayName = QString("Index %1: %2").arg(i).arg(info.serial);
        result.append(info);
    }
    return result;
#endif
    return dispatch([this]() -> QList<DeviceInfo> {
        QList<DeviceInfo> result;
        if (!m_corneaClass) {
            return result;
        }

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
    });
}

int PythonBridge::createDeviceInstance(int deviceIndex, const QString &hardwareVariant)
{
#ifdef QT_DEBUG
    int instanceId = m_nextInstanceId++;
    QString serial = QString("SIM%1").arg(deviceIndex, 4, 10, QChar('0'));
    emit logMessage(QString("[SIM] Creating device instance %1 for device %2 (%3)...")
                        .arg(instanceId).arg(deviceIndex).arg(serial));
    simSleep(SIM_DELAY_MS);

    SimDevice sim;
    sim.serial = serial;
    sim.connected = true;
    m_simDevices[instanceId] = sim;
    m_initOkMap[instanceId] = true;
    m_serialMap[instanceId] = serial;

    emit logMessage(QString("[SIM] Device instance %1 created: %2").arg(instanceId).arg(serial));
    return instanceId;
#endif
    return dispatch([this, deviceIndex, hardwareVariant]() -> int {
        if (!m_initialized) {
            setError("Python bridge not initialized");
            return -1;
        }

        int instanceId = m_nextInstanceId++;

        emit logMessage(QString("[Instance %1] Creating CorneaRax720 for device index %2 (variant: %3)...")
                        .arg(instanceId).arg(deviceIndex).arg(hardwareVariant));

        PyObject *kwargs = PyDict_New();
        PyDict_SetItemString(kwargs, "cornea_index", PyLong_FromLong(deviceIndex));
        PyDict_SetItemString(kwargs, "init_cornea", Py_True);
        PyDict_SetItemString(kwargs, "init_rj1", Py_True);
        PyDict_SetItemString(kwargs, "cal_path", PyUnicode_FromString(m_calPath.toUtf8().constData()));
        PyDict_SetItemString(kwargs, "rj1_use_i2c", Py_True);
        PyDict_SetItemString(kwargs, "rj1_use_spi", Py_True);
        PyDict_SetItemString(kwargs, "allow_default_hdf5", m_allowDefaultHdf5 ? Py_True : Py_False);
        PyDict_SetItemString(kwargs, "cal_revision", Py_None);
        PyDict_SetItemString(kwargs, "cornea_serial", Py_None);
        PyDict_SetItemString(kwargs, "rj1_version", Py_None);
        PyDict_SetItemString(kwargs, "spi_clk_freq", Py_None);
        PyDict_SetItemString(kwargs, "console_log_level", PyLong_FromLong(20));
        PyDict_SetItemString(kwargs, "hardware_variant", PyUnicode_FromString(hardwareVariant.toUtf8().constData()));

        PyObject *instance = nullptr;

        if (m_executor) {
            // Submit to ThreadPoolExecutor for parallel execution.
            // executor.submit(CorneaRax720, **kwargs)
            PyObject *submitMethod = PyObject_GetAttrString(m_executor, "submit");
            PyObject *submitArgs = PyTuple_Pack(1, m_corneaClass);
            PyObject *future = PyObject_Call(submitMethod, submitArgs, kwargs);
            Py_DECREF(submitArgs);
            Py_DECREF(submitMethod);
            Py_DECREF(kwargs);

            if (!future) {
                setError(QString("Failed to submit device %1 to executor").arg(deviceIndex));
                PyErr_Print();
                m_nextInstanceId--;
                return -1;
            }

            // Poll until the future completes.
            // Between polls: release GIL (let executor threads run Python)
            // then re-acquire and processEvents (let other dispatches in).
            while (true) {
                PyObject *done = PyObject_CallMethod(future, "done", nullptr);
                bool isDone = done && PyObject_IsTrue(done);
                Py_XDECREF(done);
                if (isDone) break;

                // Release GIL so executor threads can run Python code
                Py_BEGIN_ALLOW_THREADS
                QThread::msleep(10);
                Py_END_ALLOW_THREADS

                // GIL held again: process queued dispatches from other panels
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }

            // Get result (GIL held)
            instance = PyObject_CallMethod(future, "result", nullptr);
            Py_DECREF(future);
        } else {
            // Fallback: direct call (serial, no parallelism)
            PyObject *args = PyTuple_New(0);
            instance = PyObject_Call(m_corneaClass, args, kwargs);
            Py_DECREF(args);
            Py_DECREF(kwargs);
        }

        if (!instance) {
            // Capture the actual Python exception details
            QString pyError;
            PyObject *pType, *pValue, *pTraceback;
            PyErr_Fetch(&pType, &pValue, &pTraceback);
            if (pValue) {
                PyObject *pStr = PyObject_Str(pValue);
                if (pStr && PyUnicode_Check(pStr)) {
                    pyError = QString::fromUtf8(PyUnicode_AsUTF8(pStr));
                }
                Py_XDECREF(pStr);
            }
            QString typeName;
            if (pType) {
                PyObject *tName = PyObject_GetAttrString(pType, "__name__");
                if (tName && PyUnicode_Check(tName)) {
                    typeName = QString::fromUtf8(PyUnicode_AsUTF8(tName));
                }
                Py_XDECREF(tName);
            }
            Py_XDECREF(pType);
            Py_XDECREF(pValue);
            Py_XDECREF(pTraceback);

            QString fullError = QString("Failed to create device %1: [%2] %3")
                                    .arg(deviceIndex).arg(typeName, pyError);
            setError(fullError);
            emit logMessage(QString("[Instance %1] FAILED: %2").arg(instanceId).arg(fullError));
            return -1;
        }

        PyObject *initOk = PyObject_GetAttrString(instance, "init_ok");
        bool initOkValue = initOk && PyObject_IsTrue(initOk);
        Py_XDECREF(initOk);

        if (!initOkValue) {
            // Try to extract UCID even if init failed — panel might have been detected
            QString ucid;
            PyObject *stateVals = PyObject_GetAttrString(instance, "state_vals");
            if (stateVals && PyDict_Check(stateVals)) {
                PyObject *ucidObj = PyDict_GetItemString(stateVals, "unique_chip_id_str");
                if (ucidObj && PyUnicode_Check(ucidObj)) {
                    ucid = QString::fromUtf8(PyUnicode_AsUTF8(ucidObj));
                }
            }
            Py_XDECREF(stateVals);

            QString detail;
            if (!ucid.isEmpty()) {
                detail = QString("Hardware initialization failed for device %1.\n"
                                 "Panel detected: UCID = %2\n"
                                 "Please provide the HDF5 calibration file for this UCID and place it in the cal_path folder.")
                         .arg(deviceIndex).arg(ucid);
            } else {
                detail = QString("Hardware initialization failed for device %1 (init_ok=False)").arg(deviceIndex);
            }
            setError(detail);
            emit logMessage(QString("[Instance %1] FAILED: %2").arg(instanceId).arg(detail));
            Py_DECREF(instance);
            return -1;
        }

        QString serial;
        PyObject *serialObj = PyObject_GetAttrString(instance, "cornea_serial");
        if (serialObj && PyUnicode_Check(serialObj)) {
            serial = QString::fromUtf8(PyUnicode_AsUTF8(serialObj));
        }
        Py_XDECREF(serialObj);

        m_deviceInstances[instanceId] = instance;
        m_initOkMap[instanceId] = true;
        m_serialMap[instanceId] = serial;

        emit logMessage(QString("[Instance %1] Connected to device %2 (Serial: %3)")
                        .arg(instanceId).arg(deviceIndex).arg(serial));

        return instanceId;
    });
}

void PythonBridge::destroyDeviceInstance(int instanceId)
{
#ifdef QT_DEBUG
    m_simDevices.remove(instanceId);
    m_initOkMap.remove(instanceId);
    m_serialMap.remove(instanceId);
    emit logMessage(QString("[SIM] Device instance %1 destroyed").arg(instanceId));
    return;
#endif
    dispatchVoid([this, instanceId]() {
        if (!m_deviceInstances.contains(instanceId)) {
            return;
        }

        PyObject *instance = m_deviceInstances.take(instanceId);
        m_initOkMap.remove(instanceId);
        QString serial = m_serialMap.take(instanceId);

        PyObject *method = PyObject_GetAttrString(instance, "system_power_off");
        if (method) {
            PyObject *result = PyObject_CallNoArgs(method);
            Py_XDECREF(result);
            Py_DECREF(method);
        }
        PyErr_Clear();

        Py_DECREF(instance);
        emit logMessage(QString("[Instance %1] Disconnected (Serial: %2)").arg(instanceId).arg(serial));
    });
}

bool PythonBridge::isDeviceConnected(int instanceId) const
{
#ifdef QT_DEBUG
    return m_simDevices.contains(instanceId) && m_simDevices[instanceId].connected;
#endif
    // Map is only modified on Python thread, reads from main thread are safe
    // for QMap (non-concurrent reads are OK if no concurrent write).
    // Since callers typically check this before dispatching Python work,
    // and modifications happen sequentially on the Python thread, this is safe.
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

bool PythonBridge::systemPowerOn(int instanceId)
{
#ifdef QT_DEBUG
    emit logMessage(QString("[SIM] systemPowerOn(%1)").arg(instanceId));
    simSleep(SIM_DELAY_MS);
    return m_simDevices.contains(instanceId);
#endif
    return dispatch([this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "system_power_on");
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] System power ON").arg(instanceId));
            return true;
        }
        emit logMessage(QString("[Instance %1] System power ON FAILED: %2").arg(instanceId).arg(m_lastError));
        flushPythonOutput();
        return false;
    });
}

bool PythonBridge::systemPowerOff(int instanceId)
{
#ifdef QT_DEBUG
    emit logMessage(QString("[SIM] systemPowerOff(%1)").arg(instanceId));
    simSleep(SIM_DELAY_MS);
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].connected = false;
    return true;
#endif
    return dispatch([this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "system_power_off");
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] System power OFF").arg(instanceId));
            return true;
        }
        emit logMessage(QString("[Instance %1] System power OFF FAILED: %2").arg(instanceId).arg(m_lastError));
        flushPythonOutput();
        return false;
    });
}

bool PythonBridge::enableVsys(int instanceId, bool enable)
{
#ifdef QT_DEBUG
    Q_UNUSED(enable);
    return m_simDevices.contains(instanceId);
#endif
    return dispatch([this, instanceId, enable]() -> bool {
        PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
        PyObject *result = callInstanceMethod(instanceId, "enable_vsys", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] VSYS %2").arg(instanceId).arg(enable ? "enabled" : "disabled"));
            return true;
        }
        return false;
    });
}

bool PythonBridge::setBrightness(int instanceId, double level)
{
#ifdef QT_DEBUG
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].brightness = level;
    return m_simDevices.contains(instanceId);
#endif
    return dispatch([this, instanceId, level]() -> bool {
        PyObject *args = PyTuple_Pack(1, PyFloat_FromDouble(level));
        PyObject *result = callInstanceMethod(instanceId, "set_brightness", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] Brightness set to %2").arg(instanceId).arg(level));
            return true;
        }
        return false;
    });
}

double PythonBridge::getBrightness(int instanceId)
{
#ifdef QT_DEBUG
    return m_simDevices.contains(instanceId) ? m_simDevices[instanceId].brightness : -1.0;
#endif
    return dispatch([this, instanceId]() -> double {
        PyObject *result = callInstanceMethod(instanceId, "get_brightness");
        if (result) {
            double value = PyFloat_AsDouble(result);
            Py_DECREF(result);
            return value;
        }
        return -1.0;
    });
}

bool PythonBridge::setXFlip(int instanceId, bool flip)
{
#ifdef QT_DEBUG
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].xFlip = flip;
    return m_simDevices.contains(instanceId);
#endif
    return dispatch([this, instanceId, flip]() -> bool {
        PyObject *args = PyTuple_Pack(2, flip ? Py_True : Py_False, Py_None);
        PyObject *result = callInstanceMethod(instanceId, "rj1_set_x_flip_offset", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] X-Flip set to %2").arg(instanceId).arg(flip ? "true" : "false"));
            return true;
        }
        return false;
    });
}

bool PythonBridge::setYFlip(int instanceId, bool flip)
{
#ifdef QT_DEBUG
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].yFlip = flip;
    return m_simDevices.contains(instanceId);
#endif
    return dispatch([this, instanceId, flip]() -> bool {
        PyObject *args = PyTuple_Pack(2, flip ? Py_True : Py_False, Py_None);
        PyObject *result = callInstanceMethod(instanceId, "rj1_set_y_flip_offset", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] Y-Flip set to %2").arg(instanceId).arg(flip ? "true" : "false"));
            return true;
        }
        return false;
    });
}

bool PythonBridge::getXFlip(int instanceId)
{
#ifdef QT_DEBUG
    return m_simDevices.contains(instanceId) ? m_simDevices[instanceId].xFlip : false;
#endif
    return dispatch([this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "rj1_get_x_flip_offset");
        if (result && PyTuple_Check(result) && PyTuple_Size(result) >= 1) {
            PyObject *flipObj = PyTuple_GetItem(result, 0);
            bool value = PyObject_IsTrue(flipObj);
            Py_DECREF(result);
            return value;
        }
        Py_XDECREF(result);
        return false;
    });
}

bool PythonBridge::getYFlip(int instanceId)
{
#ifdef QT_DEBUG
    return m_simDevices.contains(instanceId) ? m_simDevices[instanceId].yFlip : false;
#endif
    return dispatch([this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "rj1_get_y_flip_offset");
        if (result && PyTuple_Check(result) && PyTuple_Size(result) >= 1) {
            PyObject *flipObj = PyTuple_GetItem(result, 0);
            bool value = PyObject_IsTrue(flipObj);
            Py_DECREF(result);
            return value;
        }
        Py_XDECREF(result);
        return false;
    });
}

bool PythonBridge::writeFrame(int instanceId, const QImage &image)
{
#ifdef QT_DEBUG
    Q_UNUSED(image);
    emit logMessage(QString("[SIM] writeFrame(%1) %2x%3").arg(instanceId).arg(image.width()).arg(image.height()));
    simSleep(SIM_DELAY_MS);
    return m_simDevices.contains(instanceId);
#endif
    return dispatch([this, instanceId, image]() -> bool {
        PyObject *instance = getDeviceInstance(instanceId);
        if (!instance) {
            setError(QString("Device instance %1 not found").arg(instanceId));
            return false;
        }

        PyObject *pyArray = qimageToPyArray(image);
        if (!pyArray) {
            return false;
        }

        PyObject *method = PyObject_GetAttrString(instance, "write_rj1_frame");
        if (!method) {
            setError(QString("write_rj1_frame not found on instance %1").arg(instanceId));
            PyErr_Clear();
            Py_DECREF(pyArray);
            return false;
        }

        PyObject *kwargs = PyDict_New();
        PyDict_SetItemString(kwargs, "opencv_frame", Py_True);

        PyObject *args = PyTuple_Pack(1, pyArray);
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
    });
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

QVariantMap PythonBridge::getChipInfo(int instanceId)
{
#ifdef QT_DEBUG
    QVariantMap info;
    info["chip"] = "SIM_RJ1B1";
    info["revision"] = "B1";
    info["instance"] = instanceId;
    return info;
#endif
    return dispatch([this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_chip_info");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QVariantMap PythonBridge::getChipInfoDecoded(int instanceId)
{
#ifdef QT_DEBUG
    return getChipInfo(instanceId);
#endif
    return dispatch([this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QString PythonBridge::getChipRevision(int instanceId)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId);
    return "B1";
#endif
    return dispatch([this, instanceId]() -> QString {
        PyObject *result = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
        if (result && PyDict_Check(result)) {
            PyObject *revision = PyDict_GetItemString(result, "revision");
            if (revision) {
                QString rev;
                if (PyLong_Check(revision)) {
                    rev = QString("B%1").arg(PyLong_AsLong(revision));
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
    });
}

double PythonBridge::getLeaTemperature(int instanceId)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId);
    return 25.0 + QRandomGenerator::global()->bounded(5.0);
#endif
    return dispatch([this, instanceId]() -> double {
        PyObject *instance = getDeviceInstance(instanceId);
        if (!instance) {
            return -999.0;
        }

        PyObject *method = PyObject_GetAttrString(instance, "get_lea_temperature");
        if (!method) {
            PyErr_Clear();
            return -999.0;
        }

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
    });
}

double PythonBridge::getDa9272Temperature(int instanceId)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId);
    return 24.0 + QRandomGenerator::global()->bounded(4.0);
#endif
    return dispatch([this, instanceId]() -> double {
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
    });
}

QVariantMap PythonBridge::getPackageVersions(int instanceId)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId);
    QVariantMap v;
    v["rj1lib"] = "1.0.0-sim";
    v["ar_display_lab_lib"] = "1.0.0-sim";
    return v;
#endif
    return dispatch([this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_pkg_versions");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QString PythonBridge::getPanelId(int instanceId)
{
#ifdef QT_DEBUG
    return QString("SIM-PANEL-%1").arg(instanceId);
#endif
    return dispatch([this, instanceId]() -> QString {
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
    });
}

int PythonBridge::readRj1Register(int instanceId, int address)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId); Q_UNUSED(address);
    return 0;
#endif
    return dispatch([this, instanceId, address]() -> int {
        PyObject *args = PyTuple_Pack(1, PyLong_FromLong(address));
        PyObject *result = callInstanceMethod(instanceId, "read_rj1_reg", args);
        Py_DECREF(args);
        if (result) {
            int value = static_cast<int>(PyLong_AsLong(result));
            Py_DECREF(result);
            return value;
        }
        return -1;
    });
}

bool PythonBridge::writeRj1Register(int instanceId, int address, int data)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId); Q_UNUSED(address); Q_UNUSED(data);
    return true;
#endif
    return dispatch([this, instanceId, address, data]() -> bool {
        PyObject *args = PyTuple_Pack(2, PyLong_FromLong(address), PyLong_FromLong(data));
        PyObject *result = callInstanceMethod(instanceId, "write_rj1_reg", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

QVariantMap PythonBridge::getRj1Dacs(int instanceId)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId);
    return QVariantMap();
#endif
    return dispatch([this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_dacs");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

bool PythonBridge::setRj1Dacs(int instanceId, const QVariantMap &dacValues)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId); Q_UNUSED(dacValues);
    return true;
#endif
    return dispatch([this, instanceId, dacValues]() -> bool {
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
    });
}

bool PythonBridge::setDemuraEnable(int instanceId, bool enable)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId); Q_UNUSED(enable);
    return true;
#endif
    return dispatch([this, instanceId, enable]() -> bool {
        PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
        PyObject *result = callInstanceMethod(instanceId, "rj1_demura_enable", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

bool PythonBridge::setRlutEnable(int instanceId, bool enable)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId); Q_UNUSED(enable);
    return true;
#endif
    return dispatch([this, instanceId, enable]() -> bool {
        PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
        PyObject *result = callInstanceMethod(instanceId, "rj1_rlut_enable", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

QVariantMap PythonBridge::getDemuraRlutState(int instanceId)
{
#ifdef QT_DEBUG
    Q_UNUSED(instanceId);
    QVariantMap state;
    state["demura"] = true;
    state["rlut"] = true;
    return state;
#endif
    return dispatch([this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "rj1_get_demura_rlut_state");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

void PythonBridge::flushPythonOutput()
{
#ifdef QT_DEBUG
    return;
#endif
    // Use QueuedConnection (non-blocking) so this never blocks the main thread.
    // When Python worker is busy with a long operation (e.g. powerOn ~10s),
    // BlockingQueuedConnection would freeze the UI until that operation finishes.
    QMetaObject::invokeMethod(m_pythonWorker, [this]() {
        if (!m_initialized) {
            return;
        }

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
    }, Qt::QueuedConnection);
}
