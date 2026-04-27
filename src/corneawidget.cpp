#include "corneawidget.h"
#include "ui_corneawidget.h"
#include "pythonbridge.h"
#include "devicecontrolpanel.h"
#include "imageloader.h"
#include "tcpserver.h"
#include "corneacontroller.h"

#include <QMessageBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QResizeEvent>
#include <QThread>

CorneaWidget::CorneaWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::CorneaWidget)
    , m_pythonBridge(std::make_unique<PythonBridge>(this))
    , m_imageLoader(std::make_unique<ImageLoader>(this))
    , m_pythonOutputTimer(new QTimer(this))
{
    ui->setupUi(this);
    ui->txtLog->document()->setMaximumBlockCount(5000);
    setupConnections();
}

CorneaWidget::~CorneaWidget()
{
    m_pythonOutputTimer->stop();

    // Stop TCP server before deleting UI — TcpServer::stop() emits logMessage
    // which writes to ui->txtLog, so the server must be stopped while UI is alive
    if (m_tcpServer) {
        m_tcpServer->disconnect(this);
        m_tcpServer.reset();
    }

    // Destroy device panels before members (PythonBridge) are destroyed.
    // Panels are QWidget children, normally destroyed in QWidget::~QWidget()
    // which runs AFTER member destruction — but panels access PythonBridge
    // in their destructor, so we must delete them while PythonBridge is alive.
    qDeleteAll(m_devicePanels);
    m_devicePanels.clear();
    m_indexToPanelMap.clear();

    delete ui;
}

bool CorneaWidget::loadConfig(const QString &configPath)
{
    if (!m_config.load(configPath)) {
        appendLog(QString("Failed to load config: %1").arg(configPath));
        return false;
    }

    appendLog(QString("Config loaded: %1").arg(configPath));

    // Initialize Python bridge with config
    if (!initializePythonBridge()) {
        return false;
    }

    // Refresh device list
    refreshDeviceList();

    // Pre-create FTDI connections for all detected devices at startup.
    // Uses init_rj1=False so no panel/光機 is needed — only driver board.
    // This ensures pyftdi SPI/I2C controllers are created once for all devices.
    // Subsequent powerOn uses system_power_on() on existing instances.
    for (DeviceControlPanel *panel : m_devicePanels) {
        CorneaController *ctrl = panel->controller();
        if (ctrl && !ctrl->isConnected()) {
            QString variant = panel->currentVariant();
            appendLog(QString("Pre-init: %1 (variant=%2, FTDI only)...")
                      .arg(panel->deviceSerial(), variant));
            if (ctrl->preInit(panel->deviceIndex(), variant)) {
                appendLog(QString("Pre-init: %1 OK").arg(panel->deviceSerial()));
            } else {
                appendLog(QString("Pre-init: %1 FAILED (will retry on powerOn)")
                          .arg(panel->deviceSerial()));
            }
        }
    }

    // Save config to persist any newly discovered devices
    saveConfig();

    // Load test chart images
    loadTestChartImages();

    // Start TCP server if enabled in config
    const TcpConfig &tcpConfig = m_config.tcp();
    if (tcpConfig.enabled) {
        if (startTcpServer(tcpConfig.port)) {
            appendLog(QString("TCP server started on port %1").arg(tcpConfig.port));
        } else {
            appendLog(QString("Failed to start TCP server on port %1").arg(tcpConfig.port));
        }
    }

    return true;
}

bool CorneaWidget::saveConfig()
{
    if (m_config.save()) {
        appendLog("Config saved");
        return true;
    }
    appendLog("Failed to save config");
    return false;
}

