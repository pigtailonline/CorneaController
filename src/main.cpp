#include "corneawidget.h"

#include <QApplication>
#include <QMainWindow>
#include <QStyleFactory>
#include <QFileInfo>
#include <QDir>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setApplicationName("Cornea Controller");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Google AR Display Lab");

    // Use Fusion style for consistent look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Create main window to host CorneaWidget
    QMainWindow mainWindow;
    mainWindow.setWindowTitle("Cornea Controller");
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

    return app.exec();
}
