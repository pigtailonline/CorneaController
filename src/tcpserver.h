#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QMap>

class CorneaWidget;

class TcpServer : public QObject
{
    Q_OBJECT

public:
    explicit TcpServer(CorneaWidget *corneaWidget, QObject *parent = nullptr);
    ~TcpServer();

    bool start(quint16 port = 5566);
    void stop();
    bool isRunning() const;
    quint16 port() const;

signals:
    void clientConnected(const QString &address);
    void clientDisconnected(const QString &address);
    void commandReceived(const QString &cmd);
    void responseSent(const QString &response);
    void logMessage(const QString &message);

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onReadyRead();

private:
    QJsonObject processCommand(const QJsonObject &cmd);

    // Command handlers
    QJsonObject handlePowerOn(const QJsonObject &params);
    QJsonObject handlePowerOff(const QJsonObject &params);
    QJsonObject handleSendImage(const QJsonObject &params);
    QJsonObject handleSetBrightness(const QJsonObject &params);
    QJsonObject handleSetFlip(const QJsonObject &params);
    QJsonObject handleIsConnected(const QJsonObject &params);
    QJsonObject handleGetPanelId(const QJsonObject &params);
    QJsonObject handleGetTemperature(const QJsonObject &params);
    QJsonObject handleGetStatus(const QJsonObject &params);
    QJsonObject handleListDevices(const QJsonObject &params);
    QJsonObject handleRefreshDevices(const QJsonObject &params);

    // Helper methods
    QJsonObject makeSuccess(const QVariant &data = QVariant());
    QJsonObject makeError(const QString &message);
    void sendResponse(QTcpSocket *client, const QJsonObject &response);

    QTcpServer *m_server;
    QList<QTcpSocket*> m_clients;
    QMap<QTcpSocket*, QByteArray> m_buffers;  // Per-client receive buffer
    CorneaWidget *m_corneaWidget;
};

#endif // TCPSERVER_H
