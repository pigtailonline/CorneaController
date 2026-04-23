#include "devicecontrolpanel.h"
#include "corneacontroller.h"
#include "imageloader.h"
#include "corneawidget.h"  // For ApiResult

#include <cmath>
#include <QThread>
#include <QtConcurrent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QApplication>

DeviceControlPanel::DeviceControlPanel(int panelId, PythonBridge *bridge, QWidget *parent)
    : QWidget(parent)
    , m_panelId(panelId)
    , m_bridge(bridge)
    , m_controller(std::make_unique<CorneaController>(bridge, this))
    , m_temperatureTimer(new QTimer(this))
    , m_brightnessProtectionTimer(new QTimer(this))
{
    setupUI();
    setupConnections();
    updateUIState();

    // Setup temperature monitoring timer
    connect(m_temperatureTimer, &QTimer::timeout, this, &DeviceControlPanel::onTemperatureMonitorTimeout);

    // Setup brightness protection timer
    connect(m_brightnessProtectionTimer, &QTimer::timeout, this, &DeviceControlPanel::onBrightnessProtectionTimeout);

    m_asyncGuard = std::make_shared<std::atomic<bool>>(true);
}

DeviceControlPanel::~DeviceControlPanel()
{
    m_asyncGuard->store(false);

    // Stop timers
    if (m_temperatureTimer) {
        m_temperatureTimer->stop();
    }
    if (m_brightnessProtectionTimer) {
        m_brightnessProtectionTimer->stop();
    }

    // Disconnect from device if connected
    if (m_controller && m_controller->isConnected()) {
        m_controller->disconnect();
    }
}

QString DeviceControlPanel::panelLabel() const
{
    // Convert panel ID to letter: 0->A, 1->B, 2->C, etc.
    QChar letter('A' + m_panelId);
    return QString("Panel %1").arg(letter);
}

bool DeviceControlPanel::isConnected() const
{
    return m_controller->isConnected();
}

bool DeviceControlPanel::isPoweredOn() const
{
    return m_controller && m_controller->isPoweredOn();
}

QString DeviceControlPanel::getPanelId() const
{
    return m_controller->getPanelId();
}

double DeviceControlPanel::getRj1Temperature() const
{
    return m_controller->getRj1Temperature();
}

void DeviceControlPanel::setDeviceInfo(const DeviceInfo &info)
{
    m_deviceInfo = info;

    // Update device name label
    m_lblDeviceName->setText(info.displayName);

    // Enable power on button since we have a valid device
    m_btnPowerOn->setEnabled(true);
}

void DeviceControlPanel::setImageLoader(ImageLoader *loader)
{
    m_imageLoader = loader;
}

void DeviceControlPanel::setVariant(const QString &variant)
{
    m_currentVariant = variant;
    int index = m_cmbVariant->findText(variant);
    if (index >= 0) {
        m_cmbVariant->setCurrentIndex(index);
    }
}

void DeviceControlPanel::setInitialBrightness(double brightness)
{
    m_currentBrightness = brightness;
    m_spinBrightness->setValue(brightness);
    m_sliderBrightness->setValue(static_cast<int>(brightness * 100));
}

bool DeviceControlPanel::powerOnDirect()
{
    if (isConnected() && m_controller->isPoweredOn()) {
        return true;
    }
    if (m_deviceInfo.index < 0) {
        return false;
    }

    QString variant = m_currentVariant;
    int deviceIndex = m_deviceInfo.index;

    bool success;
    if (m_controller->isConnected() && !m_controller->isPoweredOn()) {
        // Instance exists (from preInit or previous powerOff) — just re-power
        emit logMessage(panelLabel(), QString("Re-powering existing instance (variant: %1)...").arg(variant));
        success = m_controller->powerOn();
    } else {
        // No instance — full connect
        emit logMessage(panelLabel(), QString("Powering on device %1 (variant: %2)...")
                            .arg(deviceIndex).arg(variant));
        success = m_controller->connect(deviceIndex, variant);
    }

    // Update UI from main thread
    QString lastErr = success ? QString() : m_controller->lastError();
    QMetaObject::invokeMethod(this, [this, success, lastErr]() {
        updateUIState();
        if (success) {
            updatePanelInfo();
        } else if (lastErr.contains("UCID")) {
            // Missing HDF5 calibration file — show UCID so operator can fetch the file
            QMessageBox::warning(this, "缺少校准文件", lastErr);
        }
    }, Qt::QueuedConnection);

    return success;
}

bool DeviceControlPanel::powerOffDirect()
{
    if (!isConnected()) {
        return true;
    }

    // Thread-safe: only use m_controller, no UI widgets.
    emit logMessage(panelLabel(), "API: Powering off...");
    m_controller->disconnect();

    QMetaObject::invokeMethod(this, [this]() {
        updateUIState();
    }, Qt::QueuedConnection);

    return !isConnected();
}

