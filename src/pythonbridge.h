#ifndef PYTHONBRIDGE_H
#define PYTHONBRIDGE_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QImage>
#include <QMap>
#include <QList>
#include <QThread>
#include <QMutex>
#include <QRandomGenerator>
#include <QJsonObject>
#include <QAtomicInteger>
#include <memory>
#include <functional>

class PanelSubprocess;

// Forward declare Python types to avoid including Python.h in header
struct _object;
typedef _object PyObject;

// Device information structure
struct DeviceInfo {
    int index;
    QString serial;
    QString displayName;  // "Index 0: SERIAL"
};

class PythonBridge : public QObject
{
    Q_OBJECT

public:
    enum class SessionState {
        Ready,
        Recovering,
        Unavailable,
    };

    // A command's identity is kept separate from the worker RPC id.  The
    // latter is local to one subprocess and can be reused after replacement;
    // requestId + workerGeneration are what make late results harmless.
    struct CommandContext {
        QString requestId;
        QString serial;
        QString command;
        qint64 deadlineMs = 0;
        bool active = true;
        quint64 workerGeneration = 0;
    };

    explicit PythonBridge(QObject *parent = nullptr);
    ~PythonBridge();

    // Initialization (Python interpreter - call once)
    bool initialize(const QString &venvPath);
    bool initialize(const QString &venvPath, const QString &pythonHome,
                    const QStringList &dllPaths, const QString &calPath,
                    bool allowDefaultHdf5 = false);
    void setSpiClkFreq(double freq) { m_spiClkFreq = freq; }
    double spiClkFreq() const { return m_spiClkFreq; }
    void setLogApl(bool on) { m_logApl = on; }
    bool logApl() const { return m_logApl; }
    void setSerializeUsb(bool on) { m_serializeUsb = on; }
    bool serializeUsb() const { return m_serializeUsb; }
    void setDefectMapOutputRoot(const QString &path) { m_defectMapOutputRoot = path; }

    // Driver-owned deadlines.  The upstream client remains at 15 seconds;
    // the initial Power budget is deliberately shorter and independently
    // configurable from worker/recovery budgets.
    static constexpr int DEFAULT_POWER_DEADLINE_MS = 14000;
    static constexpr int DEFAULT_RECOVERY_BUDGET_MS = 5000;
    void setPowerDeadlineMs(int timeoutMs) {
        m_powerDeadlineMs = qBound(1, timeoutMs, 14999);
    }
    int powerDeadlineMs() const { return m_powerDeadlineMs.loadRelaxed(); }
    void setRecoveryBudgetMs(int timeoutMs) {
        m_recoveryBudgetMs = qMax(1, timeoutMs);
    }
    int recoveryBudgetMs() const { return m_recoveryBudgetMs.loadRelaxed(); }

    // Phase 2 subprocess mode toggles. When enabled, createDeviceInstance
    // spawns a python.exe child running panel_worker.py and routes the
    // critical-path operations (powerOn/Off, setBrightness, sendImage,
    // getPanelId, getLeaTemperature) through it instead of the embedded
    // interpreter — sidesteps the cross-panel GIL contention that caused
    // 4+ panel cascade fails. workerScript defaults to
    // <appdir>/python/panel_worker.py; pythonExe defaults to the venv's
    // Scripts/python.exe relative to the configured venvPath.
    void setUseSubprocess(bool on) { m_useSubprocess = on; }
    bool useSubprocess() const { return m_useSubprocess; }
    void setWorkerScript(const QString &path) { m_workerScript = path; }
    void setSubprocessPythonExe(const QString &path) { m_subprocessPythonExe = path; }
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Device enumeration
    QList<DeviceInfo> getAvailableDevicesInfo();

    // Multi-instance device management
    int createDeviceInstance(int deviceIndex, const QString &hardwareVariant = "standard",
                             const QString &expectedSerial = QString(),
                             QString *error = nullptr);  // Returns instanceId, -1 on failure
    int preInitDeviceInstance(int deviceIndex, const QString &hardwareVariant = "standard",
                              const QString &expectedSerial = QString()); // Light init: FTDI only, no RJ1/panel needed
    void destroyDeviceInstance(int instanceId);
    bool isDeviceConnected(int instanceId) const;
    bool isDeviceInitOk(int instanceId) const;
    QString getDeviceSerial(int instanceId) const;
    SessionState sessionState(int instanceId) const;
    SessionState sessionStateForSerial(const QString &serial) const;
    quint64 workerGeneration(int instanceId) const;
    static QString sessionStateName(SessionState state);

