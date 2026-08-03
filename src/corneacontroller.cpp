#include "corneacontroller.h"
#include "pythonbridge.h"

#include <QDebug>
#include <cmath>

CorneaController::CorneaController(PythonBridge *sharedBridge, QObject *parent)
    : QObject(parent)
    , m_bridge(sharedBridge)
    , m_tempPollTimer(new QTimer(this))
    , m_poweredOn(false)
    , m_currentBrightness(0.03)
    , m_currentXFlip(false)
    , m_currentYFlip(false)
    , m_instanceId(-1)
    , m_deviceIndex(-1)
{
    m_cacheClock.start();
    QObject::connect(m_tempPollTimer, &QTimer::timeout,
                     this, &CorneaController::onTemperaturePollTimeout);
}

CorneaController::~CorneaController()
{
    stopTemperaturePolling();
    disconnect();
}

QString CorneaController::lastError() const
{
    QMutexLocker lock(&m_lastErrorMutex);
    return m_lastError;
}

void CorneaController::setLastError(const QString &error)
{
    QMutexLocker lock(&m_lastErrorMutex);
    m_lastError = error;
}

bool CorneaController::failWithError(const QString &error)
{
    const QString detail = error.trimmed().isEmpty()
        ? QStringLiteral("Controller operation failed")
        : error;
    setLastError(detail);
    emit errorOccurred(detail);
    return false;
}

bool CorneaController::connect(int deviceIndex, const QString &hardwareVariant,
                               const QString &expectedSerial)
{
    if (m_instanceId >= 0) {
        disconnect();
    }

    QString commandError;
    int instanceId = m_bridge->createDeviceInstance(
        deviceIndex, hardwareVariant, expectedSerial, &commandError);
    if (instanceId >= 0) {
        m_instanceId = instanceId;
        m_deviceIndex = deviceIndex;
        m_deviceSerial = m_bridge->getDeviceSerial(instanceId);
        if (m_deviceSerial.isEmpty()) {
            m_deviceSerial = expectedSerial;
        }
        m_currentBrightness = m_bridge->getBrightness(instanceId);
        m_poweredOn = true;
        setLastError(QString());

        emit connected();
        // Do NOT emit powerStateChanged(true) here. onControllerConnected
        // already pushes a setBrightness() on the 'connected' signal; emitting
        // powerStateChanged(true) too queues a SECOND back-to-back
        // setBrightness on the per-device worker, which races with rax_lib's
        // post-connect settling and leaves the Bx panel stuck displaying
        // black. powerStateChanged is reserved for pure power transitions
        // (powerOn / powerOff on an already-existing instance).
        emit logMessage(QString("Connected to device: %1").arg(getDeviceLabel()));

        // Pre-warm temperature cache: one synchronous HW read so the first
        // TCP getTemperature after powerOn returns a real value instead of
        // the -999 sentinel that would otherwise persist until the 5 s
        // background timer fires. Failure is non-fatal — cache stays -999,
        // same as previously. Cost: ~50–200 ms on the powerOn path.
        getRj1Temperature();
        return true;
    }

    return failWithError(commandError.isEmpty()
        ? QString("Failed to connect device %1").arg(deviceIndex)
        : commandError);
}

bool CorneaController::preInit(int deviceIndex, const QString &hardwareVariant,
                               const QString &expectedSerial)
{
    if (m_instanceId >= 0) {
        return true; // Already initialized
    }

    int instanceId = m_bridge->preInitDeviceInstance(deviceIndex, hardwareVariant, expectedSerial);
    if (instanceId >= 0) {
        m_instanceId = instanceId;
        m_deviceIndex = deviceIndex;
        m_deviceSerial = m_bridge->getDeviceSerial(instanceId);
        if (m_deviceSerial.isEmpty()) {
            m_deviceSerial = expectedSerial;
        }
        m_poweredOn = false; // FTDI ready but not powered on
        setLastError(QString());

        emit logMessage(QString("Pre-init OK: %1 (FTDI ready)").arg(getDeviceLabel()));
        return true;
    }

    return false;
}

void CorneaController::disconnect()
{
    if (m_instanceId >= 0) {
        stopTemperaturePolling();
        m_bridge->destroyDeviceInstance(m_instanceId);

        QString label = getDeviceLabel();
        m_instanceId = -1;
        m_deviceIndex = -1;
        m_deviceSerial.clear();
        m_poweredOn = false;
        setLastError(QString());

        emit disconnected();
        emit logMessage(QString("Disconnected from device: %1").arg(label));
    }
}