bool CorneaWidget::initializePythonBridge()
{
    if (m_initialized) {
        return true;
    }

    appendLog("Application started");
    appendLog("Initializing Python bridge...");

    const PythonConfig &py = m_config.python();
    if (py.venvPath.isEmpty()) {
        appendLog("Error: venv_path not configured");
        return false;
    }

    // Initialize with config values
    m_pythonBridge->setSpiClkFreq(py.spiClkFreq);
    appendLog(QString("SPI clock freq: %1 MHz (from config)").arg(py.spiClkFreq / 1e6, 0, 'f', 1));
    if (!m_pythonBridge->initialize(py.venvPath, py.pythonHome, py.dllPaths, py.calPath, py.allowDefaultHdf5)) {
        appendLog("Warning: Failed to initialize Python bridge. Check config paths.");
        QMessageBox::warning(this, "Initialization Warning",
            "Failed to initialize Python bridge.\n"
            "Please check the config file paths.\n\n"
            "Ensure venv_path, python_home, and dll_paths are correct.");
        return false;
    }

    appendLog("Python bridge initialized successfully");

    // Start polling Python output every 100ms
    m_pythonOutputTimer->start(100);
    m_initialized = true;

    return true;
}

// === Public API ===

bool CorneaWidget::powerOn(int index)
{
    // If only one device connected, use it directly (ignore index)
    if (m_devicePanels.size() == 1) {
        appendLog("powerOn: using the only connected device");
        return m_devicePanels[0]->powerOnDirect();
    }

    // Multiple devices: use config index to find by serial
    if (index < 0 || index >= MAX_DEVICES) {
        appendLog(QString("powerOn: invalid index %1").arg(index));
        return false;
    }

    DeviceConfig devConfig = m_config.getDevice(index);
    if (!devConfig.isValid()) {
        appendLog(QString("powerOn: index %1 not configured").arg(index));
        return false;
    }

    // Find the panel with this serial
    DeviceControlPanel *panel = getPanelBySerial(devConfig.serial);
    if (!panel) {
        appendLog(QString("powerOn: device %1 not found").arg(devConfig.serial));
        return false;
    }

    // Power on via panel
    return panel->powerOnDirect();
}

bool CorneaWidget::powerOff(int index)
{
    // If only one device connected, use it directly (ignore index)
    if (m_devicePanels.size() == 1) {
        appendLog("powerOff: using the only connected device");
        return m_devicePanels[0]->powerOffDirect();
    }

    // Multiple devices: use config index to find by serial
    if (index < 0 || index >= MAX_DEVICES) {
        appendLog(QString("powerOff: invalid index %1").arg(index));
        return false;
    }

    DeviceConfig devConfig = m_config.getDevice(index);
    if (!devConfig.isValid()) {
        appendLog(QString("powerOff: index %1 not configured").arg(index));
        return false;
    }

    DeviceControlPanel *panel = getPanelBySerial(devConfig.serial);
    if (!panel) {
        appendLog(QString("powerOff: device %1 not found").arg(devConfig.serial));
        return false;
    }

    // Power off via panel
    return panel->powerOffDirect();
}

bool CorneaWidget::sendImage(int index, const QString &imagePath)
{
    QImage image(imagePath);
    if (image.isNull()) {
        appendLog(QString("sendImage: failed to load image %1").arg(imagePath));
        return false;
    }
    return sendImage(index, image);
}

bool CorneaWidget::sendImage(int index, const QImage &image)
{
    DeviceControlPanel *panel = nullptr;

    appendLog(QString("sendImage: called with index=%1, image=%2x%3, panels=%4")
              .arg(index).arg(image.width()).arg(image.height()).arg(m_devicePanels.size()));

    // If only one device connected, use it directly (ignore index)
    if (m_devicePanels.size() == 1) {
        panel = m_devicePanels[0];
    } else {
        // Multiple devices: use config index to find by serial
        if (index < 0 || index >= MAX_DEVICES) {
            appendLog(QString("sendImage: invalid index %1").arg(index));
            return false;
        }

        DeviceConfig devConfig = m_config.getDevice(index);
        if (!devConfig.isValid()) {
            appendLog(QString("sendImage: index %1 not configured").arg(index));
            return false;
        }

        panel = getPanelBySerial(devConfig.serial);
        if (!panel) {
            appendLog(QString("sendImage: device %1 not found").arg(devConfig.serial));
            return false;
        }
    }

    if (!panel->isConnected()) {
        appendLog(QString("sendImage: device not connected (panel=%1)")
                  .arg(panel->panelLabel()));
        return false;
    }

    return panel->sendImageDirect(image);
}

