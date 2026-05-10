#ifndef DEVICECONTROLPANEL_H
#define DEVICECONTROLPANEL_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QGroupBox>
#include <QCheckBox>
#include <QTimer>
#include <memory>
#include <atomic>

#include "pythonbridge.h"

// Forward declaration for ApiResult
struct ApiResult;

class CorneaController;
class ImageLoader;

class DeviceControlPanel : public QWidget
{
    Q_OBJECT

public:
    // Customer-spec hard limit. Any single sample ≥ this triggers immediate
    // shutdown. The earlier tiered protection (require 2 consecutive samples
    // before shutting down to filter SPI glitches) lost ~10 °C of safety
    // margin in the 2026-04-29 customer event: panel heated 24 °C/s after
    // GL255_Red, the "awaiting confirmation" sample landed at 74.8 °C
    // (vs 65.3 °C the previous tick) before triggering. The glitch-filtering
    // benefit is dwarfed by the per-second heating rate, so single-sample
    // matches the spec strictly.
    static constexpr double TEMPERATURE_LIMIT = 65.0;

    explicit DeviceControlPanel(int panelId, PythonBridge *bridge, QWidget *parent = nullptr);
    ~DeviceControlPanel();

    // Panel identification
    int panelId() const { return m_panelId; }
    QString panelLabel() const;  // e.g., "Panel A", "Panel B"

    // Device state
    bool isConnected() const;
    bool isPoweredOn() const;
    void setDeviceInfo(const DeviceInfo &info);
    void setImageLoader(ImageLoader *loader);

    // Device info accessors
    int deviceIndex() const { return m_deviceInfo.index; }
    QString deviceSerial() const { return m_deviceInfo.serial; }
    QString getPanelId() const;
    double getRj1Temperature() const;

    // Thread-safe controller access (for TCP API, no UI)
    CorneaController* controller() const { return m_controller.get(); }
    double currentBrightness() const { return m_currentBrightness; }
    QString currentVariant() const { return m_currentVariant; }
    void setCurrentVariant(const QString &v) { m_currentVariant = v; }

    // Config setters (call before power on)
    void setVariant(const QString &variant);
    QString getVariant() const;
    void setInitialBrightness(double brightness);

    // Send image from shared loader
    void sendCurrentImage();

    // Direct API methods (for programmatic control)
    bool powerOnDirect();
    bool powerOffDirect();
    bool sendImageDirect(const QImage &image);
    bool setBrightnessDirect(double level);
    bool setXFlipDirect(bool flip);
    bool setYFlipDirect(bool flip);
    bool setFlipDirect(bool xFlip, bool yFlip);

    // Direct API with detailed error (for TCP)
    ApiResult sendImageDirectEx(const QImage &image);
    ApiResult setBrightnessDirectEx(double level, bool waitProtection = false);

signals:
    void logMessage(const QString &panelLabel, const QString &message);
    void connectionChanged(int panelId, bool connected);
    void variantChanged(const QString &serial, const QString &variant);

private slots:
    void onVariantChanged(const QString &variant);
    void onPowerOnClicked();
    void onPowerOffClicked();
    void onBrightnessSliderChanged(int value);
    void onBrightnessSliderReleased();
    void onBrightnessSpinBoxChanged(double value);
    void onXFlipChanged(bool checked);
    void onYFlipChanged(bool checked);
    void onRefreshInfoClicked();
    void onSendImageClicked();

    // Controller signal handlers
    void onControllerConnected();
    void onControllerDisconnected();
    void onControllerBrightnessChanged(double level);
    void onControllerXFlipChanged(bool flip);
    void onControllerYFlipChanged(bool flip);
    void onControllerTemperatureUpdated(double rj1, double da9272);
    void onControllerError(const QString &error);
    void onControllerLog(const QString &message);

    // Temperature monitoring
    void onTemperatureMonitorTimeout();
    void onBrightnessProtectionTimeout();

private:
    void setupUI();
    void setupConnections();
    void updateUIState();
    void updatePanelInfo();

    int m_panelId;  // 0, 1, 2, ...
    PythonBridge *m_bridge;  // Shared, not owned
    std::unique_ptr<CorneaController> m_controller;
    ImageLoader *m_imageLoader = nullptr;  // Shared, not owned
    DeviceInfo m_deviceInfo{-1, QString(), QString()};  // Assigned device info

    // UI elements (created programmatically for reusability)
    QGroupBox *m_grpMain;
    QLabel *m_lblDeviceName;
    QComboBox *m_cmbVariant;
    QPushButton *m_btnPowerOn;
    QPushButton *m_btnPowerOff;
    QLabel *m_lblStatus;

    // Brightness
    QSlider *m_sliderBrightness;
    QDoubleSpinBox *m_spinBrightness;

    // Flip controls
    QCheckBox *m_chkXFlip;
    QCheckBox *m_chkYFlip;

    // Info
    QLabel *m_lblPanelId;
    QLabel *m_lblRj1Temp;
    QLabel *m_lblDa9272Temp;
    QLabel *m_lblActualBrightness;
    QPushButton *m_btnRefreshInfo;

    // Image
    QPushButton *m_btnSendImage;

    // Temperature monitoring
    QTimer *m_temperatureTimer;
    static constexpr int TEMPERATURE_CHECK_INTERVAL_MS = 5000;

    // Brightness protection (check temp every 1s for 5s after brightness change)
    QTimer *m_brightnessProtectionTimer;
    int m_brightnessProtectionCount = 0;
    static constexpr int BRIGHTNESS_PROTECTION_CHECKS = 5;
    static constexpr int BRIGHTNESS_PROTECTION_INTERVAL_MS = 1000;

    void applyBrightness(double brightness);
    void startBrightnessProtection();

    // Cached state for thread-safe API access (no UI widget reads needed)
    double m_currentBrightness = 0.03;
    QString m_currentVariant = "standard";

    // Async operation management
    bool m_asyncTempBusy = false;
    std::shared_ptr<std::atomic<bool>> m_asyncGuard;

    // True while a powerOn / powerOff click is mid-flight (the long full-init
    // path can take 5+ s; during that window we mustn't let the disconnect
    // signal's updateUIState() re-enable the PowerOn button — operators saw
    // the button "come back" and clicked again, racing pyftdi resources
    // between two CorneaRax720 instances. updateUIState() respects this flag.
    bool m_powerOpInProgress = false;

public:
    // APL calculation with gamma LUT (fast, same result as std::pow)
    static double calculatePatternApl(const QImage &image);
};

#endif // DEVICECONTROLPANEL_H
