#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "pythonbridge.h"
#include "devicecontrolpanel.h"
#include "imageloader.h"

#include <QMessageBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <QResizeEvent>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_pythonBridge(std::make_unique<PythonBridge>(this))
    , m_imageLoader(std::make_unique<ImageLoader>(this))
    , m_pythonOutputTimer(new QTimer(this))
{
    ui->setupUi(this);

    // Set venv path
    m_venvPath = "D:/projects/src/google driver/Sunny Release 2025.11.28/station_venv";

    appendLog("Application started");
    appendLog("Initializing Python bridge...");

    // Initialize shared Python bridge
    if (!m_pythonBridge->initialize(m_venvPath)) {
        appendLog("Warning: Failed to initialize Python bridge. Check venv path.");
        QMessageBox::warning(this, "Initialization Warning",
            "Failed to initialize Python bridge.\n"
            "Please ensure the virtual environment is set up correctly.\n\n"
            "Run setup_station.ps1 first.");
    } else {
        appendLog("Python bridge initialized successfully");

        // Start polling Python output every 100ms
        m_pythonOutputTimer->start(100);
    }

    setupConnections();

    // Initial device list refresh - this will create tabs dynamically
    refreshDeviceList();

    // Load images from TestChart folder if it exists
    loadTestChartImages();
}

MainWindow::~MainWindow()
{
    m_pythonOutputTimer->stop();
    delete ui;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // Update preview when window resizes
    updateImagePreview();
}

void MainWindow::setupConnections()
{
    // Refresh devices button
    connect(ui->btnRefreshDevices, &QPushButton::clicked, this, &MainWindow::onRefreshDevicesClicked);

    // Image list selection
    connect(ui->listImages, &QListWidget::currentRowChanged,
            this, &MainWindow::onImageListSelectionChanged);

    // Image loader signals
    connect(m_imageLoader.get(), &ImageLoader::imageLoaded,
            this, &MainWindow::onImageLoaded);
    connect(m_imageLoader.get(), &ImageLoader::imageSelected,
            this, &MainWindow::onImageSelected);

    // Python output timer
    connect(m_pythonOutputTimer, &QTimer::timeout,
            this, &MainWindow::onPythonOutputPollTimeout);

    // Bridge signals
    connect(m_pythonBridge.get(), &PythonBridge::logMessage,
            this, &MainWindow::onBridgeLogMessage);
    connect(m_pythonBridge.get(), &PythonBridge::errorOccurred,
            this, &MainWindow::onBridgeError);
}

void MainWindow::onRefreshDevicesClicked()
{
    appendLog("Refreshing device list...");
    refreshDeviceList();
}

void MainWindow::refreshDeviceList()
{
    QList<DeviceInfo> devices = m_pythonBridge->getAvailableDevicesInfo();

    appendLog(QString("Found %1 device(s)").arg(devices.size()));
    for (const DeviceInfo &info : devices) {
        appendLog(QString("  - %1").arg(info.displayName));
    }

    // Update device count label
    ui->lblDeviceCount->setText(QString("Devices: %1").arg(devices.size()));

    // Clear existing tabs and panels
    while (ui->tabDevices->count() > 0) {
        QWidget *widget = ui->tabDevices->widget(0);
        ui->tabDevices->removeTab(0);
        widget->deleteLater();
    }
    m_devicePanels.clear();

    // Create one tab per detected device
    for (int i = 0; i < devices.size(); ++i) {
        const DeviceInfo &info = devices[i];

        // Create panel for this device
        DeviceControlPanel *panel = new DeviceControlPanel(i, m_pythonBridge.get(), this);
        panel->setImageLoader(m_imageLoader.get());

        // Set the device info for this panel
        panel->setDeviceInfo(info);

        // Connect panel signals
        connect(panel, &DeviceControlPanel::logMessage,
                this, &MainWindow::onPanelLogMessage);
        connect(panel, &DeviceControlPanel::connectionChanged,
                this, &MainWindow::onPanelConnectionChanged);

        // Add tab with device display name
        QString tabLabel = QString("Device %1").arg(i);
        if (!info.serial.isEmpty()) {
            tabLabel = info.serial;
        }
        ui->tabDevices->addTab(panel, tabLabel);
        m_devicePanels.append(panel);
    }

    // If no devices found, show a placeholder tab
    if (devices.isEmpty()) {
        QLabel *placeholder = new QLabel("No devices detected.\nClick 'Refresh Devices' after connecting a device.");
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet("color: #666; font-size: 12pt;");
        ui->tabDevices->addTab(placeholder, "No Devices");
    }
}