bool CorneaWidget::setBrightness(int index, double level)
{
    DeviceControlPanel *panel = nullptr;

    appendLog(QString("setBrightness: called with index=%1, level=%2, panels=%3")
              .arg(index).arg(level).arg(m_devicePanels.size()));

    // If only one device connected, use it directly (ignore index)
    if (m_devicePanels.size() == 1) {
        panel = m_devicePanels[0];
    } else {
        // Multiple devices: use config index to find by serial
        if (index < 0 || index >= MAX_DEVICES) {
            appendLog(QString("setBrightness: invalid index %1").arg(index));
            return false;
        }

        DeviceConfig devConfig = m_config.getDevice(index);
        if (!devConfig.isValid()) {
            appendLog(QString("setBrightness: index %1 not configured").arg(index));
            return false;
        }

        panel = getPanelBySerial(devConfig.serial);
        if (!panel) {
            appendLog(QString("setBrightness: device %1 not found").arg(devConfig.serial));
            return false;
        }
    }

    if (!panel->isConnected()) {
        appendLog(QString("setBrightness: device not connected (panel=%1)")
                  .arg(panel->panelLabel()));
        return false;
    }

    return panel->setBrightnessDirect(level);
}

bool CorneaWidget::isConnected(int index) const
{
    if (index < 0 || index >= MAX_DEVICES) {
        return false;
    }

    DeviceConfig devConfig = m_config.getDevice(index);
    if (!devConfig.isValid()) {
        return false;
    }

    DeviceControlPanel *panel = getPanelBySerial(devConfig.serial);
    return panel && panel->isConnected();
}

bool CorneaWidget::isConfigured(int index) const
{
    if (index < 0 || index >= MAX_DEVICES) {
        return false;
    }
    return m_config.getDevice(index).isValid();
}

QString CorneaWidget::getSerial(int index) const
{
    if (index < 0 || index >= MAX_DEVICES) {
        return QString();
    }
    return m_config.getDevice(index).serial;
}

int CorneaWidget::getConnectedCount() const
{
    int count = 0;
    for (auto *panel : m_devicePanels) {
        if (panel->isConnected()) {
            count++;
        }
    }
    return count;
}

QString CorneaWidget::getConnectedSerial() const
{
    // Return the first/only connected device's serial
    if (m_devicePanels.size() == 1) {
        return m_devicePanels[0]->deviceSerial();
    }

    // Multiple devices: return first connected one
    for (auto *panel : m_devicePanels) {
        if (panel->isConnected()) {
            return panel->deviceSerial();
        }
    }
    return QString();
}

QString CorneaWidget::getPanelId(int index) const
{
    DeviceControlPanel *panel = nullptr;

    // If only one device, use it directly
    if (m_devicePanels.size() == 1) {
        panel = m_devicePanels[0];
    } else {
        // Multiple devices: use config index to find by serial
        if (index < 0 || index >= MAX_DEVICES) {
            return QString();
        }
        DeviceConfig devConfig = m_config.getDevice(index);
        if (!devConfig.isValid()) {
            return QString();
        }
        panel = getPanelBySerial(devConfig.serial);
    }

    if (!panel || !panel->isConnected()) {
        return QString();
    }
    return panel->getPanelId();
}

double CorneaWidget::getRj1Temperature(int index) const
{
    DeviceControlPanel *panel = nullptr;

    // If only one device, use it directly
    if (m_devicePanels.size() == 1) {
        panel = m_devicePanels[0];
    } else {
        // Multiple devices: use config index to find by serial
        if (index < 0 || index >= MAX_DEVICES) {
            return -999.0;
        }
        DeviceConfig devConfig = m_config.getDevice(index);
        if (!devConfig.isValid()) {
            return -999.0;
        }
        panel = getPanelBySerial(devConfig.serial);
    }

    if (!panel || !panel->isConnected()) {
        return -999.0;
    }
    return panel->getRj1Temperature();
}

// === Public API (by serial) ===

