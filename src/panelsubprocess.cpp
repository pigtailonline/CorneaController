#include "panelsubprocess.h"

#include <QJsonDocument>
#include <QElapsedTimer>
#include <QDebug>
#include <QCoreApplication>

PanelSubprocess::PanelSubprocess(QObject *parent)
    : QObject(parent)
{
    // Register meta types ONCE per process so BlockingQueuedConnection can
    // marshal them across threads. Safe to call multiple times.
    static const bool s_registered = []() {
        qRegisterMetaType<QProcess::ExitStatus>("QProcess::ExitStatus");
        return true;
    }();
    Q_UNUSED(s_registered);

    // Dedicated event loop for this panel. Everything QProcess-related
    // happens on this thread; the caller's thread only ever marshals
    // args + waits for the reply. Without this the QWinEventNotifier
    // inside QProcess (Windows backend) trips its "wrong thread" assertion
    // and CC crashes — exactly the 2026-05-27 07:55 cascade we just hit.
    m_workerThread.setObjectName("PanelSubprocessWorker");
    m_workerThread.start();
    m_threadStarted = true;
    moveToThread(&m_workerThread);
}

PanelSubprocess::~PanelSubprocess()
{
    if (m_threadStarted) {
        // Best-effort graceful stop on the worker thread.
        if (m_proc) {
            stop(2000);
        }
        m_workerThread.quit();
        if (!m_workerThread.wait(3000)) {
            m_workerThread.terminate();
            m_workerThread.wait(1000);
        }
        m_threadStarted = false;
    }
    // m_proc is deleted as a child of `this`, freed when QObject base destructs.
}

template <typename Fn>
bool PanelSubprocess::runOnWorker(Fn fn)
{
    if (QThread::currentThread() == &m_workerThread) {
        fn();
        return true;
    }
    // BlockingQueuedConnection waits for the lambda to finish before
    // returning — gives sendBlocking its synchronous contract while still
    // running QProcess calls on the right thread.
    return QMetaObject::invokeMethod(this, std::move(fn), Qt::BlockingQueuedConnection);
}

bool PanelSubprocess::spawn(int readyTimeoutMs)
{
    bool ok = false;
    runOnWorker([&]() { ok = spawnImpl(readyTimeoutMs); });
    return ok;
}

bool PanelSubprocess::spawnImpl(int readyTimeoutMs)
{
    if (m_pythonExe.isEmpty() || m_workerScript.isEmpty()) {
        emit logMessage("PanelSubprocess::spawn: python_exe or worker_script not set");
        return false;
    }

    // Construct QProcess on THIS thread (worker). All subsequent ops
    // happen here too. Parent = this so it gets deleted with us.
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_proc, &QProcess::readyReadStandardError,
            this, &PanelSubprocess::onReadyReadStandardError);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &PanelSubprocess::onProcessFinished);

    QStringList env = QProcess::systemEnvironment();
    env << "PYTHONIOENCODING=utf-8" << "PYTHONUNBUFFERED=1";
    for (const QString &kv : m_extraEnv) env << kv;
    m_proc->setEnvironment(env);

    m_proc->start(m_pythonExe, QStringList{m_workerScript});
    if (!m_proc->waitForStarted(readyTimeoutMs)) {
        emit logMessage(QString("PanelSubprocess::spawn: failed to start %1 %2 — %3")
                            .arg(m_pythonExe, m_workerScript, m_proc->errorString()));
        return false;
    }

    // Round-trip a ping so we know stdin/stdout JSON framing is healthy
    // before the caller starts queuing real work. Done inline here (no
    // marshal) because we're already on the worker thread.
    const QJsonObject reply = sendBlockingImpl("ping", QJsonObject{}, readyTimeoutMs);
    if (!reply.value("success").toBool()) {
        emit logMessage(QString("PanelSubprocess::spawn: ping failed: %1")
                            .arg(reply.value("error").toString()));
        return false;
    }
    return true;
}

QJsonObject PanelSubprocess::sendBlocking(const QString &cmd,
                                          const QJsonObject &argsObj,
                                          int timeoutMs)
{
    QJsonObject result;
    runOnWorker([&]() { result = sendBlockingImpl(cmd, argsObj, timeoutMs); });
    return result;
}