    // Power control (per instance)
    bool systemPowerOn(int instanceId, QString *error = nullptr);
    bool systemPowerOff(int instanceId);
    bool enableVsys(int instanceId, bool enable);

    // Brightness control (per instance)
    bool setBrightness(int instanceId, double level, QString *error = nullptr);
    bool setBrightnessWithDeadline(int instanceId, double level,
                                   QString *error, int commandDeadlineMs);
    double getBrightness(int instanceId);

    // Flip control (per instance) - CalMetadata.x_flip / y_flip
    bool setXFlip(int instanceId, bool flip);
    bool setYFlip(int instanceId, bool flip);
    bool getXFlip(int instanceId);
    bool getYFlip(int instanceId);

    // Image operations (per instance)
    bool writeFrame(int instanceId, const QImage &image, QString *error = nullptr);
    bool writeFrameFromPath(int instanceId, const QString &imagePath);

    // Information retrieval (per instance)
    QVariantMap getChipInfo(int instanceId);
    QVariantMap getChipInfoDecoded(int instanceId);
    QString getChipRevision(int instanceId);
    double getLeaTemperature(int instanceId);   // RJ1-RAX internal temp sensor
    double getDa9272Temperature(int instanceId);
    // Power measurements (for Harriet "Panel Tester" report)
    // Returns {vsys_power_mw, vddio_power_mw}
    QVariantMap getPowerMeasurements(int instanceId);
    QVariantMap getPackageVersions(int instanceId);
    QString getPanelId(int instanceId);

    // Register access (per instance)
    int readRj1Register(int instanceId, int address);
    bool writeRj1Register(int instanceId, int address, int data);

    // DAC control (per instance)
    QVariantMap getRj1Dacs(int instanceId);
    bool setRj1Dacs(int instanceId, const QVariantMap &dacValues);

    // Demura and RLUT (per instance)
    bool setDemuraEnable(int instanceId, bool enable);
    bool setRlutEnable(int instanceId, bool enable);
    QVariantMap getDemuraRlutState(int instanceId);

    // Utility
    QString getLastError() const;
    QString getLastError(int instanceId) const;

    // Python output capture
    void flushPythonOutput();

signals:
    void errorOccurred(const QString &error);
    void logMessage(const QString &message);
    void sessionStateChanged(const QString &serial, const QString &state,
                             quint64 generation, const QString &error);

private:
    // Internal methods (only called on Python worker thread)
    bool importModule(const QString &moduleName);
    PyObject* callInstanceMethod(int instanceId, const char *methodName, PyObject *args = nullptr);
    QVariant pyObjectToVariant(PyObject *obj);
    PyObject* variantToPyObject(const QVariant &var);
    PyObject* qimageToPyArray(const QImage &image);
    void setError(const QString &error);
    void setInstanceError(int instanceId, const QString &error);
    void clearInstanceError(int instanceId);
    void clearError();
    PyObject* getDeviceInstance(int instanceId) const;

    bool isRetryableTransportFailure(const QString &errorCode,
                                     const QString &detail) const;
    void setSessionState(int instanceId, SessionState state,
                         const QString &error = QString());
    void quarantineSubprocess(int instanceId, const QString &operation,
                              const QString &detail,
                              int cleanupTimeoutMs = 1000);
    bool recoverDeviceInstance(int instanceId, int budgetMs,
                               QString *error = nullptr);
    void startRecoveryAsync(int instanceId);
    QJsonObject subprocessCallWithRecovery(int instanceId, const QString &cmd,
                                           const QJsonObject &args,
                                           int commandDeadlineMs,
                                           bool retryOnTransport = true);
    bool m_initialized;
    PyObject *m_module;
    PyObject *m_corneaClass;
    PyObject *m_numpyModule;
    PyObject *m_executor;  // Python concurrent.futures.ThreadPoolExecutor
    QString m_lastError;
    QMap<int, QString> m_instanceLastErrors;
    mutable QMutex m_lastErrorMutex;
    QString m_venvPath;
    QString m_pythonHome;
    QStringList m_dllPaths;
    QString m_calPath;
    QString m_defectMapOutputRoot;
    bool m_allowDefaultHdf5;
    double m_spiClkFreq = 15e6;
    bool m_logApl = false;
    bool m_serializeUsb = false;
    QAtomicInteger<int> m_powerDeadlineMs{DEFAULT_POWER_DEADLINE_MS};
    QAtomicInteger<int> m_recoveryBudgetMs{DEFAULT_RECOVERY_BUDGET_MS};

