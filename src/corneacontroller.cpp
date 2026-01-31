#include "corneacontroller.h"
#include "pythonbridge.h"

#include <QDebug>

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
    QObject::connect(m_tempPollTimer, &QTimer::timeout,
                     this, &CorneaController::onTemperaturePollTimeout);
}

CorneaController::~CorneaController()
{
    stopTemperaturePolling();
    disconnect();
}

bool CorneaController::connect(int deviceIndex, const QString &hardwareVariant)
{
    if (m_instanceId >= 0) {
        disconnect();
    }

    int instanceId = m_bridge->createDeviceInstance(deviceIndex, hardwareVariant);
    if (instanceId >= 0) {
        m_instanceId = instanceId;
        m_deviceIndex = deviceIndex;
        m_deviceSerial = m_bridge->getDeviceSerial(instanceId);
        m_currentBrightness = m_bridge->getBrightness(instanceId);

        emit connected();
        emit logMessage(QString("Connected to device: %1").arg(getDeviceLabel()));
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
        emit errorOccurred("Not connected to device");
        return false;
    }

    if (m_bridge->systemPowerOn(m_instanceId)) {
        m_poweredOn = true;
        emit powerStateChanged(true);
        emit logMessage("Power ON");
        return true;
    }

    return false;
}

bool CorneaController::powerOff()
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    if (m_bridge->systemPowerOff(m_instanceId)) {
        m_poweredOn = false;
        emit powerStateChanged(false);
        emit logMessage("Power OFF");
        return true;
    }

    return false;
}

bool CorneaController::setVsysEnabled(bool enabled)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    return m_bridge->enableVsys(m_instanceId, enabled);
}

bool CorneaController::setBrightness(double level)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    // Clamp to valid range
    level = qBound(0.0, level, 1.0);

    if (m_bridge->setBrightness(m_instanceId, level)) {
        m_currentBrightness = level;
        emit brightnessChanged(level);
        return true;
    }

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
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    if (m_bridge->setXFlip(m_instanceId, flip)) {
        m_currentXFlip = flip;
        emit xFlipChanged(flip);
        return true;
    }
    return false;
}

bool CorneaController::setYFlip(bool flip)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

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
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    bool success = m_bridge->writeFrame(m_instanceId, image);
    emit imageSent(success);
    return success;
}

bool CorneaController::sendImageFile(const QString &filePath)
{
    if (!isConnected()) {
        emit errorOccurred("Not connected to device");
        return false;
    }

    bool success = m_bridge->writeFrameFromPath(m_instanceId, filePath);
    emit imageSent(success);
    return success;
}

QString CorneaController::getPanelId()
{
    if (!isConnected()) {
        return QString();
    }

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

    return m_bridge->getLeaTemperature(m_instanceId);
}

double CorneaController::getDa9272Temperature()
{
    if (!isConnected()) {
        return -999.0;
    }

    return m_bridge->getDa9272Temperature(m_instanceId);
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
