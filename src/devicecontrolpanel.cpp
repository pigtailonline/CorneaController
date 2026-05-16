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

// Power-on state machine — runs sync on caller thread, no UI / no event-loop
// hops. Both UI button and TCP API call this; the wrappers add their own
// UI feedback (transient "Powering on..." text, MessageBox for UCID error).
//
// Rules (from operator station logs 2026-05-10):
//  - Full instance (isInitOk) + powered off → system_power_on
//  - Already powered on → no-op
//  - Pre-init shell only (no RJ1 interface) → discard + full connect
//  - Disconnected → full connect
DeviceControlPanel::OpResult DeviceControlPanel::powerOnCore()
{
    OpResult r{};
    if (m_deviceInfo.index < 0) {
        r.error = QStringLiteral("No device assigned");
        return r;
    }
    if (m_controller->isConnected() && m_controller->isPoweredOn()) {
        r.ok = true;
        return r;
    }
    const QString variant = m_currentVariant;
    const int deviceIndex = m_deviceInfo.index;

    if (m_controller->isConnected() && m_controller->isInitOk() && !m_controller->isPoweredOn()) {
        emit logMessage(panelLabel(), QString("Re-powering existing instance (variant: %1)...").arg(variant));
        r.ok = m_controller->powerOn();
    } else {
        if (m_controller->isConnected() && !m_controller->isInitOk()) {
            emit logMessage(panelLabel(), QStringLiteral("Discarding pre-init shell, doing full connect..."));
            m_controller->disconnect();
        }
        emit logMessage(panelLabel(), QString("Powering on device %1 (variant: %2)...")
                            .arg(deviceIndex).arg(variant));
        r.ok = m_controller->connect(deviceIndex, variant);
    }
    if (!r.ok) r.error = m_controller->lastError();
    return r;
}

DeviceControlPanel::OpResult DeviceControlPanel::powerOffCore()
{
    OpResult r{};
    if (!isConnected()) {
        r.ok = true;   // already off — idempotent success
        return r;
    }
    m_controller->disconnect();
    r.ok = !isConnected();
    if (!r.ok) r.error = QStringLiteral("Failed to disconnect");
    return r;
}

bool DeviceControlPanel::powerOnDirect()
{
    OpResult r = powerOnCore();
    QString lastErr = r.error;
    bool success = r.ok;
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
    emit logMessage(panelLabel(), QStringLiteral("API: Powering off..."));
    OpResult r = powerOffCore();
    QMetaObject::invokeMethod(this, [this]() {
        updateUIState();
    }, Qt::QueuedConnection);
    return r.ok;
}

