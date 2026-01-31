#include "devicecontrolpanel.h"
#include "corneacontroller.h"
#include "imageloader.h"
#include "corneawidget.h"  // For ApiResult

#include <cmath>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QApplication>
#include <QThread>

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
}

DeviceControlPanel::~DeviceControlPanel()
{
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
    int index = m_cmbVariant->findText(variant);
    if (index >= 0) {
        m_cmbVariant->setCurrentIndex(index);
    }
}

void DeviceControlPanel::setInitialBrightness(double brightness)
{
    m_spinBrightness->setValue(brightness);
    m_sliderBrightness->setValue(static_cast<int>(brightness * 100));
}

bool DeviceControlPanel::powerOnDirect()
{
    if (isConnected()) {
        return true;  // Already connected
    }
    onPowerOnClicked();
    return isConnected();
}

bool DeviceControlPanel::powerOffDirect()
{
    if (!isConnected()) {
        return true;  // Already disconnected
    }
    onPowerOffClicked();
    return !isConnected();
}

bool DeviceControlPanel::sendImageDirect(const QImage &image)
{
    if (!isConnected()) {
        return false;
    }

    // Calculate Pattern APL using gamma 2.2 formula:
    // Pattern APL = Σ[(R/255)^2.2 + (G/255)^2.2 + (B/255)^2.2] / (width * height * 3)
    double totalGammaValue = 0;
    int pixelCount = image.width() * image.height();

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QColor color = image.pixelColor(x, y);
            double r = color.red() / 255.0;
            double g = color.green() / 255.0;
            double b = color.blue() / 255.0;
            totalGammaValue += std::pow(r, 2.2) + std::pow(g, 2.2) + std::pow(b, 2.2);
        }
    }

    double patternApl = totalGammaValue / (pixelCount * 3.0);
    double currentBrightness = m_spinBrightness->value();
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
    bool connected = isConnected();
    bool hasDevice = m_deviceInfo.index >= 0;

    // Power buttons - mutually exclusive
    m_btnPowerOn->setEnabled(!connected && hasDevice);
    m_btnPowerOff->setEnabled(connected);

    // Variant can only be changed when powered off
    m_cmbVariant->setEnabled(!connected);

    // Other controls
    m_sliderBrightness->setEnabled(connected);
    m_spinBrightness->setEnabled(connected);
    m_chkXFlip->setEnabled(connected);
    m_chkYFlip->setEnabled(connected);
    m_btnRefreshInfo->setEnabled(connected);
    m_btnSendImage->setEnabled(connected);

    // Update status label
    if (connected) {
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

    // Try to get panel info with retry
    QString panelId = m_controller->getPanelId();

    // Retry once if empty (device may need more time)
    if (panelId.isEmpty()) {
        QThread::msleep(200);
        panelId = m_controller->getPanelId();
    }

    m_lblPanelId->setText(panelId.isEmpty() ? "-" : panelId);

    // Update temperatures (RJ1-RAX and DA9272 only)
    double rj1 = m_controller->getRj1Temperature();
    double da9272 = m_controller->getDa9272Temperature();
    onControllerTemperatureUpdated(rj1, da9272);
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
    QApplication::processEvents();

    QString variant = m_cmbVariant->currentText();
    emit logMessage(panelLabel(), QString("Powering on %1 (variant: %2)...").arg(m_deviceInfo.displayName, variant));

    bool success = m_controller->connect(m_deviceInfo.index, variant);

    updateUIState();

    if (success) {
        // Small delay to let device fully initialize before reading info
        QApplication::processEvents();
        QThread::msleep(100);
        updatePanelInfo();
    }
}

void DeviceControlPanel::onPowerOffClicked()
{
    // Show powering off state
    m_btnPowerOn->setEnabled(false);
    m_btnPowerOff->setEnabled(false);
    m_lblStatus->setText("Powering off...");
    m_lblStatus->setStyleSheet("color: orange; font-weight: bold;");
    QApplication::processEvents();

    emit logMessage(panelLabel(), "Powering off...");
    m_controller->disconnect();

    updateUIState();
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

    emit logMessage(panelLabel(), QString("Setting brightness to %1").arg(brightness, 0, 'f', 2));
    m_controller->setBrightness(brightness);

    // Start brightness protection monitoring
    startBrightnessProtection();
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

    // Get current temperature (RJ1)
    double rj1 = m_controller->getRj1Temperature();
    double da9272 = m_controller->getDa9272Temperature();

    // Update UI
    onControllerTemperatureUpdated(rj1, da9272);

    double maxTemp = qMax(rj1, da9272);
    emit logMessage(panelLabel(), QString("Protection check %1/5: RJ1=%2°C, DA9272=%3°C")
                        .arg(m_brightnessProtectionCount)
                        .arg(rj1, 0, 'f', 1)
                        .arg(da9272, 0, 'f', 1));

    // Check temperature limit
    if (maxTemp > TEMPERATURE_LIMIT) {
        m_brightnessProtectionTimer->stop();

        QString warning = QString("OVERHEAT during brightness change!\nTemp: %1°C > %2°C limit\nEmergency power off!")
                              .arg(maxTemp, 0, 'f', 1)
                              .arg(TEMPERATURE_LIMIT, 0, 'f', 1);
        emit logMessage(panelLabel(), QString("EMERGENCY: %1").arg(warning));

        // Emergency power off
        m_controller->powerOff();

        QMessageBox::critical(this, "Overheat Protection", warning);
        return;
    }

    // Stop after 5 checks
    if (m_brightnessProtectionCount >= BRIGHTNESS_PROTECTION_CHECKS) {
        m_brightnessProtectionTimer->stop();
        emit logMessage(panelLabel(), "Brightness protection: completed, temp OK");
    }
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

    // Calculate Pattern APL using gamma 2.2 formula:
    // Pattern APL = Σ[(R/255)^2.2 + (G/255)^2.2 + (B/255)^2.2] / (width * height * 3)
    double totalGammaValue = 0;
    int pixelCount = image.width() * image.height();

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QColor color = image.pixelColor(x, y);
            double r = color.red() / 255.0;
            double g = color.green() / 255.0;
            double b = color.blue() / 255.0;
            totalGammaValue += std::pow(r, 2.2) + std::pow(g, 2.2) + std::pow(b, 2.2);
        }
    }

    double patternApl = totalGammaValue / (pixelCount * 3.0);
    double currentBrightness = m_spinBrightness->value();
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
    m_controller->sendImage(image);
}