QJsonObject PanelSubprocess::sendBlockingImpl(const QString &cmd,
                                               const QJsonObject &argsObj,
                                               int timeoutMs)
{
    QJsonObject errObj{{"success", false}};
    if (!m_proc || m_proc->state() != QProcess::Running) {
        errObj["error"] = "subprocess not running";
        return errObj;
    }

    const int id = m_nextId.fetch_add(1);
    QJsonObject req{
        {"id", id},
        {"cmd", cmd},
        {"args", argsObj},
    };
    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n";

    if (m_proc->write(line) != line.size()) {
        errObj["error"] = QString("write failed: %1").arg(m_proc->errorString());
        return errObj;
    }
    m_proc->waitForBytesWritten(1000);
    return readResponseUntilId(id, timeoutMs);
}

QJsonObject PanelSubprocess::readResponseUntilId(int expectedId, int timeoutMs)
{
    QJsonObject errObj{{"success", false}, {"id", expectedId}};
    QElapsedTimer clock;
    clock.start();

    while (true) {
        m_stdoutBuf += m_proc->readAllStandardOutput();

        int nl;
        while ((nl = m_stdoutBuf.indexOf('\n')) >= 0) {
            const QByteArray oneLine = m_stdoutBuf.left(nl).trimmed();
            m_stdoutBuf.remove(0, nl + 1);
            if (oneLine.isEmpty()) continue;

            QJsonParseError jpe;
            const QJsonDocument doc = QJsonDocument::fromJson(oneLine, &jpe);
            if (jpe.error != QJsonParseError::NoError || !doc.isObject()) {
                emit logMessage(QString("PanelSubprocess: non-JSON stdout: %1")
                                    .arg(QString::fromUtf8(oneLine)));
                continue;
            }
            QJsonObject resp = doc.object();
            const int rid = resp.value("id").toInt(-1);
            if (rid == expectedId) {
                return resp;
            }
            emit logMessage(QString("PanelSubprocess: discarding stale resp id=%1 (waiting for %2)")
                                .arg(rid).arg(expectedId));
        }

        const qint64 remaining = qint64(timeoutMs) - clock.elapsed();
        if (remaining <= 0) {
            errObj["error"] = "timed out";
            return errObj;
        }
        if (!m_proc->waitForReadyRead(int(qMin<qint64>(remaining, 250)))) {
            if (m_proc->state() != QProcess::Running) {
                errObj["error"] = QString("subprocess exited (code=%1)").arg(m_proc->exitCode());
                return errObj;
            }
        }
    }
}

void PanelSubprocess::stop(int gracefulTimeoutMs)
{
    runOnWorker([&]() { stopImpl(gracefulTimeoutMs); });
}

void PanelSubprocess::stopImpl(int gracefulTimeoutMs)
{
    if (!m_proc || m_proc->state() == QProcess::NotRunning) return;
    m_userInitiatedStop = true;

    if (m_proc->state() == QProcess::Running) {
        QJsonObject req{{"id", m_nextId.fetch_add(1)},
                        {"cmd", "shutdown"}, {"args", QJsonObject{}}};
        const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n";
        m_proc->write(line);
        m_proc->waitForBytesWritten(500);
        m_proc->closeWriteChannel();
    }

    if (!m_proc->waitForFinished(gracefulTimeoutMs)) {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
}

bool PanelSubprocess::isRunning() const
{
    // No invokeMethod here — QProcess::state() is just an enum read of a
    // member; reading without the worker thread's lock is racy but only
    // returns slightly stale info, which is good enough for the
    // isDeviceConnected check. The hot path (sendBlocking) does proper
    // dispatch and will detect a not-running state via its own check.
    return m_proc && m_proc->state() == QProcess::Running;
}

qint64 PanelSubprocess::processId() const
{
    return m_proc ? m_proc->processId() : -1;
}

void PanelSubprocess::onReadyReadStandardError()
{
    m_stderrBuf += m_proc->readAllStandardError();
    int nl;
    while ((nl = m_stderrBuf.indexOf('\n')) >= 0) {
        const QByteArray oneLine = m_stderrBuf.left(nl);
        m_stderrBuf.remove(0, nl + 1);
        const QString s = QString::fromUtf8(oneLine).trimmed();
        if (!s.isEmpty()) emit logMessage(s);
    }
}

void PanelSubprocess::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_userInitiatedStop) return;
    emit unexpectedExit(exitCode, exitStatus);
}