// Single source of truth for "send image to panel". TCP and UI both go
// through here so the safety / logging / protection trigger logic exists in
// exactly one place — preventing the TCP-vs-UI drift bugs we saw before.
//
// Returns enough information for callers to format their own response
// (bool / ApiResult / MessageBox). Does NOT block on a thread it shouldn't:
// the actual sendImage call is synchronous on the calling thread, so UI
// callers MUST wrap in QtConcurrent::run themselves.
//
// Protection trigger rules (2026-05-16):
//  - 1Hz × 5s temperature polling fires ONLY when the image's intrinsic APL
//    (independent of current brightness) is ≥ 0.06. Low-APL images cannot
//    drive the panel above the safe envelope at any brightness ≤ 1.0, so
//    polling is skipped — drops libusb-win32 pressure ~78% across the
//    18-image test sequence (only ~4/18 patterns hit the threshold).
DeviceControlPanel::SendImageResult DeviceControlPanel::sendImageCore(const QImage &image)
{
    SendImageResult r{};
    if (!isConnected()) {
        r.error = QStringLiteral("Device not connected");
        return r;
    }

    r.patternApl       = calculatePatternApl(image);
    r.currentBrightness = m_currentBrightness;
    r.totalApl         = r.patternApl * r.currentBrightness;

    emit logMessage(panelLabel(), QString("Pattern APL: %1, Brightness: %2, Total APL: %3")
                        .arg(r.patternApl, 0, 'f', 4)
                        .arg(r.currentBrightness, 0, 'f', 2)
                        .arg(r.totalApl, 0, 'f', 4));

    // Check if Total APL > 0.06 (with epsilon tolerance).
    // 1.5M-pixel accumulation in calculatePatternApl plus QDoubleSpinBox's
    // floating-point representation of 0.06 push the boundary case
    // (100% white × 0.06) just above 0.06 in MSVC's math, even though it
    // mathematically equals the limit exactly. Customers explicitly want
    // 0.06 × 255-white to send; treat 0.0600 → 0.0601 as on-limit.
    const double APL_LIMIT_EPSILON = 1e-4;
    if (r.totalApl > r.aplLimit + APL_LIMIT_EPSILON) {
        r.blockedByApl = true;
        r.error = QString("APL_EXCEEDED: Total APL %1 > limit %2 (pattern=%3, brightness=%4)")
                      .arg(r.totalApl, 0, 'f', 4)
                      .arg(r.aplLimit, 0, 'f', 2)
                      .arg(r.patternApl, 0, 'f', 4)
                      .arg(r.currentBrightness, 0, 'f', 2);
        emit logMessage(panelLabel(), QString("BLOCKED: %1").arg(r.error));
        return r;
    }

    emit logMessage(panelLabel(), QStringLiteral("Sending image via API..."));
    r.sent = m_controller->sendImage(image);
    if (r.sent && r.patternApl >= r.aplLimit) {
        // Image change is when heat ramps fastest (panel chip drives all
        // sub-pixels up at once). Customer event 2026-04-29: panel went
        // 25°C → 76.6°C in 7s after a single image send because the 5s
        // background monitor missed the ramp window. The 1Hz × 5s fast-
        // polling cycle catches a ramp within ≤1s of crossing the 65°C
        // limit. Only fired for ramp-risk images (apl ≥ 0.06) per the
        // 2026-05-16 conditional-trigger change.
        startBrightnessProtection();
    }
    if (!r.sent && r.error.isEmpty()) {
        r.error = QStringLiteral("Failed to send image to device");
    }
    return r;
}

bool DeviceControlPanel::sendImageDirect(const QImage &image)
{
    SendImageResult r = sendImageCore(image);
    return r.sent;
}

// Single source of truth for "set panel brightness". TCP and UI both go
// through here. setBrightness ALWAYS triggers protection (regardless of
// level) — a brightness ramp itself can drive heat. The conditional
// protection logic is on the sendImage side only (see sendImageCore).
//
// waitProtection=true performs a SYNCHRONOUS 1Hz × 5s protection loop in-
// line (caller blocks ~5s, gets fail if overheat); waitProtection=false
// starts the async timer-driven version and returns immediately. Note: the
// sync loop calls QApplication::processEvents() — same pre-refactor
// behavior, even when invoked from a TCP worker thread.
DeviceControlPanel::SetBrightnessResult DeviceControlPanel::setBrightnessCore(double level, bool waitProtection)
{
    SetBrightnessResult r{};
    if (!isConnected()) {
        r.error = QStringLiteral("Device not connected");
        return r;
    }

    // Cache + reflect on UI widgets. Widget setValue from a non-GUI thread is
    // technically UB but blockSignals + no repaint trigger has been stable in
    // production — preserving pre-refactor behavior.
    m_currentBrightness = level;
    m_spinBrightness->blockSignals(true);
    m_sliderBrightness->blockSignals(true);
    m_spinBrightness->setValue(level);
    m_sliderBrightness->setValue(static_cast<int>(level * 100));
    m_spinBrightness->blockSignals(false);
    m_sliderBrightness->blockSignals(false);

    emit logMessage(panelLabel(), QString("Setting brightness to %1").arg(level, 0, 'f', 2));

    if (!m_controller->setBrightness(level)) {
        r.error = QStringLiteral("Failed to set brightness");
        return r;
    }
    r.ok = true;

    if (waitProtection) {
        emit logMessage(panelLabel(), QStringLiteral("Brightness protection: checking temp for 5 seconds..."));
        for (int i = 0; i < BRIGHTNESS_PROTECTION_CHECKS; ++i) {
            QThread::msleep(BRIGHTNESS_PROTECTION_INTERVAL_MS);
            QApplication::processEvents();
            if (!isConnected()) {
                r.ok = false;
                r.error = QStringLiteral("Device disconnected during protection check");
                return r;
            }
            r.rj1Temp    = m_controller->getRj1Temperature();
            r.da9272Temp = m_controller->getDa9272Temperature();
            onControllerTemperatureUpdated(r.rj1Temp, r.da9272Temp);

            double maxTemp = qMax(r.rj1Temp, r.da9272Temp);
            emit logMessage(panelLabel(), QString("Protection check %1/%2: RJ1=%3°C, DA9272=%4°C")
                                .arg(i + 1)
                                .arg(BRIGHTNESS_PROTECTION_CHECKS)
                                .arg(r.rj1Temp, 0, 'f', 1)
                                .arg(r.da9272Temp, 0, 'f', 1));
            if (maxTemp > TEMPERATURE_LIMIT) {
                r.ok = false;
                r.overheated = true;
                r.error = QString("OVERHEAT (>%1°C) - emergency power off (RJ1=%2°C, DA9272=%3°C)")
                              .arg(TEMPERATURE_LIMIT, 0, 'f', 1)
                              .arg(r.rj1Temp, 0, 'f', 1)
                              .arg(r.da9272Temp, 0, 'f', 1);
                emit logMessage(panelLabel(), QString("EMERGENCY: %1").arg(r.error));
                m_controller->powerOff();
                return r;
            }
        }
        emit logMessage(panelLabel(), QStringLiteral("Brightness protection: completed, temp OK"));
    } else {
        // Async — 1Hz × 5s timer-driven protection. Hops to GUI thread inside.
        startBrightnessProtection();
    }
    return r;
}

