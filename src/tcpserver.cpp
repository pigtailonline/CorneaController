#include "tcpserver.h"
#include "corneawidget.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

TcpServer::TcpServer(CorneaWidget *corneaWidget, QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_corneaWidget(corneaWidget)
{
    connect(m_server, &QTcpServer::newConnection, this, &TcpServer::onNewConnection);
}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::start(quint16 port)
{
    if (m_server->isListening()) {
        return true;
    }

    if (m_server->listen(QHostAddress::Any, port)) {
        emit logMessage(QString("TCP server started on port %1").arg(port));
        return true;
    } else {
        emit logMessage(QString("TCP server failed to start: %1").arg(m_server->errorString()));
        return false;
    }
}

void TcpServer::stop()
{
    // Disconnect all clients
    for (QTcpSocket *client : m_clients) {
        client->disconnectFromHost();
    }
    m_clients.clear();
    m_buffers.clear();

    if (m_server->isListening()) {
        m_server->close();
        emit logMessage("TCP server stopped");
    }
}

bool TcpServer::isRunning() const
{
    return m_server->isListening();
}

quint16 TcpServer::port() const
{
    return m_server->serverPort();
}

void TcpServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *client = m_server->nextPendingConnection();
        m_clients.append(client);
        m_buffers[client] = QByteArray();

        connect(client, &QTcpSocket::disconnected, this, &TcpServer::onClientDisconnected);
        connect(client, &QTcpSocket::readyRead, this, &TcpServer::onReadyRead);

        QString address = QString("%1:%2").arg(client->peerAddress().toString()).arg(client->peerPort());
        emit clientConnected(address);
        emit logMessage(QString("Client connected: %1").arg(address));
    }
}

void TcpServer::onClientDisconnected()
{
    QTcpSocket *client = qobject_cast<QTcpSocket*>(sender());
    if (client) {
        QString address = QString("%1:%2").arg(client->peerAddress().toString()).arg(client->peerPort());
        m_clients.removeOne(client);
        m_buffers.remove(client);
        client->deleteLater();

        emit clientDisconnected(address);
        emit logMessage(QString("Client disconnected: %1").arg(address));
    }
}

void TcpServer::onReadyRead()
{
    QTcpSocket *client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    // Append to buffer
    m_buffers[client].append(client->readAll());

    // Process complete lines (commands end with \n)
    while (true) {
        int idx = m_buffers[client].indexOf('\n');
        if (idx < 0) break;

        QByteArray line = m_buffers[client].left(idx).trimmed();
        m_buffers[client].remove(0, idx + 1);

        if (line.isEmpty()) continue;

        emit commandReceived(QString::fromUtf8(line));

        // Parse JSON command
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

        QJsonObject response;
        if (parseError.error != QJsonParseError::NoError) {
            response = makeError(QString("JSON parse error: %1").arg(parseError.errorString()));
        } else if (!doc.isObject()) {
            response = makeError("Invalid command format: expected JSON object");
        } else {
            response = processCommand(doc.object());
        }

        sendResponse(client, response);
    }
}

QJsonObject TcpServer::processCommand(const QJsonObject &cmd)
{
    QString cmdName = cmd["cmd"].toString().toLower();

    if (cmdName == "poweron") {
        return handlePowerOn(cmd);
    } else if (cmdName == "poweroff") {
        return handlePowerOff(cmd);
    } else if (cmdName == "sendimage") {
        return handleSendImage(cmd);
    } else if (cmdName == "setbrightness") {
        return handleSetBrightness(cmd);
    } else if (cmdName == "setflip") {
        return handleSetFlip(cmd);
    } else if (cmdName == "isconnected") {
        return handleIsConnected(cmd);
    } else if (cmdName == "getpanelid") {
        return handleGetPanelId(cmd);
    } else if (cmdName == "gettemperature") {
        return handleGetTemperature(cmd);
    } else if (cmdName == "getstatus") {
        return handleGetStatus(cmd);
    } else if (cmdName == "listdevices") {
        return handleListDevices(cmd);
    } else if (cmdName == "refreshdevices") {
        return handleRefreshDevices(cmd);
    } else {
        return makeError(QString("Unknown command: %1").arg(cmdName));
    }
}

QJsonObject TcpServer::handlePowerOn(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    bool result = m_corneaWidget->powerOnBySerial(serial);
    if (result) {
        return makeSuccess();
    } else {
        return makeError(QString("Failed to power on device: %1").arg(serial));
    }
}

QJsonObject TcpServer::handlePowerOff(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    bool result = m_corneaWidget->powerOffBySerial(serial);
    if (result) {
        return makeSuccess();
    } else {
        return makeError(QString("Failed to power off device: %1").arg(serial));
    }
}

