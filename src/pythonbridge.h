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
#include <QRandomGenerator>
#include <memory>

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
    explicit PythonBridge(QObject *parent = nullptr);
    ~PythonBridge();

    // Initialization (Python interpreter - call once)
    bool initialize(const QString &venvPath);
    bool initialize(const QString &venvPath, const QString &pythonHome,
                    const QStringList &dllPaths, const QString &calPath,
                    bool allowDefaultHdf5 = false);
    void setSpiClkFreq(double freq) { m_spiClkFreq = freq; }
    double spiClkFreq() const { return m_spiClkFreq; }
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Device enumeration
    QList<DeviceInfo> getAvailableDevicesInfo();

    // Multi-instance device management
    int createDeviceInstance(int deviceIndex, const QString &hardwareVariant = "standard");  // Returns instanceId, -1 on failure
    void destroyDeviceInstance(int instanceId);
    bool isDeviceConnected(int instanceId) const;
    bool isDeviceInitOk(int instanceId) const;
    QString getDeviceSerial(int instanceId) const;

    // Power control (per instance)
    bool systemPowerOn(int instanceId);
    bool systemPowerOff(int instanceId);
    bool enableVsys(int instanceId, bool enable);

    // Brightness control (per instance)
    bool setBrightness(int instanceId, double level);
    double getBrightness(int instanceId);

    // Flip control (per instance) - CalMetadata.x_flip / y_flip
    bool setXFlip(int instanceId, bool flip);
    bool setYFlip(int instanceId, bool flip);
    bool getXFlip(int instanceId);
    bool getYFlip(int instanceId);

    // Image operations (per instance)
    bool writeFrame(int instanceId, const QImage &image);
    bool writeFrameFromPath(int instanceId, const QString &imagePath);

    // Information retrieval (per instance)
    QVariantMap getChipInfo(int instanceId);
    QVariantMap getChipInfoDecoded(int instanceId);
    QString getChipRevision(int instanceId);
    double getLeaTemperature(int instanceId);   // RJ1-RAX internal temp sensor
    double getDa9272Temperature(int instanceId);
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
    QString getLastError() const { return m_lastError; }

    // Python output capture
    void flushPythonOutput();

signals:
    void errorOccurred(const QString &error);
    void logMessage(const QString &message);

private:
    // Internal methods (only called on Python worker thread)
    bool importModule(const QString &moduleName);
    PyObject* callInstanceMethod(int instanceId, const char *methodName, PyObject *args = nullptr);
    QVariant pyObjectToVariant(PyObject *obj);
    PyObject* variantToPyObject(const QVariant &var);
    PyObject* qimageToPyArray(const QImage &image);
    void setError(const QString &error);
    void clearError();
    PyObject* getDeviceInstance(int instanceId) const;

    bool m_initialized;
    PyObject *m_module;
    PyObject *m_corneaClass;
    PyObject *m_numpyModule;
    PyObject *m_executor;  // Python concurrent.futures.ThreadPoolExecutor
    QString m_lastError;
    QString m_venvPath;
    QString m_pythonHome;
    QStringList m_dllPaths;
    QString m_calPath;
    bool m_allowDefaultHdf5;
    double m_spiClkFreq = 15e6;

    // Multi-instance management
    QMap<int, PyObject*> m_deviceInstances;  // instanceId -> CorneaRax720
    QMap<int, bool> m_initOkMap;             // instanceId -> init_ok status
    QMap<int, QString> m_serialMap;          // instanceId -> serial number
    int m_nextInstanceId = 0;

    // Dedicated Python worker thread
    // All Python C API calls happen exclusively on this thread.
    // No GIL management needed — single thread = serialized access.
    QThread m_pythonThread;
    QObject *m_pythonWorker = nullptr;

    // Dispatch helpers: run callable on Python thread, block until done.
    // If already on Python thread, runs directly (avoids deadlock).
    template<typename F> auto dispatch(F &&f) -> decltype(f());
    void dispatchVoid(std::function<void()> f);

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