void DeviceControlPanel::onControllerConnected()
{
    updateUIState();
    emit connectionChanged(m_panelId, true);
    emit logMessage(panelLabel(), QString("Connected: %1").arg(m_controller->getDeviceLabel()));

    // Apply saved brightness from config when device connects
    double savedBrightness = m_spinBrightness->value();
    if (m_controller->setBrightness(savedBrightness)) {
        emit logMessage(panelLabel(), QString("Applied saved brightness: %1").arg(savedBrightness, 0, 'f', 2));
    }

    // Get actual brightness from device and verify
    double actualBrightness = m_controller->getBrightness();
    m_lblActualBrightness->setText(QString::number(actualBrightness, 'f', 2));

    // Check if brightness values match (compare to 2 decimal places)
    if (qRound(actualBrightness * 100) != qRound(savedBrightness * 100)) {
        QString warning = QString("Brightness mismatch!\nSet: %1, Actual: %2")
                              .arg(savedBrightness, 0, 'f', 2)
                              .arg(actualBrightness, 0, 'f', 2);
        emit logMessage(panelLabel(), warning);
        QMessageBox::warning(this, "Brightness Warning", warning);
    }

    // Read current flip state from device
    bool xFlip = m_controller->getXFlip();
    bool yFlip = m_controller->getYFlip();
    m_chkXFlip->blockSignals(true);
    m_chkYFlip->blockSignals(true);
    m_chkXFlip->setChecked(xFlip);
    m_chkYFlip->setChecked(yFlip);
    m_chkXFlip->blockSignals(false);
    m_chkYFlip->blockSignals(false);
    emit logMessage(panelLabel(), QString("Flip state: X=%1, Y=%2").arg(xFlip ? "true" : "false").arg(yFlip ? "true" : "false"));

    // Start temperature monitoring timer (5 seconds interval)
    m_temperatureTimer->start(TEMPERATURE_CHECK_INTERVAL_MS);
    emit logMessage(panelLabel(), "Temperature monitoring started (5s interval)");
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
    m_controller->setXFlip(checked);
}

void DeviceControlPanel::onYFlipChanged(bool checked)
{
    if (!isConnected()) {
        return;
    }
    m_controller->setYFlip(checked);
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
    if (!isConnected()) {
        return;
    }

    // Get current temperatures
    double rj1 = m_controller->getRj1Temperature();
    double da9272 = m_controller->getDa9272Temperature();

    // Update UI
    onControllerTemperatureUpdated(rj1, da9272);

    // Also update actual brightness
    double actualBrightness = m_controller->getBrightness();
    m_lblActualBrightness->setText(QString::number(actualBrightness, 'f', 2));

    // Check temperature limit (use the higher of the two temperatures)
    double maxTemp = qMax(rj1, da9272);
    if (maxTemp > TEMPERATURE_LIMIT) {
        // Stop timer first to prevent multiple warnings
        m_temperatureTimer->stop();

        QString warning = QString("Temperature exceeded %1°C!\nRJ1: %2°C, DA9272: %3°C\nDevice will be powered off.")
                              .arg(TEMPERATURE_LIMIT, 0, 'f', 1)
                              .arg(rj1, 0, 'f', 1)
                              .arg(da9272, 0, 'f', 1);
        emit logMessage(panelLabel(), QString("OVERHEAT WARNING: %1").arg(warning));

        // Power off the device
        m_controller->powerOff();

        // Show warning to user
        QMessageBox::critical(this, "Temperature Warning", warning);
    }
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
    if (!m_deviceInfo.serial.isEmpty()) {
        emit variantChanged(m_deviceInfo.serial, variant);
    }
}

ApiResult DeviceControlPanel::sendImageDirectEx(const QImage &image)
{
    if (!isConnected()) {
        return ApiResult::fail("Device not connected");
    }

    // Calculate Pattern APL using gamma 2.2 formula
    double totalGammaValue = 0;
    int pixelCount = image.width() * image.height();

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QColor color = image.pixelColor(x, y);
            double r = color.red() / 255.0;
            double g = color.green() / 255.0;
            double b = color.blue() / 255.0;
            totalGammaValue += std::pow(r, 2.2) + std::pow(g, 2.2) + std::pow(b, 2.2);
        }
    }

    double patternApl = totalGammaValue / (pixelCount * 3.0);
    double currentBrightness = m_spinBrightness->value();
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
