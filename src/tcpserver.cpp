#include "tcpserver.h"
#include "corneawidget.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <QtConcurrent>
#include <QThreadPool>

TcpServer::TcpServer(CorneaWidget *corneaWidget, QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_corneaWidget(corneaWidget)
    , m_asyncGuard(std::make_shared<std::atomic<bool>>(true))
{
    connect(m_server, &QTcpServer::newConnection, this, &TcpServer::onNewConnection);
}

TcpServer::~TcpServer()
{
    m_asyncGuard->store(false);
    stop();
    // Wait for in-flight QtConcurrent tasks that captured 'this' to complete
    // before destroying. TcpServer is destroyed before PythonBridge (reverse
    // member declaration order), so dispatched Python calls still work here.
    QThreadPool::globalInstance()->waitForDone();
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
    // Disconnect all clients — disconnect signals first to prevent stale events
    for (QTcpSocket *client : m_clients) {
        client->disconnect(this);
        client->disconnectFromHost();
        client->deleteLater();
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

        // Disconnect all signals FIRST to prevent stale readyRead delivery
        client->disconnect(this);

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
    while (m_clients.contains(client)) {
        int idx = m_buffers[client].indexOf('\n');
        if (idx < 0) break;

        QByteArray line = m_buffers[client].left(idx).trimmed();
        m_buffers[client].remove(0, idx + 1);

        if (line.isEmpty()) continue;

        emit commandReceived(QString::fromUtf8(line));

        // Parse JSON command
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            sendResponse(client, makeError(QString("JSON parse error: %1").arg(parseError.errorString())));
            continue;
        }
        if (!doc.isObject()) {
            sendResponse(client, makeError("Invalid command format: expected JSON object"));
            continue;
        }

        QJsonObject cmd = doc.object();
        QString cmdName = cmd["cmd"].toString().toLower();

        // Log received command (skip high-frequency getTemperature)
        if (cmdName != "gettemperature") {
            QString cmdStr = QJsonDocument(cmd).toJson(QJsonDocument::Compact);
            qInfo() << "[TCP] RX <<" << cmdStr;
        }

        // Echo back cmd and serial in every response so clients can match
        // responses to requests without relying on FIFO ordering
        QString origCmd = cmd["cmd"].toString();
        QString origSerial = cmd["serial"].toString();

        if (isAsyncCommand(cmdName)) {
            // Heavy PythonBridge commands: run in thread pool, respond when done
            auto guard = m_asyncGuard;
            QPointer<QTcpSocket> clientPtr(client);

            QtConcurrent::run([this, guard, clientPtr, cmd, origCmd, origSerial]() {
                if (!guard->load()) return;
                QJsonObject response;
                try {
                    response = processCommand(cmd);
                } catch (const std::exception &e) {
                    response = makeError(QString("Exception: %1").arg(e.what()));
                } catch (...) {
                    response = makeError("Unknown exception in async command");
                }
                // Always echo cmd+serial so client can match response
                response["cmd"] = origCmd;
                if (!origSerial.isEmpty()) response["serial"] = origSerial;

                QMetaObject::invokeMethod(this, [this, guard, clientPtr, response]() {
                    if (!guard->load()) return;
                    if (clientPtr && m_clients.contains(clientPtr.data())) {
                        sendResponse(clientPtr.data(), response);
                    }
                }, Qt::QueuedConnection);
            });
        } else {
            // Light commands: process on main thread
            QJsonObject response = processCommand(cmd);
            response["cmd"] = origCmd;
            if (!origSerial.isEmpty()) response["serial"] = origSerial;
            sendResponse(client, response);
        }
    }
}

bool TcpServer::isAsyncCommand(const QString &cmdName)
{
    static const QSet<QString> asyncCommands = {
        "poweron", "poweroff",
        "sendimage", "sendimagebyname",
        "gettemperature", "getpanelid",
        "setbrightness"
    };
    return asyncCommands.contains(cmdName);
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
    } else if (cmdName == "listimages") {
        return handleListImages(cmd);
    } else if (cmdName == "sendimagebyname") {
        return handleSendImageByName(cmd);
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

    QString variant = params["variant"].toString();
    bool result = variant.isEmpty()
        ? m_corneaWidget->powerOnBySerial(serial)
        : m_corneaWidget->powerOnBySerial(serial, variant);
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

QJsonObject TcpServer::handleListImages(const QJsonObject &params)
{
    Q_UNUSED(params);
    QStringList names = m_corneaWidget->getImageNames();
    QJsonArray arr;
    for (const QString &name : names) {
        arr.append(name);
    }
    QJsonObject data;
    data["images"] = arr;
    data["count"] = arr.size();
    return makeSuccess(data);
}

QJsonObject TcpServer::handleSendImageByName(const QJsonObject &params)
{
    QString serial = params["serial"].toString();
    QString name = params["name"].toString();

    if (serial.isEmpty())
        return makeError("Missing 'serial' parameter");
    if (name.isEmpty())
        return makeError("Missing 'name' parameter");

    QImage image = m_corneaWidget->getImageByName(name);
    if (image.isNull())
        return makeError(QString("Image not found: %1").arg(name));

    ApiResult result = m_corneaWidget->sendImageBySerialEx(serial, image);
    if (result.success)
        return makeSuccess();
    else
        return makeError(result.error);
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
    if (!client || client->state() == QAbstractSocket::UnconnectedState) {
        qWarning() << "[TCP] Cannot send response: client disconnected";
        return;
    }

    QJsonDocument doc(response);
    QByteArray data = doc.toJson(QJsonDocument::Compact) + "\n";

    // Log sent response (skip high-frequency getTemperature)
    QString cmd = response.value("cmd").toString();
    if (cmd != "getTemperature") {
        qInfo() << "[TCP] TX >>" << data.trimmed();
    }

    client->write(data);
    client->flush();

    emit responseSent(QString::fromUtf8(data.trimmed()));
}