// ---------------------------------------------------------------------------
// Thread-safe helper: get controller directly, bypassing panel UI.
// All *BySerial() methods below use this for TCP async safety.
// ---------------------------------------------------------------------------
CorneaController* CorneaWidget::getControllerBySerial(const QString &serial) const
{
    DeviceControlPanel *panel = getPanelBySerial(serial);
    return panel ? panel->controller() : nullptr;
}

bool CorneaWidget::powerOnBySerial(const QString &serial)
{
    DeviceControlPanel *panel = getPanelBySerial(serial);
    if (!panel) return false;

    CorneaController *ctrl = panel->controller();

    // Already connected and powered on — nothing to do
    if (ctrl->isConnected() && ctrl->isPoweredOn()) return true;

    // Connected but powered off — re-power without creating new instance
    // This avoids pyftdi resetting all SPI/I2C controllers on the USB hub
    if (ctrl->isConnected() && !ctrl->isPoweredOn()) {
        appendLog(QString("[PowerOn] %1 — re-powering existing instance").arg(serial));
        return ctrl->powerOn();
    }

    // Not connected — first time, full connect (creates Python SDK instance)
    return ctrl->connect(panel->deviceIndex(), panel->currentVariant());
}

bool CorneaWidget::powerOnBySerial(const QString &serial, const QString &variant)
{
    DeviceControlPanel *panel = getPanelBySerial(serial);
    if (!panel) return false;

    // Thread-safe: update cached variant, schedule UI update on main thread
    panel->setCurrentVariant(variant);
    QMetaObject::invokeMethod(panel, [panel, variant]() {
        panel->setVariant(variant);
    }, Qt::QueuedConnection);

    CorneaController *ctrl = panel->controller();

    if (ctrl->isConnected() && ctrl->isPoweredOn()) return true;

    if (ctrl->isConnected() && !ctrl->isPoweredOn()) {
        appendLog(QString("[PowerOn] %1 — re-powering existing instance").arg(serial));
        return ctrl->powerOn();
    }

    return ctrl->connect(panel->deviceIndex(), variant);
}

bool CorneaWidget::powerOffBySerial(const QString &serial)
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) return true;

    // Software power-off only — keep the Python SDK instance alive.
    // This avoids pyftdi resetting all SPI/I2C controllers when the
    // next powerOn creates a new instance.
    return ctrl->powerOff();
}

bool CorneaWidget::sendImageBySerial(const QString &serial, const QString &imagePath)
{
    QImage image(imagePath);
    if (image.isNull()) return false;
    return sendImageBySerial(serial, image);
}

bool CorneaWidget::sendImageBySerial(const QString &serial, const QImage &image)
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) return false;
    return ctrl->sendImage(image);
}

bool CorneaWidget::setBrightnessBySerial(const QString &serial, double level)
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) return false;
    return ctrl->setBrightness(level);
}

bool CorneaWidget::setFlipBySerial(const QString &serial, bool xFlip, bool yFlip)
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) return false;
    return ctrl->setXFlip(xFlip) && ctrl->setYFlip(yFlip);
}

ApiResult CorneaWidget::sendImageBySerialEx(const QString &serial, const QString &imagePath)
{
    QImage image(imagePath);
    if (image.isNull()) {
        return ApiResult::fail(QString("Failed to load image: %1").arg(imagePath));
    }
    return sendImageBySerialEx(serial, image);
}

ApiResult CorneaWidget::sendImageBySerialEx(const QString &serial, const QImage &image)
{
    DeviceControlPanel *panel = getPanelBySerial(serial);
    if (!panel) return ApiResult::fail(QString("Device not found: %1").arg(serial));

    CorneaController *ctrl = panel->controller();
    if (!ctrl->isConnected()) return ApiResult::fail(QString("Device not connected: %1").arg(serial));

    // APL check (thread-safe: static method + cached brightness)
    double patternApl = DeviceControlPanel::calculatePatternApl(image);
    double brightness = panel->currentBrightness();
    double totalApl = patternApl * brightness;

    // 0.06 boundary tolerance — see DeviceControlPanel sendImageDirect for rationale.
    const double APL_LIMIT = 0.06;
    const double APL_LIMIT_EPSILON = 1e-4;
    if (totalApl > APL_LIMIT + APL_LIMIT_EPSILON) {
        return ApiResult::fail(QString("APL_EXCEEDED: Total APL %1 > limit %2 (pattern=%3, brightness=%4)")
                                   .arg(totalApl, 0, 'f', 4).arg(APL_LIMIT, 0, 'f', 2)
                                   .arg(patternApl, 0, 'f', 4).arg(brightness, 0, 'f', 2));
    }

    if (ctrl->sendImage(image)) {
        return ApiResult::ok();
    }
    return ApiResult::fail("Failed to send image to device");
}