bool DeviceControlPanel::setBrightnessDirect(double level)
{
    return setBrightnessCore(level, /*waitProtection=*/false).ok;
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

    // Hardware Variant selection. Each variant string maps to an entry in
    // ar_display_lab_lib's HARDWARE_VARIANTS dict (cornea_rax720.py:263).
    // standard/F33RP share one I2C config (PMIC=0x6A, IMU=0x6B, VLED=0x20/0x30);
    // F33R/F43V2 share another (PMIC=0x7A); F33L, F33LP, F33RV2, F33LV2 are
    // each distinct. Picking the wrong variant for a given driver board fails
    // init with NACK at the expected-but-empty I2C address.
    m_cmbVariant = new QComboBox();
    m_cmbVariant->addItem("standard");   // alias of F33RP — R74, F43 V1.0, SS, V53, H79
    m_cmbVariant->addItem("F33RP");      // alias of standard — Pilot run-LBU (IMU DNP)
    m_cmbVariant->addItem("F33R");       // alias of F43V2 — PMIC=0x7A
    m_cmbVariant->addItem("F43V2");      // alias of F33R — F43 V2.0
    m_cmbVariant->addItem("F33L");       // PMIC=0x7B, VLED_r=0x32
    m_cmbVariant->addItem("F33LP");      // Pilot run-LBU — PMIC=0x6A, VLED_r=0x32
    m_cmbVariant->addItem("F33RV2");     // F33R V2.0 — Max77675@0x44 + Max77713@0x65
    m_cmbVariant->addItem("F33LV2");     // F33L V2.0 — Max77675@0x52 + Max77713@0x6D
    m_cmbVariant->setCurrentIndex(0);    // Default: standard
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

    // Refresh UI whenever the controller's power state changes. Without this
    // connection, TCP-triggered powerOn/powerOff (called via the controller
    // directly rather than the GUI buttons) leaves the Power On/Off buttons
    // and the status label stale: the panel is actually lit and reports a
    // Panel ID, but the UI still shows "Off". Using Qt::QueuedConnection so
    // the UI update always happens on the GUI thread even though the signal
    // is emitted from the Python/worker thread.
    //
    // On power-on (re-power of an existing instance OR fresh connect), also
    // re-read panel ID + temperature. Without this, swapping the panel while
    // the same USB controller stays plugged in leaves the UI showing the
    // previous panel's ID/temp because onControllerConnected() only fires on
    // a fresh USB-level connect.
    connect(m_controller.get(), &CorneaController::powerStateChanged,
            this, [this](bool poweredOn){
                updateUIState();
                if (poweredOn) {
                    updatePanelInfo();
                    // Re-apply brightness after power-on. With the pre-init
                    // optimization (keep instance alive across PowerOff), a
                    // re-power does NOT trigger onControllerConnected, so the
                    // saved brightness was never re-asserted on the panel
                    // hardware. rax_lib resets panel brightness to its default
                    // (full) on each power-up, so without this re-apply the
                    // next sendImage runs at 100 % brightness even though the
                    // UI still shows 0.06 — directly causing overheat at the
                    // first red frame after PowerOff/PowerOn.
                    auto guard = m_asyncGuard;
                    auto controller = m_controller.get();
                    double savedBrightness = m_currentBrightness;
                    QtConcurrent::run([guard, controller, savedBrightness]() {
                        if (!guard->load()) return;
                        controller->setBrightness(savedBrightness);
                    });
                } else {
                    // Power-off without USB disconnect (PowerOff button, TCP
                    // powerOff, or overheat-triggered shutdown). Clear the panel
                    // info labels so a stale temperature reading doesn't linger
                    // on screen across the off-period and confuse the operator
                    // during the next power-on while updatePanelInfo() is still
                    // resolving. Without this, the UI shows the last-known temp
                    // (often the 65 C overheat trip value) for the entire init
                    // sequence, making it look like the freshly-powered panel
                    // is reading 65 C immediately.
                    m_lblPanelId->setText("-");
                    m_lblRj1Temp->setText("-");
                    m_lblDa9272Temp->setText("-");
                    m_lblActualBrightness->setText("-");
                }
            },
            Qt::QueuedConnection);
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
    // Suppress while a power op is mid-flight: discarding a pre-init shell
    // emits 'disconnected', which routes through updateUIState() and would
    // otherwise re-enable PowerOn during the 5+ s full-init window, letting
    // a quick second click spawn a duplicate CorneaRax720 that races on
    // pyftdi resources.
    m_btnPowerOn->setEnabled(!poweredOn && hasDevice && !m_powerOpInProgress);
    m_btnPowerOff->setEnabled(poweredOn && !m_powerOpInProgress);

    // Variant can only be changed when powered off
    m_cmbVariant->setEnabled(!poweredOn);

    // Other controls only make sense when powered
    m_sliderBrightness->setEnabled(poweredOn);
    m_spinBrightness->setEnabled(poweredOn);
    m_chkXFlip->setEnabled(poweredOn);
    m_chkYFlip->setEnabled(poweredOn);
    m_btnRefreshInfo->setEnabled(poweredOn);
    m_btnSendImage->setEnabled(poweredOn);

    // Update status label.
    // While a power op is in flight, leave whatever transient text the
    // op's start handler set (e.g. "Powering on...") — without this guard,
    // the disconnect() inside the powerOn worker emits 'disconnected',
    // which queues an updateUIState() that overwrites "Powering on..."
    // with "Off" until the connect() finishes 5 s later, producing a
    // user-visible "Connecting → Off → On" oscillation.
    if (m_powerOpInProgress) {
        // status label preserved by the on-click handler
    } else if (poweredOn) {
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
    if (m_powerOpInProgress) {
        // Double-click guard — let in-flight op finish.
        return;
    }
    m_powerOpInProgress = true;

    // Transient UI feedback
    m_btnPowerOn->setEnabled(false);
    m_btnPowerOff->setEnabled(false);
    m_cmbVariant->setEnabled(false);
    m_lblStatus->setText("Powering on...");
    m_lblStatus->setStyleSheet("color: orange; font-weight: bold;");
    emit logMessage(panelLabel(), QString("Powering on %1 (variant: %2)...")
                        .arg(m_deviceInfo.displayName, m_currentVariant));

    auto guard = m_asyncGuard;
    QtConcurrent::run([this, guard]() {
        if (!guard->load()) return;
        powerOnCore();   // logs success/fail internally
        QMetaObject::invokeMethod(this, [this, guard]() {
            if (!guard->load()) return;
            m_powerOpInProgress = false;
            updateUIState();
        }, Qt::QueuedConnection);
    });
}

void DeviceControlPanel::onPowerOffClicked()
{
    if (m_powerOpInProgress) {
        return;
    }
    m_powerOpInProgress = true;
    m_btnPowerOn->setEnabled(false);
    m_btnPowerOff->setEnabled(false);
    m_lblStatus->setText("Powering off...");
    m_lblStatus->setStyleSheet("color: orange; font-weight: bold;");
    emit logMessage(panelLabel(), QStringLiteral("Powering off..."));

    auto guard = m_asyncGuard;
    QtConcurrent::run([this, guard]() {
        if (!guard->load()) return;
        powerOffCore();
        QMetaObject::invokeMethod(this, [this, guard]() {
            if (!guard->load()) return;
            m_powerOpInProgress = false;
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
    // Dispatch via setBrightnessCore on a background thread so the UI stays
    // responsive. waitProtection=false because the UI doesn't want a 5s
    // synchronous block — the async timer-driven protection handles overheat.
    auto guard = m_asyncGuard;
    QtConcurrent::run([this, guard, brightness]() {
        if (!guard->load()) return;
        setBrightnessCore(brightness, /*waitProtection=*/false);
    });
}

void DeviceControlPanel::startBrightnessProtection()
{
    // Always hop to the GUI thread before touching m_brightnessProtectionTimer.
    // Several callers (setBrightnessDirectEx and sendImageDirectEx via the TCP
    // server, plus sendImageDirect from the TCP-side ApiResult helpers) run on
    // the TCP server's worker thread, not the GUI thread. Calling
    // QTimer::start() from a different thread than the timer's owning thread
    // is undefined behavior in Qt — under load it corrupts QString internal
    // state and surfaces as a Q_UNREACHABLE in qtextengine.cpp the next time a
    // QLabel is repainted.
    //
    // 2026-04-30 customer event reproduced locally via Debug build + the
    // multi-panel stress test: 3 simulated panels hammering setBrightness +
    // sendImage triggered the assert within seconds. After this wrap the same
    // stress test runs clean.
    //
    // Reset remaining checks to the full count. If the timer is already
    // running (e.g. another setBrightness came in mid-protection), this
    // EXTENDS the protection window rather than stacking multiple runs.
    // The CorneaController cache ensures redundant reads within 1 s are
    // deduplicated, so extending never creates extra I2C traffic.
    QMetaObject::invokeMethod(this, [this]() {
        m_brightnessProtectionCount = 0;
        if (!m_brightnessProtectionTimer->isActive()) {
            m_brightnessProtectionTimer->start(BRIGHTNESS_PROTECTION_INTERVAL_MS);
            emit logMessage(panelLabel(), "Brightness protection: monitoring temp for 5 seconds...");
        } else {
            emit logMessage(panelLabel(), "Brightness protection: extended (5 more seconds)");
        }
    }, Qt::QueuedConnection);
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

            // Single-sample trigger at TEMPERATURE_LIMIT. Stop BOTH timers
            // FIRST so a still-pending main monitor poll can't double-fire
            // while powerOff is in progress.
            if (maxTemp > TEMPERATURE_LIMIT) {
                m_brightnessProtectionTimer->stop();
                m_temperatureTimer->stop();

                QString warning = QString("OVERHEAT during brightness change (>%1°C)\nRJ1: %2°C, DA9272: %3°C\nEmergency power off!")
                                      .arg(TEMPERATURE_LIMIT, 0, 'f', 1)
                                      .arg(rj1, 0, 'f', 1)
                                      .arg(da9272, 0, 'f', 1);
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

    emit logMessage(panelLabel(), QString("Sending image: %1").arg(m_imageLoader->getCurrentImageName()));

    m_btnSendImage->setEnabled(false);
    auto guard = m_asyncGuard;

    // Dispatch to background thread. sendImageCore does APL check + send +
    // conditional protection trigger — same path as TCP, so any drift bug
    // would surface in both. UI MessageBox is shown on completion, ~50ms
    // later than the pre-refactor synchronous check (calculatePatternApl
    // cost), which is acceptable for click feedback.
    QtConcurrent::run([this, guard, image]() {
        if (!guard->load()) return;
        SendImageResult r = sendImageCore(image);
        QMetaObject::invokeMethod(this, [this, guard, r]() {
            if (!guard->load()) return;
            m_btnSendImage->setEnabled(isConnected());
            if (r.blockedByApl) {
                QString warning = QString("Total APL exceeds limit!\n\n"
                                          "Pattern APL: %1\n"
                                          "Brightness: %2\n"
                                          "Total APL: %3\n"
                                          "Limit: %4\n\n"
                                          "Image was NOT sent.")
                                      .arg(r.patternApl, 0, 'f', 4)
                                      .arg(r.currentBrightness, 0, 'f', 2)
                                      .arg(r.totalApl, 0, 'f', 4)
                                      .arg(r.aplLimit, 0, 'f', 2);
                QMessageBox::warning(this, "APL Limit Exceeded", warning);
            }
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
    if (!isConnected()) return;
    auto guard = m_asyncGuard;
    QtConcurrent::run([this, guard, checked]() {
        if (!guard->load()) return;
        setXFlipCore(checked);
    });
}

void DeviceControlPanel::onYFlipChanged(bool checked)
{
    if (!isConnected()) return;
    auto guard = m_asyncGuard;
    QtConcurrent::run([this, guard, checked]() {
        if (!guard->load()) return;
        setYFlipCore(checked);
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

DeviceControlPanel::OpResult DeviceControlPanel::setXFlipCore(bool flip)
{
    OpResult r{};
    if (!isConnected()) {
        r.error = QStringLiteral("Device not connected");
        return r;
    }
    // Reflect the new state on the UI checkbox. blockSignals avoids the
    // change emitting onXFlipChanged in a loop. (Pre-refactor sync path
    // wrote to m_chkXFlip without blockSignals — UI slot path could re-fire.)
    m_chkXFlip->blockSignals(true);
    m_chkXFlip->setChecked(flip);
    m_chkXFlip->blockSignals(false);
    r.ok = m_controller->setXFlip(flip);
    if (!r.ok) r.error = QStringLiteral("Failed to set X flip");
    return r;
}

DeviceControlPanel::OpResult DeviceControlPanel::setYFlipCore(bool flip)
{
    OpResult r{};
    if (!isConnected()) {
        r.error = QStringLiteral("Device not connected");
        return r;
    }
    m_chkYFlip->blockSignals(true);
    m_chkYFlip->setChecked(flip);
    m_chkYFlip->blockSignals(false);
    r.ok = m_controller->setYFlip(flip);
    if (!r.ok) r.error = QStringLiteral("Failed to set Y flip");
    return r;
}

DeviceControlPanel::OpResult DeviceControlPanel::setFlipCore(bool xFlip, bool yFlip)
{
    OpResult rx = setXFlipCore(xFlip);
    OpResult ry = setYFlipCore(yFlip);
    OpResult r{};
    r.ok = rx.ok && ry.ok;
    if (!r.ok) r.error = rx.ok ? ry.error : rx.error;
    return r;
}

bool DeviceControlPanel::setXFlipDirect(bool flip)  { return setXFlipCore(flip).ok; }
bool DeviceControlPanel::setYFlipDirect(bool flip)  { return setYFlipCore(flip).ok; }
bool DeviceControlPanel::setFlipDirect(bool xFlip, bool yFlip) { return setFlipCore(xFlip, yFlip).ok; }

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

            // Single-sample trigger at TEMPERATURE_LIMIT (65 °C). Stop the
            // timer FIRST so a still-pending poll can't fire a duplicate
            // overheat popup while powerOff() is still executing.
            double maxTemp = qMax(rj1, da9272);
            if (maxTemp > TEMPERATURE_LIMIT) {
                m_temperatureTimer->stop();
                m_brightnessProtectionTimer->stop();

                QString warning = QString("Temperature exceeded %1°C limit\nRJ1: %2°C, DA9272: %3°C\nDevice will be powered off.")
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
    SendImageResult r = sendImageCore(image);
    if (r.sent) return ApiResult::ok();
    return ApiResult::fail(r.error);
}

ApiResult DeviceControlPanel::setBrightnessDirectEx(double level, bool waitProtection)
{
    auto r = setBrightnessCore(level, waitProtection);
    return r.ok ? ApiResult::ok() : ApiResult::fail(r.error);
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