bool CorneaController::isConnected() const
{
    bool hasInstanceId = m_instanceId >= 0;
    bool bridgeConnected = hasInstanceId && m_bridge->isDeviceConnected(m_instanceId);
    if (!bridgeConnected && hasInstanceId) {
        qDebug() << "CorneaController::isConnected: instanceId=" << m_instanceId
                 << "hasInstanceId=" << hasInstanceId
                 << "bridgeConnected=" << bridgeConnected;
    }
    return bridgeConnected;
}

bool CorneaController::isInitOk() const
{
    return m_instanceId >= 0 && m_bridge->isDeviceInitOk(m_instanceId);
}

QString CorneaController::getDeviceLabel() const
{
    if (m_deviceIndex < 0) {
        return QString("Not connected");
    }
    return QString("Index %1: %2").arg(m_deviceIndex).arg(m_deviceSerial);
}

bool CorneaController::powerOn()
{
    if (!isConnected()) {
        return failWithError(QStringLiteral("Power on failed: device not connected"));
    }

    QString commandError;
    if (m_bridge->systemPowerOn(m_instanceId, &commandError)) {
        m_poweredOn = true;
        setLastError(QString());
        emit powerStateChanged(true);
        emit logMessage("Power ON");
        // Pre-warm temperature cache on the reuse path too — see connect().
        getRj1Temperature();
        return true;
    }

    return failWithError(commandError.isEmpty()
        ? QStringLiteral("Power on failed")
        : commandError);
}

bool CorneaController::powerOff()
{
    if (!isConnected()) {
        return failWithError(QStringLiteral("Power off failed: device not connected"));
    }

    if (m_bridge->systemPowerOff(m_instanceId)) {
        m_poweredOn = false;
        setLastError(QString());
        emit powerStateChanged(false);
        emit logMessage("Power OFF");
        return true;
    }

    const QString bridgeError = m_bridge->getLastError(m_instanceId);
    return failWithError(bridgeError.isEmpty()
        ? QStringLiteral("Power off failed")
        : bridgeError);
}

bool CorneaController::setVsysEnabled(bool enabled)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    return m_bridge->enableVsys(m_instanceId, enabled);
}

bool CorneaController::requirePoweredOn(const QString &operation)
{
    if (!isConnected()) {
        QString err = QString("%1 failed: device not connected").arg(operation);
        qWarning() << err;
        return failWithError(err);
    }
    if (!isPoweredOn()) {
        QString err = QString("%1 failed: device is powered off (possibly overheated)").arg(operation);
        qWarning() << err;
        return failWithError(err);
    }
    return true;
}

bool CorneaController::setBrightness(double level)
{
    if (!requirePoweredOn("setBrightness")) return false;

    // Clamp to valid range
    level = qBound(0.0, level, 1.0);

    QString commandError;
    if (m_bridge->setBrightness(m_instanceId, level, &commandError)) {
        setLastError(QString());
        m_currentBrightness = level;
        emit brightnessChanged(level);
        return true;
    }

    setLastError(commandError);
    emit errorOccurred(commandError);
    return false;
}

double CorneaController::getBrightness()
{
    if (!isConnected()) {
        return m_currentBrightness;
    }

    double brightness = m_bridge->getBrightness(m_instanceId);
    if (brightness >= 0) {
        m_currentBrightness = brightness;
    }
    return m_currentBrightness;
}

bool CorneaController::setXFlip(bool flip)
{
    if (!requirePoweredOn("setXFlip")) return false;

    if (m_bridge->setXFlip(m_instanceId, flip)) {
        m_currentXFlip = flip;
        emit xFlipChanged(flip);
        return true;
    }
    return false;
}

bool CorneaController::setYFlip(bool flip)
{
    if (!requirePoweredOn("setYFlip")) return false;

    if (m_bridge->setYFlip(m_instanceId, flip)) {
        m_currentYFlip = flip;
        emit yFlipChanged(flip);
        return true;
    }
    return false;
}

bool CorneaController::getXFlip()
{
    if (!isConnected()) {
        return m_currentXFlip;
    }

    m_currentXFlip = m_bridge->getXFlip(m_instanceId);
    return m_currentXFlip;
}

bool CorneaController::getYFlip()
{
    if (!isConnected()) {
        return m_currentYFlip;
    }

    m_currentYFlip = m_bridge->getYFlip(m_instanceId);
    return m_currentYFlip;
}

bool CorneaController::sendImage(const QImage &image)
{
    if (!requirePoweredOn("sendImage")) return false;

    QString commandError;
    bool success = m_bridge->writeFrame(m_instanceId, image, &commandError);
    if (!success) {
        setLastError(commandError);
        emit errorOccurred(commandError);
    } else {
        setLastError(QString());
    }
    emit imageSent(success);
    return success;
}