ApiResult CorneaWidget::setBrightnessBySerialEx(const QString &serial, double level, bool waitProtection)
{
    DeviceControlPanel *panel = getPanelBySerial(serial);
    if (!panel) return ApiResult::fail(QString("Device not found: %1").arg(serial));

    CorneaController *ctrl = panel->controller();
    if (!ctrl->isConnected()) return ApiResult::fail(QString("Device not connected: %1").arg(serial));

    if (!ctrl->setBrightness(level)) {
        return ApiResult::fail("Failed to set brightness");
    }

    if (waitProtection) {
        for (int i = 0; i < 5; ++i) {
            QThread::msleep(1000);
            if (!ctrl->isConnected()) return ApiResult::fail("Device disconnected during protection check");

            double rj1 = ctrl->getRj1Temperature();
            double da9272 = ctrl->getDa9272Temperature();
            double maxTemp = qMax(rj1, da9272);

            if (maxTemp > DeviceControlPanel::TEMPERATURE_LIMIT) {
                ctrl->powerOff();
                return ApiResult::fail(QString("OVERHEAT: Temp %1°C > %2°C limit")
                    .arg(maxTemp, 0, 'f', 1).arg(DeviceControlPanel::TEMPERATURE_LIMIT, 0, 'f', 1));
            }
        }
    }
    return ApiResult::ok();
}

bool CorneaWidget::isConnectedBySerial(const QString &serial) const
{
    CorneaController *ctrl = getControllerBySerial(serial);
    return ctrl && ctrl->isConnected();
}

QString CorneaWidget::getPanelIdBySerial(const QString &serial) const
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) return QString();
    return ctrl->getPanelId();
}

double CorneaWidget::getTemperatureBySerial(const QString &serial) const
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) return -999.0;
    return ctrl->getRj1Temperature();
}

QVariantMap CorneaWidget::getPowerBySerial(const QString &serial) const
{
    CorneaController *ctrl = getControllerBySerial(serial);
    if (!ctrl || !ctrl->isConnected()) {
        QVariantMap v;
        v["vsys_power_mw"] = -999.0;
        v["vddio_power_mw"] = -999.0;
        return v;
    }
    return ctrl->getPowerMeasurements();
}

QString CorneaWidget::getVariantBySerial(const QString &serial) const
{
    DeviceControlPanel *panel = getPanelBySerial(serial);
    if (!panel) return QString();
    return panel->currentVariant();
}

QStringList CorneaWidget::getImageNames() const
{
    return m_imageLoader ? m_imageLoader->getImageNames() : QStringList();
}

QImage CorneaWidget::getImageByName(const QString &name) const
{
    return m_imageLoader ? m_imageLoader->getImage(name) : QImage();
}

QStringList CorneaWidget::getDeviceSerials() const
{
    QStringList serials;
    for (auto *panel : m_devicePanels) {
        serials.append(panel->deviceSerial());
    }
    return serials;
}

void CorneaWidget::refreshDevices()
{
    onRefreshDevicesClicked();
}

// === TCP Server ===

bool CorneaWidget::startTcpServer(quint16 port)
{
    if (!m_tcpServer) {
        m_tcpServer = std::make_unique<TcpServer>(this, this);
        connect(m_tcpServer.get(), &TcpServer::logMessage,
                this, QOverload<const QString &>::of(&CorneaWidget::appendLog));
    }
    return m_tcpServer->start(port);
}

void CorneaWidget::stopTcpServer()
{
    if (m_tcpServer) {
        m_tcpServer->stop();
    }
}

