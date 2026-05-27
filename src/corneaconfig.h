#ifndef CORNEACONFIG_H
#define CORNEACONFIG_H

#include <QString>
#include <QList>
#include <QJsonObject>

struct PythonConfig {
    QString venvPath;
    QString pythonHome;
    QStringList dllPaths;
    QString calPath;
    bool allowDefaultHdf5 = false;
    double spiClkFreq = 15e6;       // SPI clock frequency in Hz (default 15MHz)
    // rax_lib's write_rj1_frame logs an APL line on every frame by default.
    // Default false here = quieter logs + higher throughput. Flip to true
    // when debugging APL / brightness behavior.
    bool logApl = false;
    // Phase 2 (v1.1.20): each panel routes through its own python.exe
    // subprocess running panel_worker.py via stdin/stdout JSON-RPC.
    // Sidesteps the cross-panel GIL contention documented in
    // bench_concurrent_cascade.py 2026-05-27: embedded mode produced
    // 14-second GIL holds and CC server lockups under 4+ panel
    // concurrent load, subprocess mode kept the trigger powerOn under
    // 2.2 s flat. Default ON since 2026-05-27 validation; set to false
    // here to fall back to embedded interpreter for debugging.
    bool useSubprocess = true;
    // Where panel_worker.py lives. Empty = look beside CorneaController.exe
    // at ./python/panel_worker.py.
    QString workerScriptPath;
};

struct DeviceConfig {
    QString serial;
    QString variant = "standard";
    double brightness = 0.03;

    bool isValid() const { return !serial.isEmpty(); }

    QJsonObject toJson() const;
    static DeviceConfig fromJson(const QJsonObject &obj);
};

struct TcpConfig {
    bool enabled = false;
    quint16 port = 5566;

    QJsonObject toJson() const;
    static TcpConfig fromJson(const QJsonObject &obj);
};

class CorneaConfig
{
public:
    static const int MAX_DEVICES = 12;

    CorneaConfig();

    // Load/Save
    bool load(const QString &path);
    bool save();
    bool save(const QString &path);
    QString configPath() const { return m_configPath; }

    // Python config
    const PythonConfig& python() const { return m_python; }
    void setPython(const PythonConfig &config);

    // TCP config
    const TcpConfig& tcp() const { return m_tcp; }
    void setTcp(const TcpConfig &config);

    // Device config (by index 0-11)
    DeviceConfig getDevice(int index) const;
    void setDevice(int index, const DeviceConfig &config);
    int deviceCount() const;

    // Find device by serial
    int findDeviceBySerial(const QString &serial) const;

    // Add new device to first empty slot, returns index or -1 if full
    int addNewDevice(const QString &serial, const QString &variant = "standard", double brightness = 0.03);

    // Update device settings (creates if not exists)
    void updateDeviceSettings(const QString &serial, const QString &variant, double brightness);

private:
    QString m_configPath;
    PythonConfig m_python;
    TcpConfig m_tcp;
    QList<DeviceConfig> m_devices;  // Max 12 items

    void ensureDeviceSlots();
};

#endif // CORNEACONFIG_H