bool CorneaController::sendImageFile(const QString &filePath)
{
    if (!requirePoweredOn("sendImageFile")) return false;

    bool success = m_bridge->writeFrameFromPath(m_instanceId, filePath);
    emit imageSent(success);
    return success;
}

QString CorneaController::getPanelId()
{
    if (!requirePoweredOn("getPanelId")) return QString();

    return m_bridge->getPanelId(m_instanceId);
}

QVariantMap CorneaController::getChipInfoDecoded()
{
    if (!isConnected()) {
        return QVariantMap();
    }

    return m_bridge->getChipInfoDecoded(m_instanceId);
}

double CorneaController::getRj1Temperature()
{
    if (!isConnected()) {
        return -999.0;
    }

    // Dedupe reads within RJ1_CACHE_TTL_MS. Rapid TCP traffic (brightness
    // protection checks, UI refresh, sendImage flow) hits the same value
    // instead of queueing multiple I2C round-trips. Each controller has its
    // own cache — Panel A's cache does not affect Panel B.
    const qint64 now = m_cacheClock.elapsed();
    {
        QMutexLocker lock(&m_tempCacheMutex);
        if (m_hasRj1TemperatureObservation
            && (now - m_lastRj1ReadMs) < RJ1_CACHE_TTL_MS) {
            return m_cachedRj1Temp;
        }
    }

    double temp = m_bridge->getLeaTemperature(m_instanceId);
    QMutexLocker lock(&m_tempCacheMutex);
    m_cachedRj1Temp = temp;
    m_hasRj1TemperatureObservation = true;
    m_lastRj1ReadMs = now;
    if (std::isfinite(temp) && temp > -30.0) {
        m_lastValidRj1Temp = temp;
    }
    return temp;
}

double CorneaController::getCachedRj1Temperature() const
{
    // Pure cache read — never touches HW. Background timer
    // (DeviceControlPanel::onTemperatureMonitorTimeout) is the sole writer
    // via getRj1Temperature() above. TCP `getTemperature` calls land here so
    // they never queue I2C transactions behind sendImage/setBrightness.
    QMutexLocker lock(&m_tempCacheMutex);
    return m_cachedRj1Temp;
}

double CorneaController::getDa9272Temperature()
{
    // DA9272 temperature probe is not used by the station — it often NACKs on
    // our hardware, wasting ~1 s per poll in retry loops and blocking the
    // single Python dispatch thread (which starves sendImage/brightness/etc.).
    // Skip the actual I2C read; UI shows "-" when value <= -900 and overheat
    // protection uses qMax(rj1, da9272) so RJ1 alone still governs shutdown.
    return -999.0;
}

QVariantMap CorneaController::getPowerMeasurements()
{
    if (!isConnected()) {
        QVariantMap v;
        v["vsys_power_mw"] = -999.0;
        v["vddio_power_mw"] = -999.0;
        return v;
    }
    return m_bridge->getPowerMeasurements(m_instanceId);
}

void CorneaController::startTemperaturePolling(int intervalMs)
{
    m_tempPollTimer->start(intervalMs);
}

void CorneaController::stopTemperaturePolling()
{
    m_tempPollTimer->stop();
}

void CorneaController::onTemperaturePollTimeout()
{
    if (!isConnected()) {
        return;
    }

    double rj1 = getRj1Temperature();
    double da9272 = getDa9272Temperature();

    emit temperatureUpdated(rj1, da9272);
}

int CorneaController::readRegister(int address)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return -1;
    }

    return m_bridge->readRj1Register(m_instanceId, address);
}

bool CorneaController::writeRegister(int address, int value)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    return m_bridge->writeRj1Register(m_instanceId, address, value);
}

QVariantMap CorneaController::getDacValues()
{
    if (!isConnected()) {
        return QVariantMap();
    }

    return m_bridge->getRj1Dacs(m_instanceId);
}

bool CorneaController::setDacValues(const QVariantMap &values)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    return m_bridge->setRj1Dacs(m_instanceId, values);
}

bool CorneaController::setDemuraEnabled(bool enabled)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    return m_bridge->setDemuraEnable(m_instanceId, enabled);
}

bool CorneaController::setRlutEnabled(bool enabled)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    return m_bridge->setRlutEnable(m_instanceId, enabled);
}

QVariantMap CorneaController::getDemuraRlutState()
{
    if (!isConnected()) {
        return QVariantMap();
    }

    return m_bridge->getDemuraRlutState(m_instanceId);
}

QVariantMap CorneaController::getPackageVersions()
{
    if (!isConnected()) {
        return QVariantMap();
    }

    return m_bridge->getPackageVersions(m_instanceId);
}

QString CorneaController::getLastError() const
{
    return m_bridge->getLastError();
}