bool CorneaWidget::isTcpServerRunning() const
{
    return m_tcpServer && m_tcpServer->isRunning();
}

// === Private helpers ===

DeviceControlPanel* CorneaWidget::getPanelByIndex(int index) const
{
    if (index >= 0 && index < m_devicePanels.size()) {
        return m_devicePanels[index];
    }
    return nullptr;
}

DeviceControlPanel* CorneaWidget::getPanelBySerial(const QString &serial) const
{
    for (auto *panel : m_devicePanels) {
        if (panel->deviceSerial() == serial) {
            return panel;
        }
    }
    return nullptr;
}

int CorneaWidget::findPanelIndexBySerial(const QString &serial) const
{
    for (int i = 0; i < m_devicePanels.size(); i++) {
        if (m_devicePanels[i]->deviceSerial() == serial) {
            return i;
        }
    }
    return -1;
}

void CorneaWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateImagePreview();
}

void CorneaWidget::setupConnections()
{
    // Refresh devices button
    connect(ui->btnRefreshDevices, &QPushButton::clicked, this, &CorneaWidget::onRefreshDevicesClicked);

    // Image list selection
    connect(ui->listImages, &QListWidget::currentRowChanged,
            this, &CorneaWidget::onImageListSelectionChanged);

    // Image loader signals
    connect(m_imageLoader.get(), &ImageLoader::imageLoaded,
            this, &CorneaWidget::onImageLoaded);
    connect(m_imageLoader.get(), &ImageLoader::imageSelected,
            this, &CorneaWidget::onImageSelected);

    // Python output timer
    connect(m_pythonOutputTimer, &QTimer::timeout,
            this, &CorneaWidget::onPythonOutputPollTimeout);

    // Bridge signals
    connect(m_pythonBridge.get(), &PythonBridge::logMessage,
            this, &CorneaWidget::onBridgeLogMessage);
    connect(m_pythonBridge.get(), &PythonBridge::errorOccurred,
            this, &CorneaWidget::onBridgeError);
}

void CorneaWidget::onRefreshDevicesClicked()
{
    appendLog("Refreshing device list...");
    refreshDeviceList();
}