void MainWindow::loadTestChartImages()
{
    // Load built-in test patterns first
    appendLog("Loading test patterns...");
    m_imageLoader->addImage(m_imageLoader->createSolidColor(720, 720, Qt::red), "Red");
    m_imageLoader->addImage(m_imageLoader->createSolidColor(720, 720, Qt::green), "Green");
    m_imageLoader->addImage(m_imageLoader->createSolidColor(720, 720, Qt::blue), "Blue");
    m_imageLoader->addImage(m_imageLoader->createSolidColor(720, 720, Qt::white), "White");
    m_imageLoader->addImage(m_imageLoader->createSolidColor(720, 720, Qt::black), "Black");
    m_imageLoader->addImage(m_imageLoader->createColorBars(720, 720), "Color Bars");
    m_imageLoader->addImage(m_imageLoader->createCheckerboard(720, 720, 60, Qt::white, Qt::black), "Checkerboard");

    // Check for TestChart folder in application directory
    QString appDir = QCoreApplication::applicationDirPath();
    QDir testChartDir(appDir + "/TestChart");

    if (testChartDir.exists()) {
        // Get supported image formats
        QStringList filters;
        for (const QString &format : ImageLoader::getSupportedFormats()) {
            filters.append(QString("*.%1").arg(format));
        }

        // Get all image files
        QStringList imageFiles = testChartDir.entryList(filters, QDir::Files, QDir::Name);

        if (!imageFiles.isEmpty()) {
            appendLog(QString("Loading %1 image(s) from TestChart folder...").arg(imageFiles.size()));

            // Load each image
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

void MainWindow::onImageListSelectionChanged()
{
    QListWidgetItem *item = ui->listImages->currentItem();
    if (item) {
        // Use item text (image name) instead of index to avoid QMap ordering issues
        m_imageLoader->selectImage(item->text());
    }
}

void MainWindow::onImageLoaded(const QString &name)
{
    ui->listImages->addItem(name);
    appendLog(QString("Loaded image: %1").arg(name));
}

void MainWindow::onImageSelected(const QString &name)
{
    updateImagePreview();
}

void MainWindow::updateImagePreview()
{
    // Use square size for preview (take minimum of width/height)
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

void MainWindow::onPanelLogMessage(const QString &panelLabel, const QString &message)
{
    appendLog(panelLabel, message);
}

void MainWindow::onPanelConnectionChanged(int panelId, bool connected)
{
    Q_UNUSED(panelId)
    Q_UNUSED(connected)
    // Could update UI indicators here if needed
}

void MainWindow::onPythonOutputPollTimeout()
{
    m_pythonBridge->flushPythonOutput();
}

void MainWindow::onBridgeLogMessage(const QString &message)
{
    // Filter out libusbK and other C library noise
    static QStringList noisePatterns = {
        "AllK required", "HotK Handles", "LstK Handles", "LstInfoK",
        "UsbK Handles", "DevK Handles", "OvlK Handles", "OvlPoolK",
        "StmK Handles", "IsochK Handles", "KLST_DEVINFO", "HandleSize",
        "PoolSize", "contiguous memory", "bytes each", "Dynamically allocated",
        "pmic_mux_sel_level", "got an unexpected keyword argument"
    };

    for (const QString &pattern : noisePatterns) {
        if (message.contains(pattern)) {
            return;  // Skip this message
        }
    }

    // Python logs already contain timestamp, append directly without adding another
    ui->txtLog->append(message);
    // Also output to Qt Creator console
    qDebug().noquote() << message;
}

void MainWindow::onBridgeError(const QString &error)
{
    appendLog(QString("ERROR: %1").arg(error));
}

void MainWindow::appendLog(const QString &message)
{
    // Match CLI log format: YYYY-MM-DD HH:mm:ss,mmm [LEVEL] : message
    QDateTime now = QDateTime::currentDateTime();
    QString timestamp = now.toString("yyyy-MM-dd hh:mm:ss");
    int msec = now.time().msec();
    QString logLine = QString("%1,%2 [INFO ] : %3").arg(timestamp).arg(msec, 3, 10, QChar('0')).arg(message);
    ui->txtLog->append(logLine);
    // Also output to Qt Creator console
    qDebug().noquote() << logLine;
}

void MainWindow::appendLog(const QString &panelLabel, const QString &message)
{
    // Match CLI log format: YYYY-MM-DD HH:mm:ss,mmm [LEVEL] : message
    QDateTime now = QDateTime::currentDateTime();
    QString timestamp = now.toString("yyyy-MM-dd hh:mm:ss");
    int msec = now.time().msec();
    QString logLine = QString("%1,%2 [INFO ] : [%3] %4").arg(timestamp).arg(msec, 3, 10, QChar('0')).arg(panelLabel, message);
    ui->txtLog->append(logLine);
    // Also output to Qt Creator console
    qDebug().noquote() << logLine;
}