bool DeviceControlPanel::sendImageDirect(const QImage &image)
{
    if (!isConnected()) {
        return false;
    }

    double patternApl = calculatePatternApl(image);
    double currentBrightness = m_currentBrightness;
    double totalApl = patternApl * currentBrightness;

    emit logMessage(panelLabel(), QString("Pattern APL: %1, Brightness: %2, Total APL: %3")
                        .arg(patternApl, 0, 'f', 4)
                        .arg(currentBrightness, 0, 'f', 2)
                        .arg(totalApl, 0, 'f', 4));

    // Check if Total APL > 0.06
    const double APL_BRIGHTNESS_LIMIT = 0.06;
    if (totalApl > APL_BRIGHTNESS_LIMIT) {
        emit logMessage(panelLabel(), QString("BLOCKED: Total APL(%1) > %2")
                            .arg(totalApl, 0, 'f', 4)
                            .arg(APL_BRIGHTNESS_LIMIT, 0, 'f', 2));
        return false;
    }

    emit logMessage(panelLabel(), QString("Sending image via API..."));
    return m_controller->sendImage(image);
}

bool DeviceControlPanel::setBrightnessDirect(double level)
{
    if (!isConnected()) {
        return false;
    }
    m_currentBrightness = level;
    m_spinBrightness->blockSignals(true);
    m_sliderBrightness->blockSignals(true);
    m_spinBrightness->setValue(level);
    m_sliderBrightness->setValue(static_cast<int>(level * 100));
    m_spinBrightness->blockSignals(false);
    m_sliderBrightness->blockSignals(false);

    bool result = m_controller->setBrightness(level);
    if (result) {
        startBrightnessProtection();
    }
    return result;
}

void DeviceControlPanel::sendCurrentImage()
{
    onSendImageClicked();
}

void DeviceControlPanel::setupUI()
{
    // Inherit font from parent (MainWindow sets global font)
    // No need to set font here as it's inherited through styleSheet

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(5);

    // Main group box with panel title
    m_grpMain = new QGroupBox(panelLabel(), this);
    QVBoxLayout *grpLayout = new QVBoxLayout(m_grpMain);
    grpLayout->setSpacing(8);

    // === Device Selection ===
    QHBoxLayout *deviceLayout = new QHBoxLayout();
    m_lblDeviceName = new QLabel("No device assigned");
    m_lblDeviceName->setMinimumWidth(150);
    m_lblDeviceName->setStyleSheet("font-weight: bold;");

    // Hardware Variant selection
    m_cmbVariant = new QComboBox();
    m_cmbVariant->addItem("standard");
    m_cmbVariant->addItem("F33R");
    m_cmbVariant->addItem("F33L");
    m_cmbVariant->addItem("F33LP");
    m_cmbVariant->setCurrentIndex(0);  // Default: standard
    m_cmbVariant->setMinimumWidth(80);

    m_btnPowerOn = new QPushButton("On");
    m_btnPowerOn->setMinimumWidth(60);
    m_btnPowerOn->setEnabled(false);  // Disabled until device info is set
    m_btnPowerOff = new QPushButton("Off");
    m_btnPowerOff->setMinimumWidth(60);
    m_lblStatus = new QLabel("Off");
    m_lblStatus->setMinimumWidth(100);
    m_lblStatus->setStyleSheet("color: gray; font-weight: bold;");

    deviceLayout->addWidget(new QLabel("Device:"));
    deviceLayout->addWidget(m_lblDeviceName, 1);
    deviceLayout->addWidget(new QLabel("Variant:"));
    deviceLayout->addWidget(m_cmbVariant);
    deviceLayout->addWidget(new QLabel("Power:"));
    deviceLayout->addWidget(m_btnPowerOn);
    deviceLayout->addWidget(m_btnPowerOff);
    deviceLayout->addWidget(m_lblStatus);
    grpLayout->addLayout(deviceLayout);

    // === Brightness Control ===
    QGroupBox *grpBrightness = new QGroupBox("Brightness");
    QHBoxLayout *brightnessLayout = new QHBoxLayout(grpBrightness);

    m_sliderBrightness = new QSlider(Qt::Horizontal);
    m_sliderBrightness->setRange(0, 100);  // 0.0 to 1.0 (step 0.01)
    m_sliderBrightness->setValue(3);       // Default 0.03

    m_spinBrightness = new QDoubleSpinBox();
    m_spinBrightness->setRange(0.0, 1.0);
    m_spinBrightness->setSingleStep(0.01);
    m_spinBrightness->setDecimals(2);
    m_spinBrightness->setValue(0.03);

    brightnessLayout->addWidget(m_sliderBrightness, 1);
    brightnessLayout->addWidget(m_spinBrightness);
    grpLayout->addWidget(grpBrightness);

    // === Flip Control ===
    QGroupBox *grpFlip = new QGroupBox("Image Flip (CalMetadata)");
    QHBoxLayout *flipLayout = new QHBoxLayout(grpFlip);

    m_chkXFlip = new QCheckBox("X-Flip");
    m_chkXFlip->setChecked(false);
    m_chkYFlip = new QCheckBox("Y-Flip");
    m_chkYFlip->setChecked(false);

    flipLayout->addWidget(m_chkXFlip);
    flipLayout->addWidget(m_chkYFlip);
    flipLayout->addStretch();
    grpLayout->addWidget(grpFlip);

    // === Panel Info ===
    QGroupBox *grpInfo = new QGroupBox("Panel Info");
    QGridLayout *infoLayout = new QGridLayout(grpInfo);

    infoLayout->addWidget(new QLabel("Panel ID:"), 0, 0);
    m_lblPanelId = new QLabel("-");
    infoLayout->addWidget(m_lblPanelId, 0, 1);

    infoLayout->addWidget(new QLabel("RJ1-RAX:"), 0, 2);
    m_lblRj1Temp = new QLabel("-");
    infoLayout->addWidget(m_lblRj1Temp, 0, 3);

    infoLayout->addWidget(new QLabel("DA9272:"), 1, 0);
    m_lblDa9272Temp = new QLabel("-");
    infoLayout->addWidget(m_lblDa9272Temp, 1, 1);

    infoLayout->addWidget(new QLabel("Brightness:"), 1, 2);
    m_lblActualBrightness = new QLabel("-");
    infoLayout->addWidget(m_lblActualBrightness, 1, 3);

    m_btnRefreshInfo = new QPushButton("Refresh");
    infoLayout->addWidget(m_btnRefreshInfo, 2, 3);

    grpLayout->addWidget(grpInfo);

    // === Send Image Button ===
    m_btnSendImage = new QPushButton(QString("Send Image to %1").arg(panelLabel()));
    m_btnSendImage->setMinimumHeight(30);
    grpLayout->addWidget(m_btnSendImage);

    grpLayout->addStretch();
    mainLayout->addWidget(m_grpMain);
}

