#include "corneaconfig.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>

QJsonObject DeviceConfig::toJson() const
{
    QJsonObject obj;
    obj["serial"] = serial;
    obj["variant"] = variant;
    obj["brightness"] = brightness;
    return obj;
}

QJsonObject TcpConfig::toJson() const
{
    QJsonObject obj;
    obj["enabled"] = enabled;
    obj["port"] = static_cast<int>(port);
    return obj;
}

TcpConfig TcpConfig::fromJson(const QJsonObject &obj)
{
    TcpConfig config;
    config.enabled = obj["enabled"].toBool(false);
    config.port = static_cast<quint16>(obj["port"].toInt(5566));
    return config;
}

DeviceConfig DeviceConfig::fromJson(const QJsonObject &obj)
{
    DeviceConfig config;
    config.serial = obj["serial"].toString();
    config.variant = obj["variant"].toString("standard");
    config.brightness = obj["brightness"].toDouble(0.03);
    return config;
}

CorneaConfig::CorneaConfig()
{
    ensureDeviceSlots();
}

void CorneaConfig::ensureDeviceSlots()
{
    while (m_devices.size() < MAX_DEVICES) {
        m_devices.append(DeviceConfig());
    }
}

bool CorneaConfig::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open config file:" << path;
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "JSON parse error:" << error.errorString();
        return false;
    }

    QJsonObject root = doc.object();

    // Parse python config
    QJsonObject pythonObj = root["python"].toObject();
    m_python.venvPath = pythonObj["venv_path"].toString();
    m_python.pythonHome = pythonObj["python_home"].toString();
    m_python.calPath = pythonObj["cal_path"].toString();
    m_python.allowDefaultHdf5 = pythonObj["allow_default_hdf5"].toBool(false);
    m_python.spiClkFreq = pythonObj["spi_clk_freq"].toDouble(15e6);
    m_python.logApl = pythonObj["log_apl"].toBool(false);
    // Default true (subprocess mode is now the production path; embedded
    // is the explicit-opt-out fallback). If the key is missing from an
    // older config file, we still want subprocess on.
    m_python.useSubprocess = pythonObj["use_subprocess"].toBool(true);
    m_python.workerScriptPath = pythonObj["worker_script"].toString();

    m_python.dllPaths.clear();
    QJsonArray dllArray = pythonObj["dll_paths"].toArray();
    for (const auto &val : dllArray) {
        m_python.dllPaths.append(val.toString());
    }

    // Parse TCP config
    if (root.contains("tcp")) {
        m_tcp = TcpConfig::fromJson(root["tcp"].toObject());
    }

    // Parse devices array
    m_devices.clear();
    QJsonArray devicesArray = root["devices"].toArray();
    for (const auto &val : devicesArray) {
        if (val.isNull()) {
            m_devices.append(DeviceConfig());
        } else {
            m_devices.append(DeviceConfig::fromJson(val.toObject()));
        }
    }
    ensureDeviceSlots();

    m_configPath = path;
    return true;
}

bool CorneaConfig::save()
{
    if (m_configPath.isEmpty()) {
        qWarning() << "No config path set";
        return false;
    }
    return save(m_configPath);
}

bool CorneaConfig::save(const QString &path)
{
    QJsonObject root;

    // Python config
    QJsonObject pythonObj;
    pythonObj["venv_path"] = m_python.venvPath;
    pythonObj["python_home"] = m_python.pythonHome;
    pythonObj["cal_path"] = m_python.calPath;
    pythonObj["allow_default_hdf5"] = m_python.allowDefaultHdf5;
    pythonObj["spi_clk_freq"] = m_python.spiClkFreq;
    pythonObj["log_apl"] = m_python.logApl;
    pythonObj["use_subprocess"] = m_python.useSubprocess;
    pythonObj["worker_script"] = m_python.workerScriptPath;

    QJsonArray dllArray;
    for (const auto &dll : m_python.dllPaths) {
        dllArray.append(dll);
    }
    pythonObj["dll_paths"] = dllArray;
    root["python"] = pythonObj;

    // TCP config
    root["tcp"] = m_tcp.toJson();

    // Devices array
    QJsonArray devicesArray;
    for (const auto &dev : m_devices) {
        if (dev.isValid()) {
            devicesArray.append(dev.toJson());
        } else {
            devicesArray.append(QJsonValue::Null);
        }
    }
    root["devices"] = devicesArray;

    QJsonDocument doc(root);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to write config file:" << path;
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    m_configPath = path;
    return true;
}

void CorneaConfig::setPython(const PythonConfig &config)
{
    m_python = config;
}

void CorneaConfig::setTcp(const TcpConfig &config)
{
    m_tcp = config;
}

DeviceConfig CorneaConfig::getDevice(int index) const
{
    if (index < 0 || index >= m_devices.size()) {
        return DeviceConfig();
    }
    return m_devices[index];
}

void CorneaConfig::setDevice(int index, const DeviceConfig &config)
{
    if (index < 0 || index >= MAX_DEVICES) {
        return;
    }
    ensureDeviceSlots();
    m_devices[index] = config;
}

int CorneaConfig::deviceCount() const
{
    int count = 0;
    for (const auto &dev : m_devices) {
        if (dev.isValid()) {
            count++;
        }
    }
    return count;
}

int CorneaConfig::findDeviceBySerial(const QString &serial) const
{
    for (int i = 0; i < m_devices.size(); i++) {
        if (m_devices[i].serial == serial) {
            return i;
        }
    }
    return -1;
}

int CorneaConfig::addNewDevice(const QString &serial, const QString &variant, double brightness)
{
    // Check if already exists
    int existing = findDeviceBySerial(serial);
    if (existing >= 0) {
        return existing;
    }

    // Find first empty slot
    for (int i = 0; i < m_devices.size(); i++) {
        if (!m_devices[i].isValid()) {
            DeviceConfig config;
            config.serial = serial;
            config.variant = variant;
            config.brightness = brightness;
            m_devices[i] = config;
            return i;
        }
    }

    return -1;  // No empty slot
}

void CorneaConfig::updateDeviceSettings(const QString &serial, const QString &variant, double brightness)
{
    int index = findDeviceBySerial(serial);
    if (index >= 0) {
        m_devices[index].variant = variant;
        m_devices[index].brightness = brightness;
    } else {
        addNewDevice(serial, variant, brightness);
    }
}
