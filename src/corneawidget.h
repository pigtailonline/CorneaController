#ifndef CORNEAWIDGET_H
#define CORNEAWIDGET_H

#include <QWidget>
#include <QTimer>
#include <QList>
#include <memory>

#include "corneaconfig.h"

// API result with error details
struct ApiResult {
    bool success = false;
    QString error;

    static ApiResult ok() { return {true, QString()}; }
    static ApiResult fail(const QString &err) { return {false, err}; }
};

QT_BEGIN_NAMESPACE
namespace Ui { class CorneaWidget; }
QT_END_NAMESPACE

class PythonBridge;
class CorneaController;
class DeviceControlPanel;
class ImageLoader;
class TcpServer;

class CorneaWidget : public QWidget
{
    Q_OBJECT

public:
    static const int MAX_DEVICES = 12;

    explicit CorneaWidget(QWidget *parent = nullptr);
    ~CorneaWidget();

    // === Configuration ===
    bool loadConfig(const QString &configPath);
    bool saveConfig();
    CorneaConfig* config() { return &m_config; }

    // === Public API (by index 0-11) ===
    bool powerOn(int index);
    bool powerOff(int index);
    bool sendImage(int index, const QString &imagePath);
    bool sendImage(int index, const QImage &image);
    bool setBrightness(int index, double level);

    // === Status Query ===
    bool isConnected(int index) const;
    bool isConfigured(int index) const;
    QString getSerial(int index) const;
    int getConnectedCount() const;

    // Get the first/only connected device's serial (for single-device mode)
    QString getConnectedSerial() const;

    // Get Panel ID and temperature (follows single-device pattern)
    QString getPanelId(int index) const;
    double getRj1Temperature(int index) const;

    // Application-level cleanup hook. Called from main.cpp's aboutToQuit
    // connect so embedded Python is finalized and USB interfaces released
    // BEFORE the QApplication event loop returns. Safe to call multiple
    // times — second call is a no-op.
    void shutdown();

    // === Public API (by serial) ===
    bool powerOnBySerial(const QString &serial);
    bool powerOnBySerial(const QString &serial, const QString &variant);
    bool powerOffBySerial(const QString &serial);
    bool sendImageBySerial(const QString &serial, const QString &imagePath);
    bool sendImageBySerial(const QString &serial, const QImage &image);
    bool setBrightnessBySerial(const QString &serial, double level);
    bool setFlipBySerial(const QString &serial, bool xFlip, bool yFlip);

    // === Public API with detailed error (for TCP) ===
    ApiResult sendImageBySerialEx(const QString &serial, const QString &imagePath);
    ApiResult sendImageBySerialEx(const QString &serial, const QImage &image);
    ApiResult setBrightnessBySerialEx(const QString &serial, double level, bool waitProtection = false);

    // === Status Query (by serial) ===
    bool isConnectedBySerial(const QString &serial) const;
    QString getPanelIdBySerial(const QString &serial) const;
    double getTemperatureBySerial(const QString &serial) const;
    QString getVariantBySerial(const QString &serial) const;
    QVariantMap getPowerBySerial(const QString &serial) const;  // {vsys_power_mw, vddio_power_mw}

    // === Image Access (for TCP API) ===
    QStringList getImageNames() const;
    QImage getImageByName(const QString &name) const;

    // === Device List ===
    QStringList getDeviceSerials() const;
    void refreshDevices();


    // === TCP Server ===
    bool startTcpServer(quint16 port = 5566);
    void stopTcpServer();
    bool isTcpServerRunning() const;

signals:
    void deviceConnected(int index, const QString &serial);
    void deviceDisconnected(int index, const QString &serial);
    void logMessage(const QString &message);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    // Refresh devices
    void onRefreshDevicesClicked();

    // Image list selection
    void onImageListSelectionChanged();

    // Image loader signals
    void onImageLoaded(const QString &name);
    void onImageSelected(const QString &name);

    // Panel signals
    void onPanelLogMessage(const QString &panelLabel, const QString &message);
    void onPanelConnectionChanged(int panelId, bool connected);
    void onPanelVariantChanged(const QString &serial, const QString &variant);

    // Python output polling
    void onPythonOutputPollTimeout();

    // Bridge signals
    void onBridgeLogMessage(const QString &message);
    void onBridgeError(const QString &error);

private:
    void setupConnections();
    void updateImagePreview();
    void appendLog(const QString &message);
    void appendLog(const QString &panelLabel, const QString &message);
    void refreshDeviceList();
    void loadTestChartImages();
    bool initializePythonBridge();

    // Find panel by serial or index
    DeviceControlPanel* getPanelByIndex(int index) const;
    DeviceControlPanel* getPanelBySerial(const QString &serial) const;
    int findPanelIndexBySerial(const QString &serial) const;

    // Thread-safe: get controller without touching UI (for TCP async commands)
    CorneaController* getControllerBySerial(const QString &serial) const;

    Ui::CorneaWidget *ui;

    // Configuration
    CorneaConfig m_config;
    bool m_initialized = false;

    // Shared components
    std::unique_ptr<PythonBridge> m_pythonBridge;
    std::unique_ptr<ImageLoader> m_imageLoader;
    QTimer *m_pythonOutputTimer;

    // Device panels (dynamically created)
    QList<DeviceControlPanel*> m_devicePanels;

    // Map: config index -> panel (for API access)
    QMap<int, DeviceControlPanel*> m_indexToPanelMap;

    // TCP Server
    std::unique_ptr<TcpServer> m_tcpServer;
};

#endif // CORNEAWIDGET_H
