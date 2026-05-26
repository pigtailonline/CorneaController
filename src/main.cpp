#include "corneawidget.h"

#include <QApplication>
#include <QMainWindow>
#include <QStyleFactory>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>

// ---------------------------------------------------------------------------
// File logger — writes daily log files to <app_dir>/log/YYYY-MM-DD.log
// All qDebug/qWarning/qCritical output is captured via qInstallMessageHandler.
// Since CorneaWidget::appendLog() calls qDebug(), this also captures Python
// output and all application-level logs.
// ---------------------------------------------------------------------------

static QFile    g_logFile;
static QMutex   g_logMutex;
static QString  g_logDate;
static QString  g_logDir;

static void openLogFileForDate(const QString &date)
{
    if (g_logFile.isOpen()) {
        g_logFile.flush();
        g_logFile.close();
    }
    g_logDate = date;
    g_logFile.setFileName(g_logDir + "/" + date + ".log");
    g_logFile.open(QIODevice::Append | QIODevice::Text);
}

static void fileMessageHandler(QtMsgType type,
                               const QMessageLogContext &context,
                               const QString &msg)
{
    Q_UNUSED(context);
    if (msg.isEmpty()) return;

    QMutexLocker locker(&g_logMutex);

    // Date rollover check
    QString today = QDate::currentDate().toString("yyyy-MM-dd");
    if (today != g_logDate) {
        openLogFileForDate(today);
    }

    // Build the line to write
    // Messages from appendLog already have timestamps (start with "20xx-")
    // Raw Qt debug/warning messages need a timestamp prefix
    QString line;
    bool hasTimestamp = (msg.length() > 4 && msg[4] == QChar('-')
                         && msg[0].isDigit() && msg[1].isDigit());
    if (hasTimestamp) {
        line = msg;
    } else {
        QDateTime now = QDateTime::currentDateTime();
        QString ts = now.toString("yyyy-MM-dd hh:mm:ss");
        int msec = now.time().msec();

        const char *level = "DEBUG";
        switch (type) {
        case QtInfoMsg:     level = "INFO "; break;
        case QtWarningMsg:  level = "WARN "; break;
        case QtCriticalMsg: level = "ERROR"; break;
        case QtFatalMsg:    level = "FATAL"; break;
        default: break;
        }
        line = QString("%1,%2 [%3] : %4")
                   .arg(ts)
                   .arg(msec, 3, 10, QChar('0'))
                   .arg(QLatin1String(level))
                   .arg(msg);
    }

    // Write to file and flush immediately (crash-safe)
    if (g_logFile.isOpen()) {
        QTextStream stream(&g_logFile);
        stream << line << "\n";
        stream.flush();
    }

    // Also output to stderr so Qt Creator console still works
    fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setApplicationName("Cornea Controller");
    app.setApplicationVersion("1.1.20");
    app.setOrganizationName("Google AR Display Lab");

    // Setup file logging: <app_dir>/log/
    g_logDir = QCoreApplication::applicationDirPath() + "/log";
    QDir().mkpath(g_logDir);
    openLogFileForDate(QDate::currentDate().toString("yyyy-MM-dd"));
    qInstallMessageHandler(fileMessageHandler);

    // Use Fusion style for consistent look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Create main window to host CorneaWidget
    QMainWindow mainWindow;
    QString title = QString("%1 v%2")
        .arg(app.applicationName(), app.applicationVersion());
    mainWindow.setWindowTitle(title);
    mainWindow.resize(1000, 900);

    // Create CorneaWidget as central widget
    CorneaWidget *corneaWidget = new CorneaWidget(&mainWindow);
    mainWindow.setCentralWidget(corneaWidget);

    // Try to load config from application directory
    QString appDir = QCoreApplication::applicationDirPath();
    QString configPath = appDir + "/cornea_config.json";

    if (QFileInfo::exists(configPath)) {
        corneaWidget->loadConfig(configPath);
    } else {
        // Try default config path
        configPath = "cornea_config.json";
        if (QFileInfo::exists(configPath)) {
            corneaWidget->loadConfig(configPath);
        }
    }

    mainWindow.show();

    // Force a clean shutdown when the user closes the window or hits Ctrl-Q.
    // Without this, the embedded Python interpreter's per-panel temperature
    // poll threads keep the process alive (non-daemon) and pyftdi holds the
    // FT4232 USB interface claim — the next CC launch then fails with
    // libusb's "could not claim interface". The aboutToQuit signal fires
    // BEFORE the QApplication event loop returns, giving us a controlled
    // chance to call Py_Finalize on the right thread.
    QObject::connect(&app, &QApplication::aboutToQuit, corneaWidget, [corneaWidget]() {
        corneaWidget->shutdown();
    });

    return app.exec();
}