void DeviceControlPanel::setupConnections()
{
    // Power control
    connect(m_btnPowerOn, &QPushButton::clicked, this, &DeviceControlPanel::onPowerOnClicked);
    connect(m_btnPowerOff, &QPushButton::clicked, this, &DeviceControlPanel::onPowerOffClicked);

    // Variant selection
    connect(m_cmbVariant, &QComboBox::currentTextChanged, this, &DeviceControlPanel::onVariantChanged);

    // Brightness - slider only updates display while dragging, applies on release
    connect(m_sliderBrightness, &QSlider::valueChanged, this, &DeviceControlPanel::onBrightnessSliderChanged);
    connect(m_sliderBrightness, &QSlider::sliderReleased, this, &DeviceControlPanel::onBrightnessSliderReleased);
    connect(m_spinBrightness, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &DeviceControlPanel::onBrightnessSpinBoxChanged);

    // Flip
    connect(m_chkXFlip, &QCheckBox::toggled, this, &DeviceControlPanel::onXFlipChanged);
    connect(m_chkYFlip, &QCheckBox::toggled, this, &DeviceControlPanel::onYFlipChanged);

    // Info
    connect(m_btnRefreshInfo, &QPushButton::clicked, this, &DeviceControlPanel::onRefreshInfoClicked);

    // Image
    connect(m_btnSendImage, &QPushButton::clicked, this, &DeviceControlPanel::onSendImageClicked);

    // Controller signals
    connect(m_controller.get(), &CorneaController::connected,
            this, &DeviceControlPanel::onControllerConnected);
    connect(m_controller.get(), &CorneaController::disconnected,
            this, &DeviceControlPanel::onControllerDisconnected);
    connect(m_controller.get(), &CorneaController::brightnessChanged,
            this, &DeviceControlPanel::onControllerBrightnessChanged);
    connect(m_controller.get(), &CorneaController::xFlipChanged,
            this, &DeviceControlPanel::onControllerXFlipChanged);
    connect(m_controller.get(), &CorneaController::yFlipChanged,
            this, &DeviceControlPanel::onControllerYFlipChanged);
    connect(m_controller.get(), &CorneaController::temperatureUpdated,
            this, &DeviceControlPanel::onControllerTemperatureUpdated);
    connect(m_controller.get(), &CorneaController::errorOccurred,
            this, &DeviceControlPanel::onControllerError);
    connect(m_controller.get(), &CorneaController::logMessage,
            this, &DeviceControlPanel::onControllerLog);
}

