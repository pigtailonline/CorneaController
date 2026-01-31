#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QList>
#include <memory>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class PythonBridge;
class DeviceControlPanel;
class ImageLoader;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

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

    Ui::MainWindow *ui;

    // Shared components
    std::unique_ptr<PythonBridge> m_pythonBridge;
    std::unique_ptr<ImageLoader> m_imageLoader;
    QTimer *m_pythonOutputTimer;

    // Device panels (dynamically created)
    QList<DeviceControlPanel*> m_devicePanels;

    QString m_venvPath;
};

#endif // MAINWINDOW_H