    // Multi-instance management
    QMap<int, PyObject*> m_deviceInstances;  // instanceId -> CorneaRax720
    QMap<int, bool> m_initOkMap;             // instanceId -> init_ok status
    QMap<int, QString> m_serialMap;          // instanceId -> serial number
    int m_nextInstanceId = 0;

    // Phase 2 subprocess mode. When m_useSubprocess is true, each
    // instanceId gets a PanelSubprocess in m_panelProcs INSTEAD of a
    // PyObject* in m_deviceInstances. Operations route through the
    // subprocess's JSON-RPC client. Mutex protects insertions /
    // removals; per-instance reads are racy-but-OK after creation
    // because instanceId allocation is single-threaded.
    bool m_useSubprocess = true;
    QString m_workerScript;
    QString m_subprocessPythonExe;
    QMap<int, PanelSubprocess*> m_panelProcs;
    QMap<QString, PanelSubprocess*> m_idlePanelProcs; // serial -> reset worker
    mutable QMutex m_panelProcsMutex;

    struct InstanceMetadata {
        int deviceIndex = -1;
        QString hardwareVariant;
        QString expectedSerial;
    };
    struct SessionCoordinator {
        SessionState state = SessionState::Unavailable;
        quint64 generation = 0;
        int instanceId = -1;
        bool recoveryStarted = false;
        QString lastError;
    };
    QMap<int, InstanceMetadata> m_instanceMetadata;
    QMap<QString, SessionCoordinator> m_sessions;
    mutable QMutex m_sessionMutex;

    // Helper that routes one JSON-RPC call to the panel's subprocess.
    // Caller passes cmd + args; receives the full reply object so they
    // can fish out the relevant "data" field with the correct type.
    // Returns an object with success=false and an "error" string on
    // any failure (no subprocess, send failed, worker reported error).
    QJsonObject subprocessCall(int instanceId, const QString &cmd,
                                const QJsonObject &args, int timeoutMs = 60000);
    void markSubprocessUnhealthy(int instanceId, const QString &operation,
                                 const QString &detail);

    // Main Python worker thread — used for interpreter init, module imports,
    // device enumeration, and other non-per-instance work. Per-instance ops
    // run on per-device worker threads instead (see below).
    QThread m_pythonThread;
    QObject *m_pythonWorker = nullptr;

    // Per-device worker threads. Each `createDeviceInstance` /
    // `preInitDeviceInstance` spins up a dedicated QThread + QObject worker
    // for that instanceId. Commands for that device run on its own thread so
    // multiple panels can have their Python/I-O overlap (pyftdi + SPI release
    // the GIL during transactions). Dying instances have their thread joined
    // and deleted.
    QMap<int, QThread*>  m_deviceThreads;
    QMap<int, QObject*>  m_deviceWorkers;
    mutable QMutex m_deviceThreadsMutex;

    // Main-interpreter thread state saved after init so worker threads can
    // acquire the GIL via PyGILState_Ensure. Restored (briefly) at shutdown.
    void *m_savedThreadState = nullptr;  // actually PyThreadState*

    // Dispatch helpers: run callable on a Python-capable thread, block until
    // done. Both acquire the GIL before running `f()` (via PyGILState_Ensure)
    // and release it after, so callers can use the Python C API directly
    // inside their lambdas.
    //   - dispatch / dispatchVoid → main Python worker thread
    //   - dispatchToDevice / dispatchVoidToDevice → the instanceId's worker
    template<typename F> auto dispatch(F &&f) -> decltype(f());
    void dispatchVoid(std::function<void()> f);
    template<typename F> auto dispatchToDevice(int instanceId, F &&f) -> decltype(f());
    void dispatchVoidToDevice(int instanceId, std::function<void()> f);

    // Create / destroy a device-specific worker thread. Called from
    // (pre)InitDeviceInstance and destroyDeviceInstance respectively.
    QObject *ensureDeviceWorker(int instanceId);
    void destroyDeviceWorker(int instanceId);

#ifdef QT_DEBUG
    // Debug simulation mode — no hardware, no Python, fake 12 devices.
    // Enabled automatically in debug builds.
    static constexpr bool m_simulateMode = true;
    static constexpr int SIM_DEVICE_COUNT = 12;
    static constexpr int SIM_DELAY_MS = 1000;

    struct SimDevice {
        QString serial;
        bool connected = false;
        double brightness = 0.0;
        bool xFlip = false;
        bool yFlip = false;
    };
    QMap<int, SimDevice> m_simDevices;
#else
    static constexpr bool m_simulateMode = false;
#endif
};

#endif // PYTHONBRIDGE_H