QJsonObject TcpServer::handleSendImage(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    QString path = params["path"].toString();

    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }
    if (path.isEmpty()) {
        return makeError("Missing 'path' parameter");
    }

    ApiResult result = m_corneaWidget->sendImageBySerialEx(serial, path);
    if (result.success) {
        return makeSuccess();
    } else {
        return makeError(result.error);
    }
}

QJsonObject TcpServer::handleSetBrightness(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    if (!params.contains("level")) {
        return makeError("Missing 'level' parameter");
    }
    double level = params["level"].toDouble();

    // Set brightness and start background protection monitoring
    // Returns success immediately, protection runs in background
    // If overheat detected, device will auto power off
    bool result = m_corneaWidget->setBrightnessBySerial(serial, level);
    if (result) {
        return makeSuccess();
    } else {
        return makeError(QString("Failed to set brightness for device: %1").arg(serial));
    }
}

QJsonObject TcpServer::handleSetFlip(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    bool xFlip = params["x"].toBool(false);
    bool yFlip = params["y"].toBool(false);

    bool result = m_corneaWidget->setFlipBySerial(serial, xFlip, yFlip);
    if (result) {
        return makeSuccess();
    } else {
        return makeError(QString("Failed to set flip for device: %1").arg(serial));
    }
}

QJsonObject TcpServer::handleIsConnected(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    bool connected = m_corneaWidget->isConnectedBySerial(serial);
    QJsonObject data;
    data["connected"] = connected;
    return makeSuccess(data);
}

QJsonObject TcpServer::handleGetPanelId(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    QString panelId = m_corneaWidget->getPanelIdBySerial(serial);
    QJsonObject data;
    data["panelId"] = panelId;
    return makeSuccess(data);
}

QJsonObject TcpServer::handleGetTemperature(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    if (serial.isEmpty()) {
        return makeError("Missing 'serial' parameter");
    }

    double temp = m_corneaWidget->getTemperatureBySerial(serial);
    if (temp < -900) {
        return makeError(QString("Failed to get temperature for device: %1").arg(serial));
    }

    QJsonObject data;
    data["temperature"] = temp;
    return makeSuccess(data);
}

QJsonObject TcpServer::handleGetStatus(const QJsonObject &params)
{
    Q_UNUSED(params);

    QJsonArray devices;
    QStringList serials = m_corneaWidget->getDeviceSerials();

    for (const QString &serial : serials) {
        QJsonObject dev;
        dev["serial"] = serial;
        dev["connected"] = m_corneaWidget->isConnectedBySerial(serial);
        dev["panelId"] = m_corneaWidget->getPanelIdBySerial(serial);
        dev["variant"] = m_corneaWidget->getVariantBySerial(serial);
        devices.append(dev);
    }

    QJsonObject data;
    data["deviceCount"] = devices.size();
    data["devices"] = devices;
    return makeSuccess(data);
}

QJsonObject TcpServer::handleListDevices(const QJsonObject &params)
{
    Q_UNUSED(params);

    QStringList serials = m_corneaWidget->getDeviceSerials();
    QJsonArray arr;
    for (const QString &s : serials) {
        arr.append(s);
    }

    QJsonObject data;
    data["devices"] = arr;
    return makeSuccess(data);
}

QJsonObject TcpServer::handleRefreshDevices(const QJsonObject &params)
{
    Q_UNUSED(params);

    m_corneaWidget->refreshDevices();
    int count = m_corneaWidget->getDeviceSerials().size();

    QJsonObject data;
    data["deviceCount"] = count;
    return makeSuccess(data);
}

QJsonObject TcpServer::makeSuccess(const QVariant &data)
{
    QJsonObject obj;
    obj["success"] = true;

    if (data.isValid()) {
        if (data.type() == QVariant::Map) {
            obj["data"] = QJsonObject::fromVariantMap(data.toMap());
        } else if (data.canConvert<QJsonObject>()) {
            obj["data"] = data.toJsonObject();
        }
    }

    return obj;
}

QJsonObject TcpServer::makeError(const QString &message)
{
    QJsonObject obj;
    obj["success"] = false;
    obj["error"] = message;
    return obj;
}

void TcpServer::sendResponse(QTcpSocket *client, const QJsonObject &response)
{
    QJsonDocument doc(response);
    QByteArray data = doc.toJson(QJsonDocument::Compact) + "\n";
    client->write(data);
    client->flush();

    emit responseSent(QString::fromUtf8(data.trimmed()));
}
