#pragma once

// PanelSubprocess — Phase 2 of the per-panel-process refactor.
//
// Wraps a python.exe child running panel_worker.py over stdin/stdout JSON-RPC.
// Each instance owns ONE panel's cornea_rax720 object. Six instances => six
// processes => six independent Python interpreters => six independent GILs.
// This is the alternative to PythonBridge's embedded-Python dispatch, which
// suffers documented 14 s GIL hold cascades under 4+ panel concurrent load.
//
// Threading model
// ---------------
// QProcess on Windows owns a QWinEventNotifier which is hard-bound to the
// thread that constructed it. Touching QProcess from a different thread
// crashes with "Event notifiers cannot be enabled or disabled from another
// thread". To make this class safely callable from ANY caller thread, every
// PanelSubprocess owns its OWN dedicated QThread; QProcess lives there, and
// all operations (spawn, sendBlocking, stop) are dispatched onto that thread
// via Qt::BlockingQueuedConnection. Caller blocks until the reply lands.
//
// The result: arbitrary CC server worker threads can call sendBlocking()
// concurrently on DIFFERENT instances of PanelSubprocess (one per panel)
// without any cross-thread QProcess violations. Within one instance, calls
// are still serial because the worker thread runs a single event loop.

#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <atomic>
#include <functional>

class PanelSubprocess : public QObject {
    Q_OBJECT
public:
    explicit PanelSubprocess(QObject *parent = nullptr);
    ~PanelSubprocess() override;

    // Set environment / paths before spawn().
    void setPythonExe(const QString &path)         { m_pythonExe = path; }
    void setWorkerScript(const QString &path)      { m_workerScript = path; }
    void setExtraEnv(const QStringList &env)       { m_extraEnv = env; }

    // Start the python.exe child. Returns false if QProcess can't launch
    // the binary or if the readiness ping doesn't come back within
    // `readyTimeoutMs`. Blocks the caller until done.
    bool spawn(int readyTimeoutMs = 5000);

    // Send one JSON-RPC request; block until matching response arrives or
    // `timeoutMs` elapses. On any error returns an object with
    // success=false + an "error" string. Safe to call from any thread.
    QJsonObject sendBlocking(const QString &cmd,
                             const QJsonObject &argsObj,
                             int timeoutMs);

    QJsonObject sendBlocking(const QString &cmd, int timeoutMs) {
        return sendBlocking(cmd, QJsonObject{}, timeoutMs);
    }

    // Graceful shutdown: send `shutdown` then wait for process exit; kill
    // on timeout. Always safe to call (idempotent). Blocks the caller.
    void stop(int gracefulTimeoutMs = 5000);

    bool isRunning() const;
    qint64 processId() const;

signals:
    // Forwarded child stderr lines (one per signal, CR/LF stripped).
    void logMessage(const QString &line);

    // Emitted when the child exits without a prior stop() call.
    void unexpectedExit(int exitCode, QProcess::ExitStatus exitStatus);

private:
    // ----- Worker-thread bodies. Run only on m_workerThread. -----
    bool        spawnImpl(int readyTimeoutMs);
    QJsonObject sendBlockingImpl(const QString &cmd,
                                  const QJsonObject &argsObj,
                                  int timeoutMs);
    void        stopImpl(int gracefulTimeoutMs);
    QJsonObject readResponseUntilId(int expectedId, int timeoutMs);

    void        onReadyReadStandardError();
    void        onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

    // Run `fn` on m_workerThread synchronously. Returns true if the call
    // was dispatched (independent of fn's outcome). Used by the public
    // API to keep all QProcess access on its home thread.
    template <typename Fn>
    bool runOnWorker(Fn fn);

private:
    QThread   m_workerThread;
    QProcess *m_proc = nullptr;       // created on m_workerThread in spawnImpl

    QString   m_pythonExe;
    QString   m_workerScript;
    QStringList m_extraEnv;

    std::atomic<int> m_nextId{1};

    QByteArray m_stdoutBuf;
    QByteArray m_stderrBuf;

    bool m_userInitiatedStop = false;
    bool m_threadStarted = false;
};