void CorneaWidget::refreshDeviceList()
{
    if (!m_initialized) {
        return;
    }

    QList<DeviceInfo> devices = m_pythonBridge->getAvailableDevicesInfo();

    appendLog(QString("Found %1 device(s)").arg(devices.size()));
    for (const DeviceInfo &info : devices) {
        appendLog(QString("  - %1").arg(info.displayName));
    }

    // Update device count label
    ui->lblDeviceCount->setText(QString("Devices: %1").arg(devices.size()));

    // Build a map of active (powered on) panels by serial — these must survive refresh
    QMap<QString, DeviceControlPanel*> activePanels;
    for (DeviceControlPanel *panel : m_devicePanels) {
        if (panel && panel->isConnected() && panel->isPoweredOn()) {
            QString serial = panel->controller()->getDeviceSerial();
            activePanels[serial] = panel;
            appendLog(QString("Keeping %1 alive during refresh (powered on)").arg(panel->panelLabel()));
        }
    }

    // Remove all non-DeviceControlPanel tabs (e.g. "No Devices" placeholder)
    for (int i = ui->tabDevices->count() - 1; i >= 0; --i) {
        QWidget *w = ui->tabDevices->widget(i);
        if (!qobject_cast<DeviceControlPanel*>(w)) {
            ui->tabDevices->removeTab(i);
            delete w;
        }
    }

    // Disconnect idle panels, remove tabs (but don't delete active ones)
    for (DeviceControlPanel *panel : m_devicePanels) {
        if (panel && !activePanels.values().contains(panel)) {
            if (panel->isConnected()) {
                panel->powerOffDirect();
                appendLog(QString("Disconnecting idle %1 before refresh...").arg(panel->panelLabel()));
            }
            int tabIdx = ui->tabDevices->indexOf(panel);
            if (tabIdx >= 0) ui->tabDevices->removeTab(tabIdx);
            delete panel;
        } else {
            // Remove active panel's tab (will be re-added in correct order below)
            int tabIdx = ui->tabDevices->indexOf(panel);
            if (tabIdx >= 0) ui->tabDevices->removeTab(tabIdx);
        }
    }
    m_devicePanels.clear();

    // Rebuild tabs: reuse active panels, create new ones for the rest
    for (int i = 0; i < devices.size(); ++i) {
        const DeviceInfo &info = devices[i];

        // Auto-add new device to config if not exists
        int configIndex = m_config.findDeviceBySerial(info.serial);
        if (configIndex < 0) {
            configIndex = m_config.addNewDevice(info.serial);
            if (configIndex >= 0) {
                appendLog(QString("Auto-added new device to config: %1 at index %2").arg(info.serial).arg(configIndex));
            }
        }

        DeviceConfig devConfig = m_config.getDevice(configIndex);

        DeviceControlPanel *panel = nullptr;
        if (activePanels.contains(info.serial)) {
            // Reuse existing active panel
            panel = activePanels.take(info.serial);
            panel->setDeviceInfo(info);  // Update info but keep connection alive
        } else {
            // Create new panel
            panel = new DeviceControlPanel(i, m_pythonBridge.get(), this);
            panel->setImageLoader(m_imageLoader.get());
            panel->setDeviceInfo(info);

            if (devConfig.isValid()) {
                panel->setVariant(devConfig.variant);
                panel->setInitialBrightness(devConfig.brightness);
            }

            connect(panel, &DeviceControlPanel::logMessage,
                    this, &CorneaWidget::onPanelLogMessage);
            connect(panel, &DeviceControlPanel::connectionChanged,
                    this, &CorneaWidget::onPanelConnectionChanged);
            connect(panel, &DeviceControlPanel::variantChanged,
                    this, &CorneaWidget::onPanelVariantChanged);
        }

        QString tabLabel = info.serial.isEmpty() ? QString("Device %1").arg(i) : info.serial;
        ui->tabDevices->addTab(panel, tabLabel);
        m_devicePanels.append(panel);
    }

    // Clean up any active panels whose serial no longer exists in device list
    for (DeviceControlPanel *orphan : activePanels.values()) {
        appendLog(QString("Active panel no longer detected, disconnecting: %1").arg(orphan->panelLabel()));
        orphan->powerOffDirect();
        delete orphan;
    }

    // If no devices found, show a placeholder tab
    if (devices.isEmpty()) {
        QLabel *placeholder = new QLabel("No devices detected.\nClick 'Refresh Devices' after connecting a device.");
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet("color: #666; font-size: 12pt;");
        ui->tabDevices->addTab(placeholder, "No Devices");
    }
}

void CorneaWidget::loadTestChartImages()
{
    // Only load images from TestChart folder
    // Check for TestChart folder in application directory
    QString appDir = QCoreApplication::applicationDirPath();
    QDir testChartDir(appDir + "/TestChart");

    if (testChartDir.exists()) {
        QStringList filters;
        for (const QString &format : ImageLoader::getSupportedFormats()) {
            filters.append(QString("*.%1").arg(format));
        }

        QStringList imageFiles = testChartDir.entryList(filters, QDir::Files, QDir::Name);

        if (!imageFiles.isEmpty()) {
            appendLog(QString("Loading %1 image(s) from TestChart folder...").arg(imageFiles.size()));

            QStringList fullPaths;
            for (const QString &file : imageFiles) {
                fullPaths.append(testChartDir.absoluteFilePath(file));
            }
            m_imageLoader->loadImages(fullPaths);
        }
    }

    // Select first image
    if (ui->listImages->count() > 0) {
        ui->listImages->setCurrentRow(0);
    }
}

void CorneaWidget::onImageListSelectionChanged()
{
    QListWidgetItem *item = ui->listImages->currentItem();
    if (item) {
        m_imageLoader->selectImage(item->text());
    }
}

void CorneaWidget::onImageLoaded(const QString &name)
{
    ui->listImages->addItem(name);
    appendLog(QString("Loaded image: %1").arg(name));
}