void DeviceControlPanel::updateUIState()
{
    // Use power-on state, not connected state — powerOff keeps the controller
    // instance alive (for fast re-power) so isConnected() stays true even when
    // the rails are down. UI must reflect actual power state.
    bool poweredOn = isPoweredOn();
    bool hasDevice = m_deviceInfo.index >= 0;

    // Power buttons - mutually exclusive
    m_btnPowerOn->setEnabled(!poweredOn && hasDevice);
    m_btnPowerOff->setEnabled(poweredOn);

    // Variant can only be changed when powered off
    m_cmbVariant->setEnabled(!poweredOn);

    // Other controls only make sense when powered
    m_sliderBrightness->setEnabled(poweredOn);
    m_spinBrightness->setEnabled(poweredOn);
    m_chkXFlip->setEnabled(poweredOn);
    m_chkYFlip->setEnabled(poweredOn);
    m_btnRefreshInfo->setEnabled(poweredOn);
    m_btnSendImage->setEnabled(poweredOn);

    // Update status label
    if (poweredOn) {
        m_lblStatus->setText("On");
        m_lblStatus->setStyleSheet("color: green; font-weight: bold;");
    } else {
        m_lblStatus->setText("Off");
        m_lblStatus->setStyleSheet("color: gray; font-weight: bold;");
    }
}

