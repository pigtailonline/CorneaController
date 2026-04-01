#ifndef CORNEACONTROLLER_H
#define CORNEACONTROLLER_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QTimer>
#include <memory>

class PythonBridge;

class CorneaController : public QObject
{
    Q_OBJECT

public:
    // Constructor takes shared PythonBridge pointer (not owned)
    explicit CorneaController(PythonBridge *sharedBridge, QObject *parent = nullptr);
    ~CorneaController();

    // Connection
    bool connect(int deviceIndex, const QString &hardwareVariant = "standard");
    void disconnect();
    bool isConnected() const;
    bool isInitOk() const;

    // Device identification
    int getDeviceIndex() const { return m_deviceIndex; }
    QString getDeviceSerial() const { return m_deviceSerial; }
    QString getDeviceLabel() const;  // e.g., "Index 0: SERIAL"

    // Power control
    bool powerOn();
    bool powerOff();
    bool setVsysEnabled(bool enabled);
    bool isPoweredOn() const { return m_poweredOn; }
    QString lastError() const;

    // Brightness
    bool setBrightness(double level);
    double getBrightness();

    // Flip (CalMetadata.x_flip / y_flip)
    bool setXFlip(bool flip);
    bool setYFlip(bool flip);
    bool getXFlip();
    bool getYFlip();

    // Image
    bool sendImage(const QImage &image);
    bool sendImageFile(const QString &filePath);

    // Panel information
    QString getPanelId();
    QVariantMap getChipInfoDecoded();

    // Temperature
    double getRj1Temperature();     // RJ1-RAX internal temp sensor
    double getDa9272Temperature();
    void startTemperaturePolling(int intervalMs = 5000);
    void stopTemperaturePolling();

    // Register access
    int readRegister(int address);
    bool writeRegister(int address, int value);

    // DAC
    QVariantMap getDacValues();
    bool setDacValues(const QVariantMap &values);

    // Demura/RLUT
    bool setDemuraEnabled(bool enabled);
    bool setRlutEnabled(bool enabled);
    QVariantMap getDemuraRlutState();

    // Package info
    QVariantMap getPackageVersions();

    // Error handling
    QString getLastError() const;

signals:
    void connected();
    void disconnected();
    void powerStateChanged(bool poweredOn);
    void brightnessChanged(double level);
    void xFlipChanged(bool flip);
    void yFlipChanged(bool flip);
    void imageSent(bool success);
    void temperatureUpdated(double rj1, double da9272);
    void errorOccurred(const QString &error);
    void logMessage(const QString &message);

private slots:
    void onTemperaturePollTimeout();

    // Check if device is ready for commands (connected + powered on)
    bool requirePoweredOn(const QString &operation);

private:
    PythonBridge *m_bridge;  // Shared, not owned
    QTimer *m_tempPollTimer;
    bool m_poweredOn;
    double m_currentBrightness;
    bool m_currentXFlip;
    bool m_currentYFlip;

    // Instance management
    int m_instanceId = -1;      // Instance ID in PythonBridge (-1 = not connected)
    int m_deviceIndex = -1;     // Device index
    QString m_deviceSerial;     // Device serial number
};

#endif // CORNEACONTROLLER_H