void CorneaWidget::onImageSelected(const QString &name)
{
    updateImagePreview();
}

void CorneaWidget::updateImagePreview()
{
    QSize labelSize = ui->lblImagePreview->size();
    int squareSize = qMin(labelSize.width(), labelSize.height());
    QSize previewSize(squareSize, squareSize);

    QPixmap pixmap = m_imageLoader->getCurrentPixmap(previewSize);
    if (pixmap.isNull()) {
        ui->lblImagePreview->setText("No Image");
        ui->lblImageInfo->setText("Select image from list");
    } else {
        ui->lblImagePreview->setPixmap(pixmap);
        QSize size = m_imageLoader->getCurrentImageSize();
        ui->lblImageInfo->setText(QString("%1 (%2 x %3)")
                                  .arg(m_imageLoader->getCurrentImageName())
                                  .arg(size.width())
                                  .arg(size.height()));
    }
}

void CorneaWidget::onPanelLogMessage(const QString &panelLabel, const QString &message)
{
    appendLog(panelLabel, message);
}

void CorneaWidget::onPanelConnectionChanged(int panelId, bool connected)
{
    // Get serial from panel
    if (panelId >= 0 && panelId < m_devicePanels.size()) {
        QString serial = m_devicePanels[panelId]->deviceSerial();

        // Find config index for this serial
        int configIndex = m_config.findDeviceBySerial(serial);

        if (connected) {
            emit deviceConnected(configIndex, serial);
        } else {
            emit deviceDisconnected(configIndex, serial);
        }
    }
}

void CorneaWidget::onPanelVariantChanged(const QString &serial, const QString &variant)
{
    int configIndex = m_config.findDeviceBySerial(serial);
    if (configIndex >= 0) {
        DeviceConfig devConfig = m_config.getDevice(configIndex);
        devConfig.variant = variant;
        m_config.setDevice(configIndex, devConfig);
        saveConfig();
        appendLog(QString("Variant changed for %1: %2 (saved)").arg(serial, variant));
    }
}

void CorneaWidget::onPythonOutputPollTimeout()
{
    m_pythonBridge->flushPythonOutput();
}

void CorneaWidget::onBridgeLogMessage(const QString &message)
{
    static QStringList noisePatterns = {
        "AllK required", "HotK Handles", "LstK Handles", "LstInfoK",
        "UsbK Handles", "DevK Handles", "OvlK Handles", "OvlPoolK",
        "StmK Handles", "IsochK Handles", "KLST_DEVINFO", "HandleSize",
        "PoolSize", "contiguous memory", "bytes each", "Dynamically allocated",
        "pmic_mux_sel_level", "got an unexpected keyword argument",
        "Detected panel version", "Using TMP108", "retina_temp_readout"
    };

    for (const QString &pattern : noisePatterns) {
        if (message.contains(pattern)) {
            return;
        }
    }

    ui->txtLog->append(message);
    emit logMessage(message);
    qDebug().noquote() << message;
}

void CorneaWidget::onBridgeError(const QString &error)
{
    appendLog(QString("ERROR: %1").arg(error));
}

void CorneaWidget::appendLog(const QString &message)
{
    QDateTime now = QDateTime::currentDateTime();
    QString timestamp = now.toString("yyyy-MM-dd hh:mm:ss");
    int msec = now.time().msec();
    QString logLine = QString("%1,%2 [INFO ] : %3").arg(timestamp).arg(msec, 3, 10, QChar('0')).arg(message);
    ui->txtLog->append(logLine);
    emit logMessage(logLine);
    qDebug().noquote() << logLine;
}

void CorneaWidget::appendLog(const QString &panelLabel, const QString &message)
{
    QDateTime now = QDateTime::currentDateTime();
    QString timestamp = now.toString("yyyy-MM-dd hh:mm:ss");
    int msec = now.time().msec();
    QString logLine = QString("%1,%2 [INFO ] : [%3] %4").arg(timestamp).arg(msec, 3, 10, QChar('0')).arg(panelLabel, message);
    ui->txtLog->append(logLine);
    emit logMessage(logLine);
    qDebug().noquote() << logLine;
}