void DeviceControlPanel::updatePanelInfo()
{
    if (!isConnected()) {
        return;
    }

    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller]() {
        if (!guard->load()) return;
        QString panelId = controller->getPanelId();

        // Retry once if empty (device may need more time)
        if (panelId.isEmpty() && guard->load()) {
            QThread::msleep(200);
            panelId = controller->getPanelId();
        }

        if (!guard->load()) return;
        double rj1 = controller->getRj1Temperature();
        if (!guard->load()) return;
        double da9272 = controller->getDa9272Temperature();

        QMetaObject::invokeMethod(this, [this, guard, panelId, rj1, da9272]() {
            if (!guard->load()) return;
            m_lblPanelId->setText(panelId.isEmpty() ? "-" : panelId);
            onControllerTemperatureUpdated(rj1, da9272);
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onPowerOnClicked()
{
    if (m_deviceInfo.index < 0) {
        QMessageBox::warning(this, "No Device", "No device assigned to this panel.");
        return;
    }

    // Show powering on state
    m_btnPowerOn->setEnabled(false);
    m_btnPowerOff->setEnabled(false);
    m_cmbVariant->setEnabled(false);
    m_lblStatus->setText("Powering on...");
    m_lblStatus->setStyleSheet("color: orange; font-weight: bold;");

    QString variant = m_cmbVariant->currentText();
    int deviceIndex = m_deviceInfo.index;
    QString deviceName = m_deviceInfo.displayName;
    emit logMessage(panelLabel(), QString("Powering on %1 (variant: %2)...").arg(deviceName, variant));

    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller, deviceIndex, variant]() {
        if (!guard->load()) return;
        bool success;
        if (controller->isConnected() && !controller->isPoweredOn()) {
            // Instance exists (from preInit or previous powerOff) — just re-power
            // Note: if variant changed, need full reconnect
            success = controller->powerOn();
        } else if (controller->isConnected() && controller->isPoweredOn()) {
            success = true;
        } else {
            success = controller->connect(deviceIndex, variant);
        }

        QMetaObject::invokeMethod(this, [this, guard, success]() {
            if (!guard->load()) return;
            updateUIState();
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onPowerOffClicked()
{
    // Show powering off state
    m_btnPowerOn->setEnabled(false);
    m_btnPowerOff->setEnabled(false);
    m_lblStatus->setText("Powering off...");
    m_lblStatus->setStyleSheet("color: orange; font-weight: bold;");

    emit logMessage(panelLabel(), "Powering off...");

    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller]() {
        if (!guard->load()) return;
        // Software power-off only — keep instance alive for fast re-power
        controller->powerOff();

        QMetaObject::invokeMethod(this, [this, guard]() {
            if (!guard->load()) return;
            updateUIState();
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onBrightnessSliderChanged(int value)
{
    // Only update spinbox display while dragging (no I2C call)
    double brightness = value / 100.0;
    m_spinBrightness->blockSignals(true);
    m_spinBrightness->setValue(brightness);
    m_spinBrightness->blockSignals(false);
}

void DeviceControlPanel::onBrightnessSliderReleased()
{
    // Apply brightness when slider is released
    double brightness = m_sliderBrightness->value() / 100.0;
    applyBrightness(brightness);
}

void DeviceControlPanel::onBrightnessSpinBoxChanged(double value)
{
    m_sliderBrightness->blockSignals(true);
    m_sliderBrightness->setValue(static_cast<int>(value * 100));
    m_sliderBrightness->blockSignals(false);
    applyBrightness(value);
}

void DeviceControlPanel::applyBrightness(double brightness)
{
    if (!isConnected()) {
        return;
    }

    m_currentBrightness = brightness;
    emit logMessage(panelLabel(), QString("Setting brightness to %1").arg(brightness, 0, 'f', 2));

    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller, brightness]() {
        if (!guard->load()) return;
        controller->setBrightness(brightness);

        QMetaObject::invokeMethod(this, [this, guard]() {
            if (!guard->load()) return;
            // Start brightness protection monitoring after brightness is set
            startBrightnessProtection();
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::startBrightnessProtection()
{
    m_brightnessProtectionCount = 0;
    m_brightnessProtectionTimer->start(BRIGHTNESS_PROTECTION_INTERVAL_MS);
    emit logMessage(panelLabel(), "Brightness protection: monitoring temp for 5 seconds...");
}

void DeviceControlPanel::onBrightnessProtectionTimeout()
{
    if (!isConnected()) {
        m_brightnessProtectionTimer->stop();
        return;
    }

    m_brightnessProtectionCount++;
    int checkNum = m_brightnessProtectionCount;

    // Stop timer after dispatching the last check
    if (m_brightnessProtectionCount >= BRIGHTNESS_PROTECTION_CHECKS) {
        m_brightnessProtectionTimer->stop();
    }

    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller, checkNum]() {
        if (!guard->load()) return;
        double rj1 = controller->getRj1Temperature();
        if (!guard->load()) return;
        double da9272 = controller->getDa9272Temperature();

        QMetaObject::invokeMethod(this, [this, guard, rj1, da9272, checkNum]() {
            if (!guard->load() || !isConnected()) return;

            onControllerTemperatureUpdated(rj1, da9272);

            double maxTemp = qMax(rj1, da9272);
            emit logMessage(panelLabel(), QString("Protection check %1/%2: RJ1=%3°C, DA9272=%4°C")
                                .arg(checkNum)
                                .arg(BRIGHTNESS_PROTECTION_CHECKS)
                                .arg(rj1, 0, 'f', 1)
                                .arg(da9272, 0, 'f', 1));

            if (maxTemp > TEMPERATURE_LIMIT) {
                m_brightnessProtectionTimer->stop();

                QString warning = QString("OVERHEAT during brightness change!\nTemp: %1°C > %2°C limit\nEmergency power off!")
                                      .arg(maxTemp, 0, 'f', 1)
                                      .arg(TEMPERATURE_LIMIT, 0, 'f', 1);
                emit logMessage(panelLabel(), QString("EMERGENCY: %1").arg(warning));

                auto g = m_asyncGuard;
                auto c = m_controller.get();
                QtConcurrent::run([g, c]() {
                    if (!g->load()) return;
                    c->powerOff();
                });
                m_lblStatus->setText("OVERHEAT!");
                m_lblStatus->setStyleSheet("color: red; font-weight: bold;");
                // Non-modal dialog: visible but doesn't block the main thread
                auto *msg = new QMessageBox(QMessageBox::Critical, "Overheat Protection", warning, QMessageBox::Ok, this);
                msg->setAttribute(Qt::WA_DeleteOnClose);
                msg->show();
                return;
            }

            if (checkNum >= BRIGHTNESS_PROTECTION_CHECKS) {
                emit logMessage(panelLabel(), "Brightness protection: completed, temp OK");
            }
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onRefreshInfoClicked()
{
    updatePanelInfo();
    emit logMessage(panelLabel(), "Panel info refreshed");
}

void DeviceControlPanel::onSendImageClicked()
{
    if (!m_imageLoader) {
        QMessageBox::warning(this, "No Image Loader", "Image loader not set.");
        return;
    }

    QImage image = m_imageLoader->getCurrentImage();
    if (image.isNull()) {
        QMessageBox::warning(this, "No Image", "Please load an image first.");
        return;
    }

    double patternApl = calculatePatternApl(image);
    double currentBrightness = m_currentBrightness;
    double totalApl = patternApl * currentBrightness;

    emit logMessage(panelLabel(), QString("Pattern APL: %1, Brightness: %2, Total APL: %3")
                        .arg(patternApl, 0, 'f', 4)
                        .arg(currentBrightness, 0, 'f', 2)
                        .arg(totalApl, 0, 'f', 4));

    // Check if Total APL > 0.06
    const double APL_BRIGHTNESS_LIMIT = 0.06;
    if (totalApl > APL_BRIGHTNESS_LIMIT) {
        QString warning = QString("Total APL exceeds limit!\n\n"
                                  "Pattern APL: %1\n"
                                  "Brightness: %2\n"
                                  "Total APL: %3\n"
                                  "Limit: %4\n\n"
                                  "Image will NOT be sent.")
                              .arg(patternApl, 0, 'f', 4)
                              .arg(currentBrightness, 0, 'f', 2)
                              .arg(totalApl, 0, 'f', 4)
                              .arg(APL_BRIGHTNESS_LIMIT, 0, 'f', 2);
        emit logMessage(panelLabel(), QString("BLOCKED: Total APL(%1) > %2")
                            .arg(totalApl, 0, 'f', 4)
                            .arg(APL_BRIGHTNESS_LIMIT, 0, 'f', 2));
        QMessageBox::warning(this, "APL Limit Exceeded", warning);
        return;
    }

    emit logMessage(panelLabel(), QString("Sending image: %1").arg(m_imageLoader->getCurrentImageName()));

    m_btnSendImage->setEnabled(false);
    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller, image]() {
        if (!guard->load()) return;
        controller->sendImage(image);

        QMetaObject::invokeMethod(this, [this, guard]() {
            if (!guard->load()) return;
            m_btnSendImage->setEnabled(isConnected());
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onControllerConnected()
{
    updateUIState();
    emit connectionChanged(m_panelId, true);
    emit logMessage(panelLabel(), QString("Connected: %1").arg(m_controller->getDeviceLabel()));

    // Run all post-connect Python calls in background to keep UI responsive
    double savedBrightness = m_spinBrightness->value();
    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller, savedBrightness]() {
        if (!guard->load()) return;
        bool brightnessOk = controller->setBrightness(savedBrightness);
        if (!guard->load()) return;
        double actualBrightness = controller->getBrightness();
        if (!guard->load()) return;
        bool xFlip = controller->getXFlip();
        if (!guard->load()) return;
        bool yFlip = controller->getYFlip();

        // Get panel ID (with retry — device may need time after connect)
        if (!guard->load()) return;
        QString panelId = controller->getPanelId();
        if (panelId.isEmpty() && guard->load()) {
            QThread::msleep(500);
            panelId = controller->getPanelId();
        }
        if (panelId.isEmpty() && guard->load()) {
            QThread::msleep(1000);
            panelId = controller->getPanelId();
        }

        // Get initial temperatures
        if (!guard->load()) return;
        double rj1 = controller->getRj1Temperature();
        if (!guard->load()) return;
        double da9272 = controller->getDa9272Temperature();

        QMetaObject::invokeMethod(this, [this, guard, savedBrightness, brightnessOk,
                                         actualBrightness, xFlip, yFlip,
                                         panelId, rj1, da9272]() {
            if (!guard->load()) return;

            if (brightnessOk) {
                emit logMessage(panelLabel(), QString("Applied saved brightness: %1").arg(savedBrightness, 0, 'f', 2));
            }

            m_lblActualBrightness->setText(QString::number(actualBrightness, 'f', 2));

            if (qRound(actualBrightness * 100) != qRound(savedBrightness * 100)) {
                emit logMessage(panelLabel(), QString("Brightness mismatch: set=%1, actual=%2")
                                    .arg(savedBrightness, 0, 'f', 2).arg(actualBrightness, 0, 'f', 2));
            }

            m_chkXFlip->blockSignals(true);
            m_chkYFlip->blockSignals(true);
            m_chkXFlip->setChecked(xFlip);
            m_chkYFlip->setChecked(yFlip);
            m_chkXFlip->blockSignals(false);
            m_chkYFlip->blockSignals(false);
            emit logMessage(panelLabel(), QString("Flip state: X=%1, Y=%2").arg(xFlip ? "true" : "false").arg(yFlip ? "true" : "false"));

            // Update panel info labels
            m_lblPanelId->setText(panelId.isEmpty() ? "-" : panelId);
            onControllerTemperatureUpdated(rj1, da9272);
            if (!panelId.isEmpty()) {
                emit logMessage(panelLabel(), QString("Panel ID: %1").arg(panelId));
            }
        }, Qt::QueuedConnection);
    });

    // Start temperature monitoring timer, staggered by panel ID to avoid
    // all devices polling simultaneously and blocking the UI
    int staggerMs = m_panelId * 1000;  // 0s, 1s, 2s, 3s, ... offset
    QTimer::singleShot(staggerMs, this, [this]() {
        if (isConnected()) {
            m_temperatureTimer->start(TEMPERATURE_CHECK_INTERVAL_MS);
        }
    });
    emit logMessage(panelLabel(), QString("Temperature monitoring starts in %1s (%2s interval)")
                        .arg(staggerMs / 1000).arg(TEMPERATURE_CHECK_INTERVAL_MS / 1000));
}

void DeviceControlPanel::onControllerDisconnected()
{
    updateUIState();
    emit connectionChanged(m_panelId, false);

    // Stop all timers
    m_temperatureTimer->stop();
    m_brightnessProtectionTimer->stop();

    // Clear info labels
    m_lblPanelId->setText("-");
    m_lblRj1Temp->setText("-");
    m_lblDa9272Temp->setText("-");
    m_lblActualBrightness->setText("-");
}

void DeviceControlPanel::onControllerBrightnessChanged(double level)
{
    m_currentBrightness = level;
    m_sliderBrightness->blockSignals(true);
    m_spinBrightness->blockSignals(true);
    m_sliderBrightness->setValue(static_cast<int>(level * 100));
    m_spinBrightness->setValue(level);
    m_sliderBrightness->blockSignals(false);
    m_spinBrightness->blockSignals(false);
}

void DeviceControlPanel::onXFlipChanged(bool checked)
{
    if (!isConnected()) {
        return;
    }
    auto guard = m_asyncGuard;
    auto controller = m_controller.get();
    QtConcurrent::run([guard, controller, checked]() {
        if (!guard->load()) return;
        controller->setXFlip(checked);
    });
}

void DeviceControlPanel::onYFlipChanged(bool checked)
{
    if (!isConnected()) {
        return;
    }
    auto guard = m_asyncGuard;
    auto controller = m_controller.get();
    QtConcurrent::run([guard, controller, checked]() {
        if (!guard->load()) return;
        controller->setYFlip(checked);
    });
}

void DeviceControlPanel::onControllerXFlipChanged(bool flip)
{
    m_chkXFlip->blockSignals(true);
    m_chkXFlip->setChecked(flip);
    m_chkXFlip->blockSignals(false);
}

void DeviceControlPanel::onControllerYFlipChanged(bool flip)
{
    m_chkYFlip->blockSignals(true);
    m_chkYFlip->setChecked(flip);
    m_chkYFlip->blockSignals(false);
}

bool DeviceControlPanel::setXFlipDirect(bool flip)
{
    if (!isConnected()) {
        return false;
    }
    m_chkXFlip->setChecked(flip);
    return m_controller->setXFlip(flip);
}

bool DeviceControlPanel::setYFlipDirect(bool flip)
{
    if (!isConnected()) {
        return false;
    }
    m_chkYFlip->setChecked(flip);
    return m_controller->setYFlip(flip);
}

bool DeviceControlPanel::setFlipDirect(bool xFlip, bool yFlip)
{
    if (!isConnected()) {
        return false;
    }
    bool resultX = setXFlipDirect(xFlip);
    bool resultY = setYFlipDirect(yFlip);
    return resultX && resultY;
}

QString DeviceControlPanel::getVariant() const
{
    return m_cmbVariant->currentText();
}

void DeviceControlPanel::onControllerTemperatureUpdated(double rj1, double da9272)
{
    auto formatTemp = [](double temp) -> QString {
        if (temp < -900) return "-";
        return QString("%1 C").arg(temp, 0, 'f', 1);
    };

    m_lblRj1Temp->setText(formatTemp(rj1));
    m_lblDa9272Temp->setText(formatTemp(da9272));
}

void DeviceControlPanel::onTemperatureMonitorTimeout()
{
    if (!isConnected() || m_asyncTempBusy) return;
    m_asyncTempBusy = true;

    auto guard = m_asyncGuard;
    auto controller = m_controller.get();

    QtConcurrent::run([this, guard, controller]() {
        if (!guard->load()) return;
        double rj1 = controller->getRj1Temperature();
        if (!guard->load()) return;
        double da9272 = controller->getDa9272Temperature();

        QMetaObject::invokeMethod(this, [this, guard, rj1, da9272]() {
            m_asyncTempBusy = false;
            if (!guard->load() || !isConnected()) return;

            onControllerTemperatureUpdated(rj1, da9272);
            m_lblActualBrightness->setText(QString::number(m_currentBrightness, 'f', 2));

            double maxTemp = qMax(rj1, da9272);
            if (maxTemp > TEMPERATURE_LIMIT) {
                m_temperatureTimer->stop();

                QString warning = QString("Temperature exceeded %1°C!\nRJ1: %2°C, DA9272: %3°C\nDevice will be powered off.")
                                      .arg(TEMPERATURE_LIMIT, 0, 'f', 1)
                                      .arg(rj1, 0, 'f', 1)
                                      .arg(da9272, 0, 'f', 1);
                emit logMessage(panelLabel(), QString("OVERHEAT WARNING: %1").arg(warning));

                auto g = m_asyncGuard;
                auto c = m_controller.get();
                QtConcurrent::run([g, c]() {
                    if (!g->load()) return;
                    c->powerOff();
                });
                m_lblStatus->setText("OVERHEAT!");
                m_lblStatus->setStyleSheet("color: red; font-weight: bold;");
                auto *msg = new QMessageBox(QMessageBox::Critical, "Temperature Warning", warning, QMessageBox::Ok, this);
                msg->setAttribute(Qt::WA_DeleteOnClose);
                msg->show();
            }
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onControllerError(const QString &error)
{
    emit logMessage(panelLabel(), QString("ERROR: %1").arg(error));
}

void DeviceControlPanel::onControllerLog(const QString &message)
{
    emit logMessage(panelLabel(), message);
}

void DeviceControlPanel::onVariantChanged(const QString &variant)
{
    m_currentVariant = variant;
    if (!m_deviceInfo.serial.isEmpty()) {
        emit variantChanged(m_deviceInfo.serial, variant);
    }
}

ApiResult DeviceControlPanel::sendImageDirectEx(const QImage &image)
{
    if (!isConnected()) {
        return ApiResult::fail("Device not connected");
    }

    double patternApl = calculatePatternApl(image);
    double currentBrightness = m_currentBrightness;
    double totalApl = patternApl * currentBrightness;

    emit logMessage(panelLabel(), QString("Pattern APL: %1, Brightness: %2, Total APL: %3")
                        .arg(patternApl, 0, 'f', 4)
                        .arg(currentBrightness, 0, 'f', 2)
                        .arg(totalApl, 0, 'f', 4));

    // Check if Total APL > 0.06
    const double APL_BRIGHTNESS_LIMIT = 0.06;
    if (totalApl > APL_BRIGHTNESS_LIMIT) {
        QString error = QString("APL_EXCEEDED: Total APL %1 > limit %2 (pattern=%3, brightness=%4)")
                            .arg(totalApl, 0, 'f', 4)
                            .arg(APL_BRIGHTNESS_LIMIT, 0, 'f', 2)
                            .arg(patternApl, 0, 'f', 4)
                            .arg(currentBrightness, 0, 'f', 2);
        emit logMessage(panelLabel(), QString("BLOCKED: %1").arg(error));
        return ApiResult::fail(error);
    }

    emit logMessage(panelLabel(), "Sending image via API...");
    if (m_controller->sendImage(image)) {
        return ApiResult::ok();
    } else {
        return ApiResult::fail("Failed to send image to device");
    }
}

ApiResult DeviceControlPanel::setBrightnessDirectEx(double level, bool waitProtection)
{
    if (!isConnected()) {
        return ApiResult::fail("Device not connected");
    }

    m_currentBrightness = level;
    m_spinBrightness->blockSignals(true);
    m_sliderBrightness->blockSignals(true);
    m_spinBrightness->setValue(level);
    m_sliderBrightness->setValue(static_cast<int>(level * 100));
    m_spinBrightness->blockSignals(false);
    m_sliderBrightness->blockSignals(false);

    emit logMessage(panelLabel(), QString("Setting brightness to %1").arg(level, 0, 'f', 2));

    if (!m_controller->setBrightness(level)) {
        return ApiResult::fail("Failed to set brightness");
    }

    if (waitProtection) {
        // Synchronously check temperature for 5 seconds
        emit logMessage(panelLabel(), "Brightness protection: checking temp for 5 seconds...");

        for (int i = 0; i < BRIGHTNESS_PROTECTION_CHECKS; ++i) {
            QThread::msleep(BRIGHTNESS_PROTECTION_INTERVAL_MS);
            QApplication::processEvents();

            if (!isConnected()) {
                return ApiResult::fail("Device disconnected during protection check");
            }

            double rj1 = m_controller->getRj1Temperature();
            double da9272 = m_controller->getDa9272Temperature();
            onControllerTemperatureUpdated(rj1, da9272);

            double maxTemp = qMax(rj1, da9272);
            emit logMessage(panelLabel(), QString("Protection check %1/%2: RJ1=%3°C, DA9272=%4°C")
                                .arg(i + 1)
                                .arg(BRIGHTNESS_PROTECTION_CHECKS)
                                .arg(rj1, 0, 'f', 1)
                                .arg(da9272, 0, 'f', 1));

            if (maxTemp > TEMPERATURE_LIMIT) {
                QString error = QString("OVERHEAT: Temp %1°C > %2°C limit - emergency power off")
                                    .arg(maxTemp, 0, 'f', 1)
                                    .arg(TEMPERATURE_LIMIT, 0, 'f', 1);
                emit logMessage(panelLabel(), QString("EMERGENCY: %1").arg(error));
                m_controller->powerOff();
                return ApiResult::fail(error);
            }
        }
        emit logMessage(panelLabel(), "Brightness protection: completed, temp OK");
    } else {
        // Start async protection monitoring
        startBrightnessProtection();
    }

    return ApiResult::ok();
}

double DeviceControlPanel::calculatePatternApl(const QImage &image)
{
    // Pre-computed gamma 2.2 lookup table (thread-safe static initialization)
    static double gammaLUT[256];
    static bool initialized = []() {
        for (int i = 0; i < 256; ++i) {
            gammaLUT[i] = std::pow(i / 255.0, 2.2);
        }
        return true;
    }();
    Q_UNUSED(initialized);

    // Use scanLine for direct pixel access (much faster than pixelColor)
    QImage rgbImage = image.format() == QImage::Format_RGB32
        ? image : image.convertToFormat(QImage::Format_RGB32);

    double totalGamma = 0;
    int pixelCount = rgbImage.width() * rgbImage.height();

    for (int y = 0; y < rgbImage.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb*>(rgbImage.constScanLine(y));
        for (int x = 0; x < rgbImage.width(); ++x) {
            totalGamma += gammaLUT[qRed(line[x])]
                        + gammaLUT[qGreen(line[x])]
                        + gammaLUT[qBlue(line[x])];
        }
    }

    return pixelCount > 0 ? totalGamma / (pixelCount * 3.0) : 0;
}
