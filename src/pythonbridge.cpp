#include "pythonbridge.h"

// Qt defines 'slots' as a macro, but Python/NumPy headers use 'slots' as a member name
// We need to temporarily undefine it before including Python headers
#ifdef slots
#undef slots
#endif

// Force release Python library even in debug builds (python312_d.lib is not included in standard Python)
#ifdef _DEBUG
#undef _DEBUG
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define _DEBUG
#else
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

// Restore Qt slots macro
#define slots Q_SLOTS

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QBuffer>
#include "panelsubprocess.h"
#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <functional>

// ---------------------------------------------------------------------------
// Dispatch helpers: run work on a Python-capable thread with proper GIL
// handling. After initialize() calls PyEval_SaveThread(), the GIL is no
// longer held by any C++ thread. Every dispatch lambda acquires the GIL via
// PyGILState_Ensure(), runs the work, then releases it. This is what enables
// several device-worker threads to call Python in parallel (pyftdi/SPI I/O
// releases the GIL internally, letting other threads make progress).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Watchdog: tracks every in-flight dispatchToDevice / dispatch lambda so a
// dedicated timer thread can detect "stuck" ops (>5s) and log the entire
// in-flight set — gives us a snapshot of what was queued when the
// 14-second stall occurred, even when Python is unresponsive.
// ---------------------------------------------------------------------------
struct InFlightOp {
    qint64  start_ms;
    QString tag;
    int     instanceId;   // -1 for non-device dispatch
    quintptr tid;
};
static QMutex            g_watchdogMutex;
static QMap<quint64, InFlightOp> g_inFlightOps;  // op_id → details
static QAtomicInteger<quint64>   g_nextOpId{1};

// Per-second op-frequency counter. Reset and logged once per watchdog tick.
// Used to answer "is a burst of cmds causing the stuck?" — if a stuck event is
// preceded by an [FREQ/1s] line showing e.g. inst=2 fielding 30 ops in a second,
// the libusb queue is being saturated. Tagged by invokeOnWorker tag so we can
// see per-instance breakdown.
static QMutex             g_freqMutex;
static QMap<QString, int> g_freqCounter;

// Global libusb serialization (Method 1, v1.1.17):
// Across ALL per-instance worker threads, only ONE fn() runs at a time. This
// trades parallelism for stability — eliminates the libusb-win32 kernel
// driver race condition observed in production 2026-05-18, where multiple
// concurrent libusb_bulk_transfer / control_transfer calls into different
// FT4232 boards triggered IRP-level stucks of 30-105 seconds and a 6-instance
// cascade. With serialization the worst case becomes "queue-and-wait" not
// "all-stuck-together"; throughput drops modestly but predictably.
//
// Mutex is acquired BEFORE the GIL so a thread waiting on the mutex does
// not also hold the GIL — keeps unrelated Python work in other threads
// (e.g. logging, background timers) responsive.
static QMutex g_libusbGlobalMutex;

// Forward decl
static void watchdogEnsureStarted();

static quint64 watchdogRegister(const QString &tag, int instanceId) {
    watchdogEnsureStarted();
    const quint64 id = g_nextOpId.fetchAndAddRelaxed(1);
    InFlightOp op;
    op.start_ms = QDateTime::currentMSecsSinceEpoch();
    op.tag = tag;
    op.instanceId = instanceId;
    op.tid = reinterpret_cast<quintptr>(QThread::currentThreadId());
    QMutexLocker lock(&g_watchdogMutex);
    g_inFlightOps.insert(id, op);
    return id;
}
static void watchdogDeregister(quint64 id) {
    QMutexLocker lock(&g_watchdogMutex);
    g_inFlightOps.remove(id);
}

// Tracks which ops have already been warned to avoid re-printing every 1s.
static QSet<quint64> g_watchdogWarned;

// Dump all Python thread stacks via sys._current_frames() + traceback.format_stack().
// Called from watchdog thread when a stuck op is detected. Acquires GIL — which
// can take a moment if the stuck op is currently inside a Python-with-GIL section,
// but during the 2026-05-15 release stuck events we observed 0 gil_wait>30ms,
// meaning the stuck Python ops release GIL during USB I/O — so the watchdog
// CAN grab GIL during those release windows and inspect frames.
//
// Without this dump we know fn_duration but not which Python source line —
// this fills that gap, pointing directly at pyftdi/ar_display_lab_lib row.
static void watchdogDumpPythonStacks(const QString &context) {
    if (!Py_IsInitialized()) return;   // SIM-build / pre-init — no Python to inspect

    PyGILState_STATE g = PyGILState_Ensure();

    QString out = QString("[WATCHDOG] === Python thread stacks (%1) ===\n").arg(context);
    bool ok = true;

    PyObject *sys = PyImport_ImportModule("sys");
    PyObject *getCurFrames = sys ? PyObject_GetAttrString(sys, "_current_frames") : nullptr;
    PyObject *frames = getCurFrames ? PyObject_CallObject(getCurFrames, nullptr) : nullptr;
    PyObject *tb = PyImport_ImportModule("traceback");
    PyObject *formatStack = tb ? PyObject_GetAttrString(tb, "format_stack") : nullptr;

    if (frames && PyDict_Check(frames) && formatStack) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(frames, &pos, &key, &value)) {
            unsigned long tid = PyLong_AsUnsignedLong(key);
            out += QString("Thread 0x%1:\n").arg(tid, 0, 16);

            PyObject *args = PyTuple_Pack(1, value);
            PyObject *stackList = PyObject_CallObject(formatStack, args);
            Py_DECREF(args);
            if (stackList && PyList_Check(stackList)) {
                Py_ssize_t n = PyList_Size(stackList);
                for (Py_ssize_t i = 0; i < n; ++i) {
                    PyObject *item = PyList_GetItem(stackList, i);  // borrowed
                    if (item && PyUnicode_Check(item)) {
                        out += "  " + QString::fromUtf8(PyUnicode_AsUTF8(item));
                    }
                }
            }
            Py_XDECREF(stackList);
            if (PyErr_Occurred()) PyErr_Clear();
        }
    } else {
        ok = false;
    }
    out += "[WATCHDOG] === end Python stacks ===";

    Py_XDECREF(formatStack);
    Py_XDECREF(tb);
    Py_XDECREF(frames);
    Py_XDECREF(getCurFrames);
    Py_XDECREF(sys);
    if (PyErr_Occurred()) PyErr_Clear();

    PyGILState_Release(g);

    if (ok) {
        qWarning().noquote() << out;
    } else {
        qWarning() << "[WATCHDOG] Python stack dump unavailable (sys/traceback import failed)";
    }
}
// Periodic check fired every 1s on a dedicated watchdog thread. Independent
// of GIL, Qt event loop, or any per-instance worker — so it keeps running
// even when Python is fully wedged.
static void watchdogTick() {
    // First, snapshot+reset the per-second freq counter and emit one line.
    // Always log (even when empty) so the cadence is obvious in the log file
    // and we can correlate to STUCK events by timestamp.
    QString freqLine;
    {
        QMutexLocker fl(&g_freqMutex);
        if (!g_freqCounter.isEmpty()) {
            QStringList parts;
            int total = 0;
            for (auto it = g_freqCounter.begin(); it != g_freqCounter.end(); ++it) {
                parts.append(QString("%1=%2").arg(it.key()).arg(it.value()));
                total += it.value();
            }
            freqLine = QString("[FREQ/1s] total=%1 %2").arg(total).arg(parts.join(" "));
            g_freqCounter.clear();
        }
    }
    if (!freqLine.isEmpty()) qDebug().noquote() << freqLine;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<QPair<quint64, InFlightOp>> stuck;
    QMutexLocker lock(&g_watchdogMutex);
    for (auto it = g_inFlightOps.begin(); it != g_inFlightOps.end(); ++it) {
        if ((now - it.value().start_ms) > 5000 && !g_watchdogWarned.contains(it.key())) {
            stuck.append(qMakePair(it.key(), it.value()));
            g_watchdogWarned.insert(it.key());
        }
    }
    // Clean warned set: remove ids no longer in-flight
    QSet<quint64> live_ids = QSet<quint64>(g_inFlightOps.keyBegin(), g_inFlightOps.keyEnd());
    g_watchdogWarned.intersect(live_ids);
    // List ALL in-flight ops for context (not just newly-stuck)
    QList<QPair<quint64, InFlightOp>> all_in_flight;
    for (auto it = g_inFlightOps.begin(); it != g_inFlightOps.end(); ++it) {
        all_in_flight.append(qMakePair(it.key(), it.value()));
    }
    lock.unlock();
    if (stuck.isEmpty()) return;
    qWarning() << "==================== [WATCHDOG] STUCK OPS DETECTED ====================";
    for (const auto &p : stuck) {
        qWarning().noquote() << QString("  STUCK op_id=%1 elapsed=%2ms tag=%3 tid=0x%4 — Python call hasn't returned!")
                                .arg(p.first)
                                .arg(now - p.second.start_ms)
                                .arg(p.second.tag)
                                .arg(p.second.tid, 0, 16);
    }
    qWarning() << "[WATCHDOG] Full in-flight snapshot (" << all_in_flight.size() << " ops):";
    for (const auto &p : all_in_flight) {
        qWarning().noquote() << QString("    op_id=%1 elapsed=%2ms tag=%3 tid=0x%4")
                                .arg(p.first)
                                .arg(now - p.second.start_ms)
                                .arg(p.second.tag)
                                .arg(p.second.tid, 0, 16);
    }
    qWarning() << "=======================================================================";

    // Dump Python stacks of ALL threads. This is the key new instrumentation
    // for the 2026-05-15 release stuck investigation: fn_duration tells us
    // HOW long the Python call took, but not WHICH source line it's stuck on.
    // sys._current_frames() returns the frame of each running Python thread;
    // we format and log so the next stuck event self-identifies the culprit.
    QString context;
    for (const auto &p : stuck) {
        context += QString("op_id=%1 elapsed=%2ms; ").arg(p.first).arg(now - p.second.start_ms);
    }
    watchdogDumpPythonStacks(context.trimmed());
}

// Dedicated watchdog thread. Started lazily on first registerOp.
static QThread *g_watchdogThread = nullptr;
static QTimer  *g_watchdogTimer  = nullptr;
static QMutex   g_watchdogStartMutex;
static void watchdogEnsureStarted() {
    QMutexLocker lock(&g_watchdogStartMutex);
    if (g_watchdogThread) return;
    g_watchdogThread = new QThread();
    g_watchdogThread->setObjectName("CCWatchdog");
    g_watchdogThread->start();
    // Create the timer on the watchdog thread and start it via QueuedConnection
    // so timer events fire on the watchdog thread's event loop.
    g_watchdogTimer = new QTimer();
    g_watchdogTimer->moveToThread(g_watchdogThread);
    QObject::connect(g_watchdogTimer, &QTimer::timeout, []() { watchdogTick(); });
    QMetaObject::invokeMethod(g_watchdogTimer, [](){ g_watchdogTimer->start(1000); },
                              Qt::QueuedConnection);
}

// Invokes f() on `worker` and blocks the caller until it completes.
// When `acquireGil` is true, the GIL is wrapped around f()'s invocation so
// the lambda can freely use the Python C API.
//
// Shared-state lifetime contract: the queued lambda captures `state` and `fn`
// BY VALUE, so once invokeOnWorker has posted the lambda it can safely return
// (whether the lambda finished or hit the 15s timeout). Earlier versions
// captured `&f / &result / &done` by REFERENCE on the caller's stack — when
// dispatchToDevice timed out the caller returned and those stack slots died,
// but the lambda remained queued in the worker's event loop. As soon as the
// worker thread unblocked (e.g. pyftdi USB hub conflict cleared), the queued
// lambda fired and wrote to the dead stack frame → access violation, process
// died. Observed 2026-05-12 15:07: three simultaneous dispatchToDevice
// timeouts on inst=6/7/8 followed by process crash.
template<typename Result>
struct InvokeState {
    Result result{};
    QSemaphore done;
};
template<typename F>
static auto invokeOnWorker(QObject *worker, QThread *thread, F &&f,
                           bool acquireGil, const QString &timeoutTag,
                           std::function<void(const QString&)> logFn)
    -> decltype(f())
{
    if (QThread::currentThread() == thread) {
        if (acquireGil) {
            PyGILState_STATE g = PyGILState_Ensure();
            auto r = f();
            PyGILState_Release(g);
            return r;
        }
        return f();
    }
    using Result = decltype(f());
    auto state = std::make_shared<InvokeState<Result>>();
    const qint64 t_dispatched = QDateTime::currentMSecsSinceEpoch();
    const QString tag = timeoutTag;

    // Diagnostic timing: log the three gaps that explain a 14s stall —
    //  (a) queue_wait    = invokeMethod → lambda start (worker event-loop
    //                      delivery delay; should be < 5ms when worker idle)
    //  (b) gil_wait      = lambda start → PyGILState_Ensure returns (Python
    //                      GIL contention; large value means another thread
    //                      held the GIL — prime suspect for cross-worker stall)
    //  (c) fn_duration   = inside fn() (actual Python work; large value means
    //                      this op itself is slow and likely held GIL too)
    // Only log when above thresholds to keep release builds quiet.
    QMetaObject::invokeMethod(worker, [state, fn = std::forward<F>(f), acquireGil,
                                       t_dispatched, tag, logFn]() mutable {
        const qint64 t_lambda = QDateTime::currentMSecsSinceEpoch();
        const qint64 queue_wait = t_lambda - t_dispatched;
        if (queue_wait > 30 && logFn) {
            logFn(QString("[invokeOnWorker] %1 queue_wait=%2ms tid=0x%3")
                  .arg(tag).arg(queue_wait)
                  .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
        }
        // Register with watchdog before doing any Python work — watchdog can
        // then list this op as "in-flight" if the lambda gets stuck below.
        // Instance id is encoded in the tag for dispatchToDevice (logFn prefix
        // adds it on the caller side), so we extract via a sentinel here.
        const quint64 op_id = watchdogRegister(tag, /*instanceId=*/ -1);
        {
            QMutexLocker fl(&g_freqMutex);
            g_freqCounter[tag]++;
        }
        // Global libusb serialization — see g_libusbGlobalMutex docstring.
        // Acquired BEFORE GIL so blocked threads don't hold the GIL.
        const qint64 t_lock_start = QDateTime::currentMSecsSinceEpoch();
        QMutexLocker libusbLock(&g_libusbGlobalMutex);
        const qint64 lock_wait = QDateTime::currentMSecsSinceEpoch() - t_lock_start;
        if (lock_wait > 50 && logFn) {
            logFn(QString("[invokeOnWorker] %1 libusb_lock_wait=%2ms tid=0x%3 — another instance was using USB")
                  .arg(tag).arg(lock_wait)
                  .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
        }
        if (acquireGil) {
            PyGILState_STATE g = PyGILState_Ensure();
            const qint64 t_gil = QDateTime::currentMSecsSinceEpoch();
            const qint64 gil_wait = t_gil - t_lambda - lock_wait;
            if (gil_wait > 30 && logFn) {
                logFn(QString("[invokeOnWorker] %1 gil_wait=%2ms tid=0x%3 — GIL held by another thread")
                      .arg(tag).arg(gil_wait)
                      .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
            }
            state->result = fn();
            const qint64 fn_ms = QDateTime::currentMSecsSinceEpoch() - t_gil;
            if (fn_ms > 300 && logFn) {
                logFn(QString("[invokeOnWorker] %1 fn_duration=%2ms tid=0x%3 — long Python call (held GIL %2ms)")
                      .arg(tag).arg(fn_ms)
                      .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16));
            }
            PyGILState_Release(g);
        } else {
            state->result = fn();
        }
        watchdogDeregister(op_id);
        state->done.release();
    }, Qt::QueuedConnection);
    if (!state->done.tryAcquire(1, 15000)) {
        if (logFn) logFn(QString("[PythonBridge] %1 TIMEOUT (15s) — thread may be stuck").arg(tag));
        // Snapshot in-flight ops at timeout — tells us what else was queued
        // and when each op started, which is the actual smoking gun for
        // identifying which Python call is blocking everyone else.
        QMap<quint64, InFlightOp> snapshot;
        {
            QMutexLocker lock(&g_watchdogMutex);
            snapshot = g_inFlightOps;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (logFn) {
            logFn(QString("[Watchdog] %1 in-flight ops at TIMEOUT moment:").arg(snapshot.size()));
            for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
                const auto &op = it.value();
                logFn(QString("  op_id=%1 elapsed=%2ms tag=%3 tid=0x%4")
                      .arg(it.key())
                      .arg(now - op.start_ms)
                      .arg(op.tag)
                      .arg(op.tid, 0, 16));
            }
        }
        return state->result;
    }
    return state->result;
}

template<typename F>
auto PythonBridge::dispatch(F &&f) -> decltype(f()) {
    auto log = [this](const QString &msg){ emit logMessage(msg); };
    return invokeOnWorker(m_pythonWorker, &m_pythonThread, std::forward<F>(f),
                          /*acquireGil*/ true, QStringLiteral("dispatch"), log);
}

void PythonBridge::dispatchVoid(std::function<void()> f) {
    if (QThread::currentThread() == &m_pythonThread) {
        PyGILState_STATE g = PyGILState_Ensure();
        f();
        PyGILState_Release(g);
        return;
    }
    // Heap-allocated semaphore so the lambda is safe even if the caller hits
    // the 15s timeout and returns — without this, a delayed lambda fires
    // against a destroyed stack-allocated QSemaphore. See invokeOnWorker
    // comment for the full root-cause analysis.
    auto done = std::make_shared<QSemaphore>();
    auto wrapped = [fn = std::move(f), done]() {
        PyGILState_STATE g = PyGILState_Ensure();
        fn();
        PyGILState_Release(g);
        done->release();
    };
    QMetaObject::invokeMethod(m_pythonWorker, std::move(wrapped), Qt::QueuedConnection);
    if (!done->tryAcquire(1, 15000)) {
        emit logMessage("[PythonBridge] dispatchVoid TIMEOUT (15s) — Python thread may be stuck");
    }
}

template<typename F>
auto PythonBridge::dispatchToDevice(int instanceId, F &&f) -> decltype(f()) {
    QObject *worker = nullptr;
    QThread *thread = nullptr;
    {
        QMutexLocker lock(&m_deviceThreadsMutex);
        worker = m_deviceWorkers.value(instanceId, nullptr);
        thread = m_deviceThreads.value(instanceId, nullptr);
    }
    // Fallback to the main Python thread if the instance has no dedicated
    // worker yet (shouldn't happen post-createDeviceInstance, but keeps the
    // method safe against lookup misses).
    if (!worker || !thread) {
        return dispatch(std::forward<F>(f));
    }
    auto log = [this, instanceId](const QString &msg) {
        emit logMessage(QString("[PythonBridge/inst=%1] %2").arg(instanceId).arg(msg));
    };
    const QString tag = QString("dispatchToDevice[inst=%1]").arg(instanceId);
    return invokeOnWorker(worker, thread, std::forward<F>(f),
                          /*acquireGil*/ true, tag, log);
}

void PythonBridge::dispatchVoidToDevice(int instanceId, std::function<void()> f) {
    QObject *worker = nullptr;
    QThread *thread = nullptr;
    {
        QMutexLocker lock(&m_deviceThreadsMutex);
        worker = m_deviceWorkers.value(instanceId, nullptr);
        thread = m_deviceThreads.value(instanceId, nullptr);
    }
    if (!worker || !thread) {
        dispatchVoid(std::move(f));
        return;
    }
    if (QThread::currentThread() == thread) {
        PyGILState_STATE g = PyGILState_Ensure();
        f();
        PyGILState_Release(g);
        return;
    }
    // Heap-allocated semaphore so a delayed lambda (after timeout) doesn't
    // write to dead caller stack. Same root-cause as invokeOnWorker.
    auto done = std::make_shared<QSemaphore>();
    auto wrapped = [fn = std::move(f), done]() {
        PyGILState_STATE g = PyGILState_Ensure();
        fn();
        PyGILState_Release(g);
        done->release();
    };
    QMetaObject::invokeMethod(worker, std::move(wrapped), Qt::QueuedConnection);
    if (!done->tryAcquire(1, 15000)) {
        emit logMessage(QString("[PythonBridge/inst=%1] dispatchVoidToDevice TIMEOUT (15s)").arg(instanceId));
    }
}

// Per-instance worker lifecycle.
QObject *PythonBridge::ensureDeviceWorker(int instanceId)
{
    QMutexLocker lock(&m_deviceThreadsMutex);
    QObject *existing = m_deviceWorkers.value(instanceId, nullptr);
    if (existing) return existing;

    QThread *t = new QThread();
    t->setObjectName(QString("CorneaDev%1").arg(instanceId));
    QObject *w = new QObject();
    w->moveToThread(t);
    t->start();
    m_deviceThreads[instanceId] = t;
    m_deviceWorkers[instanceId] = w;
    return w;
}

void PythonBridge::destroyDeviceWorker(int instanceId)
{
    QThread *t = nullptr;
    QObject *w = nullptr;
    {
        QMutexLocker lock(&m_deviceThreadsMutex);
        t = m_deviceThreads.take(instanceId);
        w = m_deviceWorkers.take(instanceId);
    }
    if (!t && !w) return;
    if (t) {
        t->quit();
        t->wait();
        delete t;
    }
    // Worker is owned by its thread's event loop; safe to delete now that
    // the thread has exited.
    delete w;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
PythonBridge::PythonBridge(QObject *parent)
    : QObject(parent)
    , m_initialized(false)
    , m_module(nullptr)
    , m_corneaClass(nullptr)
    , m_numpyModule(nullptr)
    , m_executor(nullptr)
    , m_allowDefaultHdf5(false)
    , m_nextInstanceId(0)
{
    m_pythonWorker = new QObject();
    m_pythonWorker->moveToThread(&m_pythonThread);
    m_pythonThread.start();
}

PythonBridge::~PythonBridge()
{
    shutdown();
    m_pythonThread.quit();
    m_pythonThread.wait();
    delete m_pythonWorker;
    m_pythonWorker = nullptr;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
// Simulation sleep helpers (debug builds only).
//   simSleep(ms)       — used by init paths (createDeviceInstance, preInit).
//                        Always sleeps the given ms, never overridden.
//   simStallSleep(ms)  — used by runtime ops (setBrightness, sendImage,
//                        getPanelId, getLeaTemperature, systemPowerOn/Off).
//                        CC_SIM_STALL_MS env var, when set > 0, overrides ms.
//                        Acquires g_simSharedLock for the duration to MIMIC
//                        the Python GIL: a long-running call on one worker
//                        blocks all other workers' Python entries — the
//                        prime suspect for the 2026-05-12 22:23 cross-worker
//                        14s stall. The lock is debug-only and exits with the
//                        function, so production code path is unchanged.
//                        Init paths intentionally use simSleep (no shared
//                        lock) so launch isn't blocked for minutes.
static QMutex g_simSharedLock;
static void simSleep(int ms) { QThread::msleep(ms); }
static void simStallSleep(int ms) {
    const int stallMs = qEnvironmentVariableIntValue("CC_SIM_STALL_MS");
    const int actual = stallMs > 0 ? stallMs : ms;
    const bool useSharedLock = !qEnvironmentVariableIsSet("CC_SIM_NO_SHARED_LOCK");

    if (!useSharedLock) {
        // Control arm: no shared lock — true per-device parallelism.
        QThread::msleep(actual);
        return;
    }
    const qint64 t_arrive = QDateTime::currentMSecsSinceEpoch();
    const auto tid = reinterpret_cast<quintptr>(QThread::currentThreadId());
    g_simSharedLock.lock();
    const qint64 wait_ms = QDateTime::currentMSecsSinceEpoch() - t_arrive;
    if (wait_ms > 5) {
        qDebug().noquote() << QString("[SIM-lock] tid=0x%1 acquired after %2ms wait")
                              .arg(tid, 0, 16).arg(wait_ms);
    }
    QThread::msleep(actual);
    g_simSharedLock.unlock();
}
#endif

bool PythonBridge::initialize(const QString &venvPath)
{
    QStringList defaultDllPaths;
    defaultDllPaths << "C:/Python312" << "D:/projects/deps/dll";
    return initialize(venvPath, "C:/Python312", defaultDllPaths, "C:/google_cal/hdf5_files", false);
}

bool PythonBridge::initialize(const QString &venvPath, const QString &pythonHome,
                               const QStringList &dllPaths, const QString &calPath,
                               bool allowDefaultHdf5)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    emit logMessage("[SIM] Debug simulation mode — skipping Python initialization");
    m_initialized = true;
    return true;
#endif
    // Dispatch entire initialization to the Python worker thread.
    // Python interpreter will be created on that thread.
    // NOTE: bypass the GIL-acquiring dispatch here because Python isn't
    // initialized yet; PyGILState_Ensure would be a no-op at best and
    // undefined behavior at worst. The lambda internally runs Py_Initialize
    // and ends with PyEval_SaveThread so subsequent dispatches can use GIL
    // management normally.
    auto initLambda = [=]() -> bool {
        if (m_initialized) {
            return true;
        }

        m_venvPath = venvPath;
        m_pythonHome = pythonHome;
        m_dllPaths = dllPaths;
        m_calPath = calPath;
        m_allowDefaultHdf5 = allowDefaultHdf5;

        // Set PYTHONHOME and PYTHONPATH environment variables BEFORE initializing Python
        if (!venvPath.isEmpty() && QDir(venvPath).exists()) {
            QString sitePackages = venvPath + "/Lib/site-packages";
            qputenv("PYTHONHOME", m_pythonHome.toUtf8());
            qputenv("PYTHONPATH", sitePackages.toUtf8());

            QString currentPath = qEnvironmentVariable("PATH");
            QString cv2Path = sitePackages + "/cv2";
            QStringList pathParts;
            pathParts << m_pythonHome << cv2Path << m_dllPaths << currentPath;
            QString newPath = pathParts.join(";");
            qputenv("PATH", newPath.toUtf8());

            QString pythonPath = venvPath + "/Scripts/python.exe";
            if (QFileInfo::exists(pythonPath)) {
                std::wstring wpath = pythonPath.toStdWString();
                Py_SetProgramName(wpath.c_str());
            }
        }

        Py_Initialize();
        if (!Py_IsInitialized()) {
            setError("Failed to initialize Python interpreter");
            return false;
        }

        if (!venvPath.isEmpty()) {
            QString sitePackages = venvPath + "/Lib/site-packages";
            PyObject *sysPath = PySys_GetObject("path");
            if (sysPath) {
                PyObject *path = PyUnicode_FromString(sitePackages.toUtf8().constData());
                PyList_Insert(sysPath, 0, path);
                Py_DECREF(path);
            }
        }

        if (_import_array() < 0) {
            setError("Failed to import numpy");
            PyErr_Print();
            return false;
        }

        if (!venvPath.isEmpty()) {
            QString cv2Path = venvPath + "/Lib/site-packages/cv2";
            QString addDllCode = QString(
                "import os\n"
                "if hasattr(os, 'add_dll_directory'):\n"
                "    os.add_dll_directory(r'%1')\n"
            ).arg(cv2Path);
            for (const QString &dllPath : m_dllPaths) {
                addDllCode += QString("    os.add_dll_directory(r'%1')\n").arg(dllPath);
            }
            PyRun_SimpleString(addDllCode.toUtf8().constData());
        }

        // Configure Python output capture
        PyRun_SimpleString(
            "import sys\n"
            "import io\n"
            "import logging\n"
            "\n"
            "class OutputCapture:\n"
            "    def __init__(self):\n"
            "        self.buffer = []\n"
            "        self._noise_starts = ['^', '\\x1b', '\\r', '|', '/', '\\\\',\n"
            "                              'Traceback', 'File \"', 'TypeError', 'AttributeError',\n"
            "                              'AllK', 'Dynamically', 'pmic_mux']\n"
            "        self._noise_contains = ['HotK Handles', 'LstK Handles', 'LstInfoK', 'UsbK Handles',\n"
            "                                'DevK Handles', 'OvlK Handles', 'OvlPoolK', 'StmK Handles',\n"
            "                                'IsochK Handles', 'KLST_DEVINFO', 'HandleSize', 'PoolSize',\n"
            "                                'contiguous memory', 'bytes each', 'unexpected keyword argument',\n"
            "                                'Pcal6534.get_pin_level']\n"
            "    def write(self, text):\n"
            "        stripped = text.strip()\n"
            "        if not stripped or len(stripped) < 3:\n"
            "            return\n"
            "        if any(stripped.startswith(p) for p in self._noise_starts):\n"
            "            return\n"
            "        if any(n in stripped for n in self._noise_contains):\n"
            "            return\n"
            "        if len(set(stripped)) == 1 and len(stripped) > 3:\n"
            "            return\n"
            "        self.buffer.append(text.rstrip())\n"
            "    def flush(self):\n"
            "        pass\n"
            "    def get_and_clear(self):\n"
            "        result = self.buffer[:]\n"
            "        self.buffer.clear()\n"
            "        return result\n"
            "\n"
            "_qt_output_capture = OutputCapture()\n"
            "sys.stdout = _qt_output_capture\n"
            "sys.stderr = _qt_output_capture\n"
            "\n"
            "class QtLogHandler(logging.Handler):\n"
            "    def emit(self, record):\n"
            "        msg = self.format(record)\n"
            "        _qt_output_capture.buffer.append(msg)\n"
            "\n"
            "_qt_log_handler = QtLogHandler()\n"
            "_qt_log_handler.setLevel(logging.INFO)\n"
            "_qt_log_handler.setFormatter(logging.Formatter(\n"
            "    '%(asctime)s,%(msecs)03d [%(levelname)-5.5s] : %(message)s',\n"
            "    datefmt='%Y-%m-%d %H:%M:%S'))\n"
            "\n"
            "root_logger = logging.getLogger()\n"
            "root_logger.setLevel(logging.INFO)\n"
            "root_logger.addHandler(_qt_log_handler)\n"
            "\n"
            "for noisy in ['urllib3', 'PIL', 'matplotlib', 'numba', 'h5py', 'usb', 'pyftdi', 'ftdi']:\n"
            "    logging.getLogger(noisy).setLevel(logging.WARNING)\n"
            "\n"
            "def _intercept_coloredlogs():\n"
            "    try:\n"
            "        import coloredlogs\n"
            "        _original_install = coloredlogs.install\n"
            "        def _custom_install(level=logging.INFO, **kwargs):\n"
            "            logging.getLogger().setLevel(level)\n"
            "        coloredlogs.install = _custom_install\n"
            "    except ImportError:\n"
            "        pass\n"
            "_intercept_coloredlogs()\n"
        );

        if (!importModule("ar_display_lab_lib.control_boards.cornea_rax720")) {
            return false;
        }

        // Monkey-patch CorneaRax720 init paths to serialize construction.
        // rj1lib_external.config exposes CHIP / CHIPSEL / CFGBits / OPTBits /
        // regs / subregs as MODULE-LEVEL globals; select_chip() mutates them.
        // Concurrent invocations from different panel threads observed 52%
        // race rate in pure-Python bench (verify_chip_race.py 2026-05-20)
        // and produce the production NACK / "No answer from FTDI" cascades.
        //
        // v1.1.18 fix scope: wrap BOTH __init__ AND system_power_on. v1.1.13
        // wrapped only __init__, but per the rax_lib docs system_power_on
        // is equivalent to running init_cornea(...) again when hardware has
        // no VSYS rail enable — that path was unprotected, so a powerOn on
        // a previously-constructed instance still raced. system_power_off
        // not wrapped (no init_cornea inside; only PMIC writes).
        //
        // Print on apply success too so we can grep production startup log
        // to confirm the patch fired (previously only logged on failure).
        PyRun_SimpleString(
            "def _patch_cornea_init_lock():\n"
            "    try:\n"
            "        import threading, functools\n"
            "        import ar_display_lab_lib.control_boards.cornea_rax720 as _mod\n"
            "        _init_lock = threading.Lock()\n"
            "        _mod._init_lock = _init_lock  # exposed for diagnostics\n"
            "        wrapped = []\n"
            "        for name in ('__init__', 'system_power_on'):\n"
            "            orig = getattr(_mod.CorneaRax720, name, None)\n"
            "            if orig is None:\n"
            "                continue\n"
            "            def _make_wrapper(orig_fn):\n"
            "                @functools.wraps(orig_fn)\n"
            "                def _wrapper(self, *args, **kwargs):\n"
            "                    with _init_lock:\n"
            "                        return orig_fn(self, *args, **kwargs)\n"
            "                return _wrapper\n"
            "            setattr(_mod.CorneaRax720, name, _make_wrapper(orig))\n"
            "            wrapped.append(name)\n"
            "        print(f'CorneaRax720 _init_lock applied to {wrapped}')\n"
            "    except Exception as e:\n"
            "        print(f'Warning: failed to apply CorneaRax720 _init_lock: {e}')\n"
            "_patch_cornea_init_lock()\n"
        );

        // Create a ThreadPoolExecutor for parallel device creation.
        // Device I/O (USB) releases the GIL, so multiple creations can overlap.
        PyObject *cfModule = PyImport_ImportModule("concurrent.futures");
        if (cfModule) {
            PyObject *executorClass = PyObject_GetAttrString(cfModule, "ThreadPoolExecutor");
            if (executorClass) {
                PyObject *kwargs = PyDict_New();
                PyDict_SetItemString(kwargs, "max_workers", PyLong_FromLong(12));
                PyObject *args = PyTuple_New(0);
                m_executor = PyObject_Call(executorClass, args, kwargs);
                Py_DECREF(args);
                Py_DECREF(kwargs);
                Py_DECREF(executorClass);
            }
            Py_DECREF(cfModule);
        }
        if (!m_executor) {
            emit logMessage("Warning: ThreadPoolExecutor not available, device creation will be serial");
            PyErr_Clear();
        }

        m_initialized = true;
        // Release the GIL held by the main Python thread so per-device worker
        // threads can acquire it via PyGILState_Ensure. Saved state is
        // restored (briefly) at shutdown before Py_Finalize.
        m_savedThreadState = PyEval_SaveThread();
        emit logMessage("Python bridge initialized successfully");
        return true;
    };

    // Manual invoke (bypass dispatch()'s GIL wrapping) because the
    // interpreter isn't live yet when this runs.
    if (QThread::currentThread() == &m_pythonThread) {
        return initLambda();
    }
    bool result = false;
    QSemaphore done;
    QMetaObject::invokeMethod(m_pythonWorker, [&initLambda, &result, &done]() {
        result = initLambda();
        done.release();
    }, Qt::QueuedConnection);
    done.acquire();  // init can take a while; no timeout here
    return result;
}

void PythonBridge::shutdown()
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    m_simDevices.clear();
    m_serialMap.clear();
    m_initOkMap.clear();
    m_initialized = false;
    return;
#endif
    // Tear down all panels. In subprocess mode iterate m_panelProcs;
    // in embedded mode iterate m_deviceWorkers. Either way the
    // destroyDeviceInstance branch picks the right path.
    //
    // Without this, subprocess panels stay LIT after the CC window
    // closes (rax_lib's __del__ doesn't run reliably on process exit,
    // and our cmd_shutdown didn't explicitly power off either — the
    // panel hardware just keeps the rails up until reset).
    {
        QList<int> instanceIds;
        if (m_useSubprocess) {
            QMutexLocker lock(&m_panelProcsMutex);
            instanceIds = m_panelProcs.keys();
        } else {
            QMutexLocker lock(&m_deviceThreadsMutex);
            instanceIds = m_deviceWorkers.keys();
        }
        for (int id : instanceIds) destroyDeviceInstance(id);
    }

    // Everything below runs ON m_pythonThread — that's the thread that holds
    // the saved main interpreter state. Restoring a PyThreadState has to
    // happen on the thread the state was created on, otherwise Py_Finalize
    // walks a corrupt state and segfaults (v1.1.6 bug seen at GUI close).
    auto shutdownLambda = [this]() {
        // Re-acquire GIL + main thread state on this thread.
        if (m_savedThreadState) {
            PyEval_RestoreThread(reinterpret_cast<PyThreadState*>(m_savedThreadState));
            m_savedThreadState = nullptr;
        }

        if (m_initialized) {
            // Shutdown ThreadPoolExecutor (wait for pending tasks)
            if (m_executor) {
                PyObject *shutdownKw = PyDict_New();
                PyDict_SetItemString(shutdownKw, "wait", Py_True);
                PyObject *shutdownMethod = PyObject_GetAttrString(m_executor, "shutdown");
                if (shutdownMethod) {
                    PyObject *r = PyObject_Call(shutdownMethod, PyTuple_New(0), shutdownKw);
                    Py_XDECREF(r);
                    Py_DECREF(shutdownMethod);
                }
                Py_DECREF(shutdownKw);
                Py_DECREF(m_executor);
                m_executor = nullptr;
            }

            if (m_module) {
                Py_DECREF(m_module);
                m_module = nullptr;
            }
            if (m_corneaClass) {
                Py_DECREF(m_corneaClass);
                m_corneaClass = nullptr;
            }
            if (m_numpyModule) {
                Py_DECREF(m_numpyModule);
                m_numpyModule = nullptr;
            }

            Py_Finalize();
            m_initialized = false;
        }
    };

    // Always run shutdown on m_pythonThread. If we're not already on it,
    // post via invokeMethod and wait.
    if (QThread::currentThread() == &m_pythonThread) {
        shutdownLambda();
    } else {
        QSemaphore done;
        QMetaObject::invokeMethod(m_pythonWorker, [&shutdownLambda, &done]() {
            shutdownLambda();
            done.release();
        }, Qt::QueuedConnection);
        done.acquire();
    }
}

// ---------------------------------------------------------------------------
// Internal helpers (always called on Python thread — no dispatch needed)
// ---------------------------------------------------------------------------
bool PythonBridge::importModule(const QString &moduleName)
{
    PyObject *name = PyUnicode_FromString(moduleName.toUtf8().constData());
    m_module = PyImport_Import(name);
    Py_DECREF(name);

    if (!m_module) {
        setError(QString("Failed to import module: %1").arg(moduleName));
        PyErr_Print();
        return false;
    }

    m_corneaClass = PyObject_GetAttrString(m_module, "CorneaRax720");
    if (!m_corneaClass) {
        setError("Failed to get CorneaRax720 class");
        PyErr_Print();
        return false;
    }

    return true;
}

PyObject* PythonBridge::callInstanceMethod(int instanceId, const char *methodName, PyObject *args)
{
    PyObject *instance = getDeviceInstance(instanceId);
    if (!instance) {
        setError(QString("Device instance %1 not found").arg(instanceId));
        return nullptr;
    }

    PyObject *method = PyObject_GetAttrString(instance, methodName);
    if (!method) {
        setError(QString("Method not found: %1").arg(methodName));
        PyErr_Print();
        return nullptr;
    }

    PyObject *result;
    if (args) {
        result = PyObject_CallObject(method, args);
    } else {
        result = PyObject_CallNoArgs(method);
    }

    Py_DECREF(method);

    if (!result) {
        setError(QString("Error calling method: %1").arg(methodName));
        PyErr_Print();
        return nullptr;
    }

    return result;
}

PyObject* PythonBridge::getDeviceInstance(int instanceId) const
{
    return m_deviceInstances.value(instanceId, nullptr);
}

PyObject* PythonBridge::qimageToPyArray(const QImage &image)
{
    QImage rgbImage = image.convertToFormat(QImage::Format_RGB888);

    npy_intp dims[3] = {rgbImage.height(), rgbImage.width(), 3};
    PyObject *array = PyArray_SimpleNew(3, dims, NPY_UINT8);

    if (!array) {
        setError("Failed to create numpy array");
        return nullptr;
    }

    uint8_t *data = static_cast<uint8_t*>(PyArray_DATA(reinterpret_cast<PyArrayObject*>(array)));
    for (int y = 0; y < rgbImage.height(); ++y) {
        const uchar *scanLine = rgbImage.constScanLine(y);
        for (int x = 0; x < rgbImage.width(); ++x) {
            int srcIdx = x * 3;
            int dstIdx = y * rgbImage.width() * 3 + x * 3;
            data[dstIdx + 0] = scanLine[srcIdx + 2];  // B <- R
            data[dstIdx + 1] = scanLine[srcIdx + 1];  // G <- G
            data[dstIdx + 2] = scanLine[srcIdx + 0];  // R <- B
        }
    }

    return array;
}

QVariant PythonBridge::pyObjectToVariant(PyObject *obj)
{
    if (!obj || obj == Py_None) {
        return QVariant();
    }
    if (PyBool_Check(obj)) {
        return QVariant(obj == Py_True);
    }
    if (PyLong_Check(obj)) {
        return QVariant(static_cast<qlonglong>(PyLong_AsLongLong(obj)));
    }
    if (PyFloat_Check(obj)) {
        return QVariant(PyFloat_AsDouble(obj));
    }
    if (PyUnicode_Check(obj)) {
        return QVariant(QString::fromUtf8(PyUnicode_AsUTF8(obj)));
    }
    if (PyDict_Check(obj)) {
        QVariantMap map;
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            QString keyStr;
            if (PyUnicode_Check(key)) {
                keyStr = QString::fromUtf8(PyUnicode_AsUTF8(key));
            } else {
                keyStr = QString::number(pos);
            }
            map[keyStr] = pyObjectToVariant(value);
        }
        return QVariant(map);
    }
    if (PyList_Check(obj)) {
        QVariantList list;
        Py_ssize_t size = PyList_Size(obj);
        for (Py_ssize_t i = 0; i < size; ++i) {
            list.append(pyObjectToVariant(PyList_GetItem(obj, i)));
        }
        return QVariant(list);
    }
    if (PyTuple_Check(obj)) {
        QVariantList list;
        Py_ssize_t size = PyTuple_Size(obj);
        for (Py_ssize_t i = 0; i < size; ++i) {
            list.append(pyObjectToVariant(PyTuple_GetItem(obj, i)));
        }
        return QVariant(list);
    }
    return QVariant();
}

PyObject* PythonBridge::variantToPyObject(const QVariant &var)
{
    switch (var.type()) {
    case QVariant::Bool:
        return PyBool_FromLong(var.toBool() ? 1 : 0);
    case QVariant::Int:
    case QVariant::LongLong:
        return PyLong_FromLongLong(var.toLongLong());
    case QVariant::Double:
        return PyFloat_FromDouble(var.toDouble());
    case QVariant::String:
        return PyUnicode_FromString(var.toString().toUtf8().constData());
    case QVariant::Map: {
        PyObject *dict = PyDict_New();
        QVariantMap map = var.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            PyObject *key = PyUnicode_FromString(it.key().toUtf8().constData());
            PyObject *value = variantToPyObject(it.value());
            PyDict_SetItem(dict, key, value);
            Py_DECREF(key);
            Py_DECREF(value);
        }
        return dict;
    }
    case QVariant::List: {
        QVariantList list = var.toList();
        PyObject *pyList = PyList_New(list.size());
        for (int i = 0; i < list.size(); ++i) {
            PyList_SetItem(pyList, i, variantToPyObject(list[i]));
        }
        return pyList;
    }
    default:
        Py_RETURN_NONE;
    }
}

void PythonBridge::setError(const QString &error)
{
    m_lastError = error;
    emit errorOccurred(error);
    qWarning() << "PythonBridge Error:" << error;
}

void PythonBridge::clearError()
{
    m_lastError.clear();
}

// ---------------------------------------------------------------------------
// Public API methods — each dispatches its entire body to the Python thread
// ---------------------------------------------------------------------------

QList<DeviceInfo> PythonBridge::getAvailableDevicesInfo()
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    QList<DeviceInfo> result;
    for (int i = 0; i < SIM_DEVICE_COUNT; ++i) {
        DeviceInfo info;
        info.index = i;
        info.serial = QString("SIM%1").arg(i, 4, 10, QChar('0'));
        info.displayName = QString("Index %1: %2").arg(i).arg(info.serial);
        result.append(info);
    }
    return result;
#endif
    return dispatch([this]() -> QList<DeviceInfo> {
        QList<DeviceInfo> result;
        if (!m_corneaClass) {
            return result;
        }

        PyRun_SimpleString(
            "try:\n"
            "    from pyftdi.usbtools import UsbTools\n"
            "    UsbTools.flush_cache()\n"
            "except Exception as e:\n"
            "    print(f'[pyftdi] Cache flush failed: {e}')\n"
        );
        PyErr_Clear();

        PyObject *method = PyObject_GetAttrString(m_corneaClass, "get_available_corneas");
        if (!method) {
            PyErr_Print();
            return result;
        }

        PyObject *pyResult = PyObject_CallNoArgs(method);
        Py_DECREF(method);

        if (pyResult && PyTuple_Check(pyResult) && PyTuple_Size(pyResult) >= 2) {
            PyObject *indices = PyTuple_GetItem(pyResult, 0);
            PyObject *serials = PyTuple_GetItem(pyResult, 1);

            if (PyList_Check(indices) && PyList_Check(serials)) {
                Py_ssize_t size = PyList_Size(indices);
                for (Py_ssize_t i = 0; i < size; ++i) {
                    DeviceInfo info;
                    PyObject *idxItem = PyList_GetItem(indices, i);
                    PyObject *serialItem = PyList_GetItem(serials, i);

                    info.index = static_cast<int>(PyLong_AsLong(idxItem));
                    if (PyUnicode_Check(serialItem)) {
                        info.serial = QString::fromUtf8(PyUnicode_AsUTF8(serialItem));
                    }
                    info.displayName = QString("Index %1: %2").arg(info.index).arg(info.serial);
                    result.append(info);
                }
            }
        }

        Py_XDECREF(pyResult);
        return result;
    });
}

int PythonBridge::createDeviceInstance(int deviceIndex, const QString &hardwareVariant)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    int instanceId = m_nextInstanceId++;
    QString serial = QString("SIM%1").arg(deviceIndex, 4, 10, QChar('0'));
    emit logMessage(QString("[SIM] Creating device instance %1 for device %2 (%3)...")
                        .arg(instanceId).arg(deviceIndex).arg(serial));
    simSleep(SIM_DELAY_MS);  // init path — never stalled by CC_SIM_STALL_MS

    SimDevice sim;
    sim.serial = serial;
    sim.connected = true;
    m_simDevices[instanceId] = sim;
    m_initOkMap[instanceId] = true;
    m_serialMap[instanceId] = serial;

    emit logMessage(QString("[SIM] Device instance %1 created: %2").arg(instanceId).arg(serial));
    return instanceId;
#endif

    // Phase 2 subprocess mode: each panel gets its own python.exe child
    // running panel_worker.py. The child's CorneaRax720 ctor (init_cornea
    // + init_rj1 sequence) runs in a fresh interpreter and CANNOT contend
    // with other panels' GIL — see bench_concurrent_cascade.py 2026-05-27
    // for the embedded-Python failure mode this replaces.
    if (m_useSubprocess) {
        const int instanceId = m_nextInstanceId++;
        emit logMessage(QString("[Instance %1] Spawning subprocess for device index %2 (variant: %3)...")
                            .arg(instanceId).arg(deviceIndex).arg(hardwareVariant));

        // No parent: PanelSubprocess moves itself to its own QThread in its
        // ctor, and we'd otherwise hit Qt's "Cannot create children for a
        // parent that is in a different thread" when createDeviceInstance
        // is called from a TCP worker thread (which is most of them).
        // Lifetime is manually managed via m_panelProcs + delete in
        // destroyDeviceInstance.
        auto *proc = new PanelSubprocess(nullptr);
        proc->setPythonExe(m_subprocessPythonExe);
        proc->setWorkerScript(m_workerScript);
        // Qt::QueuedConnection because the signal fires on PanelSubprocess's
        // worker thread; the slot mutates state owned by PythonBridge (logs)
        // so it must hop over to PythonBridge's thread.
        connect(proc, &PanelSubprocess::logMessage, this,
                [this, instanceId](const QString &line) {
                    emit logMessage(QString("[Instance %1] %2").arg(instanceId).arg(line));
                },
                Qt::QueuedConnection);

        if (!proc->spawn(10000)) {
            emit logMessage(QString("[Instance %1] subprocess spawn failed").arg(instanceId));
            delete proc;
            m_nextInstanceId--;
            return -1;
        }

        QJsonObject initArgs{
            {"cornea_index",       deviceIndex},
            {"hardware_variant",   hardwareVariant},
            {"cal_path",           m_calPath},
            {"init_cornea",        true},
            {"init_rj1",           true},
            {"allow_default_hdf5", m_allowDefaultHdf5},
            {"spi_clk_freq",       m_spiClkFreq},
        };
        // 60 s is generous for a cold cornea_rax720 ctor (typical 5–30 s
        // including FTDI enum + RJ1 init + cal file load).
        const QJsonObject reply = proc->sendBlocking("init", initArgs, 60000);
        if (!reply.value("success").toBool()) {
            emit logMessage(QString("[Instance %1] init failed: %2")
                                .arg(instanceId).arg(reply.value("error").toString()));
            proc->stop(3000);
            delete proc;
            m_nextInstanceId--;
            return -1;
        }

        const QJsonObject data = reply.value("data").toObject();
        const bool initOk = data.value("init_ok").toBool();
        const QString serial = data.value("cornea_serial").toString();
        {
            QMutexLocker lock(&m_panelProcsMutex);
            m_panelProcs.insert(instanceId, proc);
        }
        m_initOkMap[instanceId] = initOk;
        m_serialMap[instanceId] = serial;
        emit logMessage(QString("[Instance %1] subprocess ready (pid=%2, serial=%3, init_ok=%4, %5 ms)")
                            .arg(instanceId).arg(proc->processId()).arg(serial)
                            .arg(initOk).arg(data.value("duration_ms").toInt()));
        return instanceId;
    }

    return dispatch([this, deviceIndex, hardwareVariant]() -> int {
        if (!m_initialized) {
            setError("Python bridge not initialized");
            return -1;
        }

        int instanceId = m_nextInstanceId++;

        emit logMessage(QString("[Instance %1] Creating CorneaRax720 for device index %2 (variant: %3)...")
                        .arg(instanceId).arg(deviceIndex).arg(hardwareVariant));

        PyObject *kwargs = PyDict_New();
        PyDict_SetItemString(kwargs, "cornea_index", PyLong_FromLong(deviceIndex));
        PyDict_SetItemString(kwargs, "init_cornea", Py_True);
        PyDict_SetItemString(kwargs, "init_rj1", Py_True);
        PyDict_SetItemString(kwargs, "cal_path", PyUnicode_FromString(m_calPath.toUtf8().constData()));
        PyDict_SetItemString(kwargs, "rj1_use_i2c", Py_True);
        PyDict_SetItemString(kwargs, "rj1_use_spi", Py_True);
        PyDict_SetItemString(kwargs, "allow_default_hdf5", m_allowDefaultHdf5 ? Py_True : Py_False);
        PyDict_SetItemString(kwargs, "cal_revision", Py_None);
        PyDict_SetItemString(kwargs, "cornea_serial", Py_None);
        PyDict_SetItemString(kwargs, "rj1_version", Py_None);
        PyDict_SetItemString(kwargs, "spi_clk_freq", PyFloat_FromDouble(m_spiClkFreq));
        emit logMessage(QString("[Instance %1] SPI clock freq: %2 Hz (%3 MHz)")
                        .arg(instanceId).arg(m_spiClkFreq, 0, 'f', 0).arg(m_spiClkFreq / 1e6, 0, 'f', 1));
        PyDict_SetItemString(kwargs, "console_log_level", PyLong_FromLong(20));
        PyDict_SetItemString(kwargs, "hardware_variant", PyUnicode_FromString(hardwareVariant.toUtf8().constData()));

        PyObject *instance = nullptr;

        if (m_executor) {
            // Submit to ThreadPoolExecutor for parallel execution.
            // executor.submit(CorneaRax720, **kwargs)
            PyObject *submitMethod = PyObject_GetAttrString(m_executor, "submit");
            PyObject *submitArgs = PyTuple_Pack(1, m_corneaClass);
            PyObject *future = PyObject_Call(submitMethod, submitArgs, kwargs);
            Py_DECREF(submitArgs);
            Py_DECREF(submitMethod);
            Py_DECREF(kwargs);

            if (!future) {
                setError(QString("Failed to submit device %1 to executor").arg(deviceIndex));
                PyErr_Print();
                m_nextInstanceId--;
                return -1;
            }

            // Poll until the future completes.
            // Between polls: release GIL (let executor threads run Python)
            // then re-acquire and processEvents (let other dispatches in).
            while (true) {
                PyObject *done = PyObject_CallMethod(future, "done", nullptr);
                bool isDone = done && PyObject_IsTrue(done);
                Py_XDECREF(done);
                if (isDone) break;

                // Release GIL so executor threads can run Python code
                Py_BEGIN_ALLOW_THREADS
                QThread::msleep(10);
                Py_END_ALLOW_THREADS

                // GIL held again: process queued dispatches from other panels
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }

            // Get result (GIL held)
            instance = PyObject_CallMethod(future, "result", nullptr);
            Py_DECREF(future);
        } else {
            // Fallback: direct call (serial, no parallelism)
            PyObject *args = PyTuple_New(0);
            instance = PyObject_Call(m_corneaClass, args, kwargs);
            Py_DECREF(args);
            Py_DECREF(kwargs);
        }

        if (!instance) {
            // Capture the actual Python exception details
            QString pyError;
            PyObject *pType, *pValue, *pTraceback;
            PyErr_Fetch(&pType, &pValue, &pTraceback);
            if (pValue) {
                PyObject *pStr = PyObject_Str(pValue);
                if (pStr && PyUnicode_Check(pStr)) {
                    pyError = QString::fromUtf8(PyUnicode_AsUTF8(pStr));
                }
                Py_XDECREF(pStr);
            }
            QString typeName;
            if (pType) {
                PyObject *tName = PyObject_GetAttrString(pType, "__name__");
                if (tName && PyUnicode_Check(tName)) {
                    typeName = QString::fromUtf8(PyUnicode_AsUTF8(tName));
                }
                Py_XDECREF(tName);
            }
            Py_XDECREF(pType);
            Py_XDECREF(pValue);
            Py_XDECREF(pTraceback);

            QString fullError = QString("Failed to create device %1: [%2] %3")
                                    .arg(deviceIndex).arg(typeName, pyError);
            setError(fullError);
            emit logMessage(QString("[Instance %1] FAILED: %2").arg(instanceId).arg(fullError));
            return -1;
        }

        PyObject *initOk = PyObject_GetAttrString(instance, "init_ok");
        bool initOkValue = initOk && PyObject_IsTrue(initOk);
        Py_XDECREF(initOk);

        if (!initOkValue) {
            // Try to extract UCID even if init failed — panel might have been detected
            QString ucid;
            PyObject *stateVals = PyObject_GetAttrString(instance, "state_vals");
            if (stateVals && PyDict_Check(stateVals)) {
                PyObject *ucidObj = PyDict_GetItemString(stateVals, "unique_chip_id_str");
                if (ucidObj && PyUnicode_Check(ucidObj)) {
                    ucid = QString::fromUtf8(PyUnicode_AsUTF8(ucidObj));
                }
            }
            Py_XDECREF(stateVals);

            QString detail;
            if (!ucid.isEmpty()) {
                detail = QString("Hardware initialization failed for device %1.\n"
                                 "Panel detected: UCID = %2\n"
                                 "Please provide the HDF5 calibration file for this UCID and place it in the cal_path folder.")
                         .arg(deviceIndex).arg(ucid);
            } else {
                detail = QString("Hardware initialization failed for device %1 (init_ok=False)").arg(deviceIndex);
            }
            setError(detail);
            emit logMessage(QString("[Instance %1] FAILED: %2").arg(instanceId).arg(detail));
            Py_DECREF(instance);
            return -1;
        }

        QString serial;
        PyObject *serialObj = PyObject_GetAttrString(instance, "cornea_serial");
        if (serialObj && PyUnicode_Check(serialObj)) {
            serial = QString::fromUtf8(PyUnicode_AsUTF8(serialObj));
        }
        Py_XDECREF(serialObj);

        m_deviceInstances[instanceId] = instance;
        m_initOkMap[instanceId] = true;
        m_serialMap[instanceId] = serial;

        emit logMessage(QString("[Instance %1] Connected to device %2 (Serial: %3)")
                        .arg(instanceId).arg(deviceIndex).arg(serial));

        // Spin up a dedicated worker thread for this instance so subsequent
        // per-device calls (sendImage / setBrightness / getTemperature) run
        // concurrently with other panels.
        ensureDeviceWorker(instanceId);

        return instanceId;
    });
}

int PythonBridge::preInitDeviceInstance(int deviceIndex, const QString &hardwareVariant)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return createDeviceInstance(deviceIndex, hardwareVariant);
#endif
    return dispatch([this, deviceIndex, hardwareVariant]() -> int {
        if (!m_initialized) {
            setError("Python bridge not initialized");
            return -1;
        }

        int instanceId = m_nextInstanceId++;

        emit logMessage(QString("[Instance %1] Pre-init: FTDI-only for device index %2 (init_rj1=False)...")
                        .arg(instanceId).arg(deviceIndex));

        PyObject *kwargs = PyDict_New();
        PyDict_SetItemString(kwargs, "cornea_index", PyLong_FromLong(deviceIndex));
        PyDict_SetItemString(kwargs, "init_cornea", Py_True);
        PyDict_SetItemString(kwargs, "init_rj1", Py_False);  // Skip RJ1 — no panel needed
        PyDict_SetItemString(kwargs, "cal_path", PyUnicode_FromString(m_calPath.toUtf8().constData()));
        PyDict_SetItemString(kwargs, "rj1_use_i2c", Py_True);
        PyDict_SetItemString(kwargs, "rj1_use_spi", Py_True);
        PyDict_SetItemString(kwargs, "allow_default_hdf5", Py_True);
        PyDict_SetItemString(kwargs, "cal_revision", Py_None);
        PyDict_SetItemString(kwargs, "cornea_serial", Py_None);
        PyDict_SetItemString(kwargs, "rj1_version", Py_None);
        PyDict_SetItemString(kwargs, "spi_clk_freq", PyFloat_FromDouble(m_spiClkFreq));
        PyDict_SetItemString(kwargs, "console_log_level", PyLong_FromLong(20));
        PyDict_SetItemString(kwargs, "hardware_variant", PyUnicode_FromString(hardwareVariant.toUtf8().constData()));

        PyObject *args = PyTuple_New(0);
        PyObject *instance = PyObject_Call(m_corneaClass, args, kwargs);
        Py_DECREF(args);
        Py_DECREF(kwargs);

        if (!instance) {
            PyErr_Print();
            emit logMessage(QString("[Instance %1] Pre-init FAILED").arg(instanceId));
            m_nextInstanceId--;
            return -1;
        }

        QString serial;
        PyObject *serialObj = PyObject_GetAttrString(instance, "cornea_serial");
        if (serialObj && PyUnicode_Check(serialObj)) {
            serial = QString::fromUtf8(PyUnicode_AsUTF8(serialObj));
        }
        Py_XDECREF(serialObj);

        m_deviceInstances[instanceId] = instance;
        m_initOkMap[instanceId] = false;  // Not fully initialized yet
        m_serialMap[instanceId] = serial;

        emit logMessage(QString("[Instance %1] Pre-init OK: device %2 (Serial: %3) — FTDI ready, awaiting powerOn")
                        .arg(instanceId).arg(deviceIndex).arg(serial));

        // Spin up a dedicated worker thread for this instance (same reason
        // as in createDeviceInstance).
        ensureDeviceWorker(instanceId);

        return instanceId;
    });
}

void PythonBridge::destroyDeviceInstance(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    m_simDevices.remove(instanceId);
    m_initOkMap.remove(instanceId);
    m_serialMap.remove(instanceId);
    emit logMessage(QString("[SIM] Device instance %1 destroyed").arg(instanceId));
    return;
#endif

    // Subprocess path: send shutdown, wait, kill on hang. Python's
    // process exit handles cornea_rax720 instance teardown (rax_lib's
    // __del__ runs on python.exe shutdown), and the OS releases the
    // FT4232 USB claim when the process dies. Much simpler than the
    // embedded-Python teardown below.
    if (m_useSubprocess) {
        PanelSubprocess *proc = nullptr;
        {
            QMutexLocker lock(&m_panelProcsMutex);
            proc = m_panelProcs.take(instanceId);
        }
        m_initOkMap.remove(instanceId);
        const QString serial = m_serialMap.take(instanceId);
        if (proc) {
            // Power off the panel hardware BEFORE killing the worker
            // process. Without this the rails stay up — the rax_lib
            // __del__ that would normally do power-down doesn't run
            // reliably on process exit, so we have to do it explicitly
            // via JSON-RPC while the subprocess is still alive.
            // Bounded short timeout — if powerOff hangs we still want
            // shutdown to finish, the next CC launch will re-init.
            const QJsonObject offReply = proc->sendBlocking("powerOff", 5000);
            if (!offReply.value("success").toBool()) {
                emit logMessage(QString("[Instance %1] powerOff during teardown "
                                        "failed (serial=%2): %3 — panel rails may "
                                        "stay up until the next powerOn cycle")
                                    .arg(instanceId).arg(serial)
                                    .arg(offReply.value("error").toString()));
            }
            proc->stop(5000);
            // Plain delete (NOT deleteLater): deleteLater posts the deletion
            // event to proc's home thread (its own worker thread). The
            // destructor then calls m_workerThread.wait() — which would
            // deadlock waiting for itself to exit. Synchronous delete on
            // the caller's thread avoids that; the ~PanelSubprocess runs
            // here and its m_workerThread is a foreign thread it can wait on.
            delete proc;
            emit logMessage(QString("[Instance %1] subprocess stopped (serial=%2)")
                                .arg(instanceId).arg(serial));
        }
        return;
    }

    // Run the Python teardown on the device's own worker thread if it has
    // one (ensures no other call is mid-flight on this instance). Falls
    // back to the main worker via dispatchVoid if no device worker exists.
    auto teardown = [this, instanceId]() {
        if (!m_deviceInstances.contains(instanceId)) {
            return;
        }

        PyObject *instance = m_deviceInstances.take(instanceId);
        m_initOkMap.remove(instanceId);
        QString serial = m_serialMap.take(instanceId);

        PyObject *method = PyObject_GetAttrString(instance, "system_power_off");
        if (method) {
            PyObject *result = PyObject_CallNoArgs(method);
            Py_XDECREF(result);
            Py_DECREF(method);
        }
        PyErr_Clear();

        Py_DECREF(instance);
        emit logMessage(QString("[Instance %1] Disconnected (Serial: %2)").arg(instanceId).arg(serial));
    };

    bool hasDeviceWorker;
    {
        QMutexLocker lock(&m_deviceThreadsMutex);
        hasDeviceWorker = m_deviceWorkers.contains(instanceId);
    }
    if (hasDeviceWorker) {
        dispatchVoidToDevice(instanceId, teardown);
        // Now that Python state is released, stop and delete the worker.
        destroyDeviceWorker(instanceId);
    } else {
        dispatchVoid(teardown);
    }
}

bool PythonBridge::isDeviceConnected(int instanceId) const
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return m_simDevices.contains(instanceId) && m_simDevices[instanceId].connected;
#endif
    if (m_useSubprocess) {
        QMutexLocker lock(&m_panelProcsMutex);
        PanelSubprocess *proc = m_panelProcs.value(instanceId, nullptr);
        return proc != nullptr && proc->isRunning();
    }
    // Map is only modified on Python thread, reads from main thread are safe
    // for QMap (non-concurrent reads are OK if no concurrent write).
    // Since callers typically check this before dispatching Python work,
    // and modifications happen sequentially on the Python thread, this is safe.
    bool result = m_deviceInstances.contains(instanceId);
    if (!result) {
        qDebug() << "PythonBridge::isDeviceConnected: instanceId=" << instanceId
                 << "not found, available instances:" << m_deviceInstances.keys();
    }
    return result;
}

bool PythonBridge::isDeviceInitOk(int instanceId) const
{
    return m_initOkMap.value(instanceId, false);
}

QString PythonBridge::getDeviceSerial(int instanceId) const
{
    return m_serialMap.value(instanceId, QString());
}

bool PythonBridge::systemPowerOn(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    emit logMessage(QString("[SIM] systemPowerOn(%1)").arg(instanceId));
    simStallSleep(SIM_DELAY_MS);
    if (!m_simDevices.contains(instanceId)) return false;
    // Restore connected=true so subsequent setBrightness / sendImage /
    // getPanelId hit their simStallSleep instead of fail-fast at the
    // isConnected() gate in CorneaWidget::setBrightnessBySerial / etc.
    m_simDevices[instanceId].connected = true;
    return true;
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "powerOn", {}, 60000);
        return reply.value("success").toBool()
               && reply.value("data").toObject().value("init_ok").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "system_power_on");
        if (!result) {
            emit logMessage(QString("[Instance %1] System power ON threw exception: %2")
                                .arg(instanceId).arg(m_lastError));
            flushPythonOutput();
            return false;
        }
        // rax_lib's system_power_on returns init_ok (bool): True only if the
        // init sequence completed successfully. When the panel isn't physically
        // connected (Pogo not engaged), init_cornea NACKs internally but the
        // call returns False rather than throwing — so checking only "did it
        // throw?" is not enough. Without this PyObject_IsTrue check we'd
        // misreport "Power ON" with m_poweredOn=true and the UI would light up
        // green even though no panel was connected. Surfaced 2026-05-10 on a
        // station where right-eye Pogo was unseated; v1.1.12 reported "lit".
        const bool initOk = PyObject_IsTrue(result) == 1;
        Py_DECREF(result);
        if (initOk) {
            emit logMessage(QString("[Instance %1] System power ON").arg(instanceId));
            return true;
        }
        emit logMessage(QString("[Instance %1] System power ON returned init_ok=False (panel not responding — check Pogo connection / hardware)")
                            .arg(instanceId));
        flushPythonOutput();
        return false;
    });
}

bool PythonBridge::systemPowerOff(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    emit logMessage(QString("[SIM] systemPowerOff(%1)").arg(instanceId));
    simStallSleep(SIM_DELAY_MS);
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].connected = false;
    return true;
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "powerOff", {}, 30000);
        return reply.value("success").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "system_power_off");
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] System power OFF").arg(instanceId));
            return true;
        }
        emit logMessage(QString("[Instance %1] System power OFF FAILED: %2").arg(instanceId).arg(m_lastError));
        flushPythonOutput();
        return false;
    });
}

bool PythonBridge::enableVsys(int instanceId, bool enable)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(enable);
    return m_simDevices.contains(instanceId);
#endif
    return dispatchToDevice(instanceId, [this, instanceId, enable]() -> bool {
        PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
        PyObject *result = callInstanceMethod(instanceId, "enable_vsys", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] VSYS %2").arg(instanceId).arg(enable ? "enabled" : "disabled"));
            return true;
        }
        return false;
    });
}

bool PythonBridge::setBrightness(int instanceId, double level)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    simStallSleep(SIM_DELAY_MS);
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].brightness = level;
    return m_simDevices.contains(instanceId);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "setBrightness",
                                                  QJsonObject{{"level", level}}, 10000);
        return reply.value("success").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId, level]() -> bool {
        PyObject *args = PyTuple_Pack(1, PyFloat_FromDouble(level));
        PyObject *result = callInstanceMethod(instanceId, "set_brightness", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] Brightness set to %2").arg(instanceId).arg(level));
            return true;
        }
        return false;
    });
}

double PythonBridge::getBrightness(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return m_simDevices.contains(instanceId) ? m_simDevices[instanceId].brightness : -1.0;
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getBrightness", {}, 10000);
        if (!reply.value("success").toBool()) return -1.0;
        return reply.value("data").toObject().value("level").toDouble(-1.0);
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> double {
        PyObject *result = callInstanceMethod(instanceId, "get_brightness");
        if (result) {
            double value = PyFloat_AsDouble(result);
            Py_DECREF(result);
            return value;
        }
        return -1.0;
    });
}

bool PythonBridge::setXFlip(int instanceId, bool flip)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].xFlip = flip;
    return m_simDevices.contains(instanceId);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "setXFlip",
                                                  QJsonObject{{"flip", flip}}, 10000);
        return reply.value("success").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId, flip]() -> bool {
        PyObject *args = PyTuple_Pack(2, flip ? Py_True : Py_False, Py_None);
        PyObject *result = callInstanceMethod(instanceId, "rj1_set_x_flip_offset", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] X-Flip set to %2").arg(instanceId).arg(flip ? "true" : "false"));
            return true;
        }
        return false;
    });
}

bool PythonBridge::setYFlip(int instanceId, bool flip)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    if (m_simDevices.contains(instanceId)) m_simDevices[instanceId].yFlip = flip;
    return m_simDevices.contains(instanceId);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "setYFlip",
                                                  QJsonObject{{"flip", flip}}, 10000);
        return reply.value("success").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId, flip]() -> bool {
        PyObject *args = PyTuple_Pack(2, flip ? Py_True : Py_False, Py_None);
        PyObject *result = callInstanceMethod(instanceId, "rj1_set_y_flip_offset", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            emit logMessage(QString("[Instance %1] Y-Flip set to %2").arg(instanceId).arg(flip ? "true" : "false"));
            return true;
        }
        return false;
    });
}

bool PythonBridge::getXFlip(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return m_simDevices.contains(instanceId) ? m_simDevices[instanceId].xFlip : false;
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getXFlip", {}, 10000);
        if (!reply.value("success").toBool()) return false;
        return reply.value("data").toObject().value("flip").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "rj1_get_x_flip_offset");
        if (result && PyTuple_Check(result) && PyTuple_Size(result) >= 1) {
            PyObject *flipObj = PyTuple_GetItem(result, 0);
            bool value = PyObject_IsTrue(flipObj);
            Py_DECREF(result);
            return value;
        }
        Py_XDECREF(result);
        return false;
    });
}

bool PythonBridge::getYFlip(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return m_simDevices.contains(instanceId) ? m_simDevices[instanceId].yFlip : false;
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getYFlip", {}, 10000);
        if (!reply.value("success").toBool()) return false;
        return reply.value("data").toObject().value("flip").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> bool {
        PyObject *result = callInstanceMethod(instanceId, "rj1_get_y_flip_offset");
        if (result && PyTuple_Check(result) && PyTuple_Size(result) >= 1) {
            PyObject *flipObj = PyTuple_GetItem(result, 0);
            bool value = PyObject_IsTrue(flipObj);
            Py_DECREF(result);
            return value;
        }
        Py_XDECREF(result);
        return false;
    });
}

bool PythonBridge::writeFrame(int instanceId, const QImage &image)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(image);
    emit logMessage(QString("[SIM] writeFrame(%1) %2x%3").arg(instanceId).arg(image.width()).arg(image.height()));
    simStallSleep(SIM_DELAY_MS);
    return m_simDevices.contains(instanceId);
#endif
    if (m_useSubprocess) {
        // Save the image to a temp PNG and pass the path. Phase 2 keeps
        // this file-based to avoid encoding raw RGB bytes into JSON;
        // Phase 3 can add a stdin binary side-channel for higher
        // throughput if needed.
        const QString tmpPath = QDir::tempPath()
            + QString("/cc_frame_inst%1.png").arg(instanceId);
        if (!image.save(tmpPath, "PNG")) {
            setError(QString("writeFrame: failed to save temp image to %1").arg(tmpPath));
            return false;
        }
        const QJsonObject reply = subprocessCall(
            instanceId, "sendImage", QJsonObject{{"path", tmpPath}}, 30000);
        return reply.value("success").toBool();
    }
    return dispatchToDevice(instanceId, [this, instanceId, image]() -> bool {
        PyObject *instance = getDeviceInstance(instanceId);
        if (!instance) {
            setError(QString("Device instance %1 not found").arg(instanceId));
            return false;
        }

        PyObject *pyArray = qimageToPyArray(image);
        if (!pyArray) {
            return false;
        }

        PyObject *method = PyObject_GetAttrString(instance, "write_rj1_frame");
        if (!method) {
            setError(QString("write_rj1_frame not found on instance %1").arg(instanceId));
            PyErr_Clear();
            Py_DECREF(pyArray);
            return false;
        }

        PyObject *kwargs = PyDict_New();
        PyDict_SetItemString(kwargs, "opencv_frame", Py_True);
        // rax_lib's write_rj1_frame logs an APL line on every frame by default.
        // Configurable via cornea_config.json's python.log_apl (default false).
        // Default off = quieter logs + less per-frame overhead. Flip on when
        // debugging APL / brightness issues. Recommended by Google FW team
        // when chasing throughput (e.g. validating 30 Mbps SPI clock).
        PyDict_SetItemString(kwargs, "log_apl", m_logApl ? Py_True : Py_False);

        PyObject *args = PyTuple_Pack(1, pyArray);

        // Time the actual rax_lib call in isolation. Google FW team's spec is
        // <1 s for write_rj1_frame on a healthy panel; if our integrated path
        // logs longer than that, the extra time is upstream of this call
        // (image conversion, kwargs build, GIL contention, dispatch hop —
        // already measured outside this bracket). qimageToPyArray runs above;
        // that's the suspect for any C++→Python conversion overhead.
        QElapsedTimer rax_call_timer;
        rax_call_timer.start();
        PyObject *result = PyObject_Call(method, args, kwargs);
        const qint64 rax_call_ms = rax_call_timer.elapsed();

        Py_DECREF(method);
        Py_DECREF(args);
        Py_DECREF(kwargs);
        Py_DECREF(pyArray);

        if (result) {
            bool success = PyObject_IsTrue(result);
            Py_DECREF(result);
            if (success) {
                emit logMessage(QString("[Instance %1] Frame written successfully (rax write_rj1_frame: %2 ms)")
                                    .arg(instanceId).arg(rax_call_ms));
            } else {
                emit logMessage(QString("[Instance %1] Frame write returned False (rax write_rj1_frame: %2 ms)")
                                    .arg(instanceId).arg(rax_call_ms));
            }
            return success;
        }

        emit logMessage(QString("[Instance %1] Frame write threw (rax write_rj1_frame: %2 ms before throw)")
                            .arg(instanceId).arg(rax_call_ms));
        PyErr_Print();
        return false;
    });
}

bool PythonBridge::writeFrameFromPath(int instanceId, const QString &imagePath)
{
    QImage image(imagePath);
    if (image.isNull()) {
        setError(QString("Failed to load image: %1").arg(imagePath));
        return false;
    }
    return writeFrame(instanceId, image);
}

QVariantMap PythonBridge::getChipInfo(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    QVariantMap info;
    info["chip"] = "SIM_RJ1B1";
    info["revision"] = "B1";
    info["instance"] = instanceId;
    return info;
#endif
    return dispatchToDevice(instanceId, [this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_chip_info");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QVariantMap PythonBridge::getChipInfoDecoded(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return getChipInfo(instanceId);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getChipInfoDecoded", {}, 10000);
        QVariantMap out;
        if (!reply.value("success").toBool()) return out;
        const QJsonObject info = reply.value("data").toObject().value("info").toObject();
        for (auto it = info.begin(); it != info.end(); ++it) {
            out.insert(it.key(), it.value().toVariant());
        }
        return out;
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QString PythonBridge::getChipRevision(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    return "B1";
#endif
    return dispatchToDevice(instanceId, [this, instanceId]() -> QString {
        PyObject *result = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
        if (result && PyDict_Check(result)) {
            PyObject *revision = PyDict_GetItemString(result, "revision");
            if (revision) {
                QString rev;
                if (PyLong_Check(revision)) {
                    rev = QString("B%1").arg(PyLong_AsLong(revision));
                } else if (PyUnicode_Check(revision)) {
                    rev = QString::fromUtf8(PyUnicode_AsUTF8(revision));
                }
                Py_DECREF(result);
                if (!rev.isEmpty()) {
                    return rev;
                }
            }
            Py_DECREF(result);
        }
        Py_XDECREF(result);
        PyErr_Clear();
        return QString();
    });
}

double PythonBridge::getLeaTemperature(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    simStallSleep(SIM_DELAY_MS);
    return 25.0 + QRandomGenerator::global()->bounded(5.0);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getTemperature", {}, 10000);
        if (!reply.value("success").toBool()) return -999.0;
        return reply.value("data").toObject().value("temperature").toDouble(-999.0);
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> double {
        PyObject *instance = getDeviceInstance(instanceId);
        if (!instance) {
            return -999.0;
        }

        PyObject *method = PyObject_GetAttrString(instance, "get_lea_temperature");
        if (!method) {
            PyErr_Clear();
            return -999.0;
        }

        PyObject *kwargs = PyDict_New();
        PyDict_SetItemString(kwargs, "sensor_sel", PyUnicode_FromString("rax"));

        PyObject *args = PyTuple_New(0);
        PyObject *result = PyObject_Call(method, args, kwargs);

        Py_DECREF(method);
        Py_DECREF(args);
        Py_DECREF(kwargs);

        if (result && result != Py_None) {
            double temp = -999.0;
            if (PyFloat_Check(result)) {
                temp = PyFloat_AsDouble(result);
            } else if (PyLong_Check(result)) {
                temp = static_cast<double>(PyLong_AsLong(result));
            }
            Py_DECREF(result);
            return temp;
        }
        Py_XDECREF(result);
        PyErr_Clear();
        return -999.0;
    });
}

double PythonBridge::getDa9272Temperature(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    return 24.0 + QRandomGenerator::global()->bounded(4.0);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getDa9272Temperature", {}, 10000);
        if (!reply.value("success").toBool()) return -999.0;
        return reply.value("data").toObject().value("temperature").toDouble(-999.0);
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> double {
        PyObject *result = callInstanceMethod(instanceId, "get_da9272_temperature");
        if (result && result != Py_None) {
            double temp = -999.0;
            if (PyFloat_Check(result)) {
                temp = PyFloat_AsDouble(result);
            } else if (PyLong_Check(result)) {
                temp = static_cast<double>(PyLong_AsLong(result));
            }
            Py_DECREF(result);
            return temp;
        }
        Py_XDECREF(result);
        PyErr_Clear();
        return -999.0;
    });
}

QVariantMap PythonBridge::getPowerMeasurements(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    QVariantMap v;
    v["vsys_power_mw"] = 500.0 + QRandomGenerator::global()->bounded(100.0);
    v["vddio_power_mw"] = 50.0 + QRandomGenerator::global()->bounded(20.0);
    return v;
#endif
    return dispatchToDevice(instanceId, [this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *instance = getDeviceInstance(instanceId);
        if (!instance) return result;

        // Python equivalent of:
        //   vsys_power_mw  = cornea_ctrl.pwr["vsys"].get_power() * 1000
        //   vddio_power_mw = cornea_ctrl.pwr["vddio"].get_power() * 1000
        auto readPower = [&](const char *railName, double &outMw) -> bool {
            PyObject *pwr = PyObject_GetAttrString(instance, "pwr");
            if (!pwr) { PyErr_Clear(); return false; }
            PyObject *rail = PyObject_GetItem(pwr, PyUnicode_FromString(railName));
            Py_DECREF(pwr);
            if (!rail) { PyErr_Clear(); return false; }
            PyObject *result = PyObject_CallMethod(rail, "get_power", nullptr);
            Py_DECREF(rail);
            if (!result) { PyErr_Clear(); return false; }
            if (PyFloat_Check(result)) {
                outMw = PyFloat_AsDouble(result) * 1000.0; // W → mW
            } else if (PyLong_Check(result)) {
                outMw = static_cast<double>(PyLong_AsLong(result)) * 1000.0;
            }
            Py_DECREF(result);
            return true;
        };

        double vsysMw = -999.0, vddioMw = -999.0;
        readPower("vsys", vsysMw);
        readPower("vddio", vddioMw);
        result["vsys_power_mw"] = vsysMw;
        result["vddio_power_mw"] = vddioMw;
        return result;
    });
}

QVariantMap PythonBridge::getPackageVersions(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    QVariantMap v;
    v["rj1lib"] = "1.0.0-sim";
    v["ar_display_lab_lib"] = "1.0.0-sim";
    return v;
#endif
    return dispatchToDevice(instanceId, [this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_pkg_versions");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QString PythonBridge::getPanelId(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    simStallSleep(SIM_DELAY_MS);
    return QString("SIM-PANEL-%1").arg(instanceId);
#endif
    if (m_useSubprocess) {
        const QJsonObject reply = subprocessCall(instanceId, "getPanelId", {}, 10000);
        if (!reply.value("success").toBool()) return QString();
        return reply.value("data").toObject().value("panel_id").toString();
    }
    return dispatchToDevice(instanceId, [this, instanceId]() -> QString {
        PyObject *result = callInstanceMethod(instanceId, "get_rj1_chip_info_decoded");
        if (result && PyDict_Check(result)) {
            PyObject *ucidStr = PyDict_GetItemString(result, "unique_chip_id_str");
            if (ucidStr && PyUnicode_Check(ucidStr)) {
                QString panelId = QString::fromUtf8(PyUnicode_AsUTF8(ucidStr));
                Py_DECREF(result);
                return panelId;
            }
            Py_DECREF(result);
        }
        Py_XDECREF(result);
        PyErr_Clear();
        return QString();
    });
}

int PythonBridge::readRj1Register(int instanceId, int address)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId); Q_UNUSED(address);
    return 0;
#endif
    return dispatchToDevice(instanceId, [this, instanceId, address]() -> int {
        PyObject *args = PyTuple_Pack(1, PyLong_FromLong(address));
        PyObject *result = callInstanceMethod(instanceId, "read_rj1_reg", args);
        Py_DECREF(args);
        if (result) {
            int value = static_cast<int>(PyLong_AsLong(result));
            Py_DECREF(result);
            return value;
        }
        return -1;
    });
}

bool PythonBridge::writeRj1Register(int instanceId, int address, int data)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId); Q_UNUSED(address); Q_UNUSED(data);
    return true;
#endif
    return dispatchToDevice(instanceId, [this, instanceId, address, data]() -> bool {
        PyObject *args = PyTuple_Pack(2, PyLong_FromLong(address), PyLong_FromLong(data));
        PyObject *result = callInstanceMethod(instanceId, "write_rj1_reg", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

QVariantMap PythonBridge::getRj1Dacs(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    return QVariantMap();
#endif
    return dispatchToDevice(instanceId, [this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "get_rj1_dacs");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

bool PythonBridge::setRj1Dacs(int instanceId, const QVariantMap &dacValues)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId); Q_UNUSED(dacValues);
    return true;
#endif
    return dispatchToDevice(instanceId, [this, instanceId, dacValues]() -> bool {
        PyObject *pyDict = PyDict_New();
        for (auto it = dacValues.begin(); it != dacValues.end(); ++it) {
            PyObject *key = PyUnicode_FromString(it.key().toUtf8().constData());
            PyObject *value = variantToPyObject(it.value());
            PyDict_SetItem(pyDict, key, value);
            Py_DECREF(key);
            Py_DECREF(value);
        }

        PyObject *args = PyTuple_Pack(1, pyDict);
        PyObject *result = callInstanceMethod(instanceId, "set_rj1_dacs", args);
        Py_DECREF(args);
        Py_DECREF(pyDict);

        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

bool PythonBridge::setDemuraEnable(int instanceId, bool enable)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId); Q_UNUSED(enable);
    return true;
#endif
    return dispatchToDevice(instanceId, [this, instanceId, enable]() -> bool {
        PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
        PyObject *result = callInstanceMethod(instanceId, "rj1_demura_enable", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

bool PythonBridge::setRlutEnable(int instanceId, bool enable)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId); Q_UNUSED(enable);
    return true;
#endif
    return dispatchToDevice(instanceId, [this, instanceId, enable]() -> bool {
        PyObject *args = PyTuple_Pack(1, enable ? Py_True : Py_False);
        PyObject *result = callInstanceMethod(instanceId, "rj1_rlut_enable", args);
        Py_DECREF(args);
        if (result) {
            Py_DECREF(result);
            return true;
        }
        return false;
    });
}

QVariantMap PythonBridge::getDemuraRlutState(int instanceId)
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    Q_UNUSED(instanceId);
    QVariantMap state;
    state["demura"] = true;
    state["rlut"] = true;
    return state;
#endif
    return dispatchToDevice(instanceId, [this, instanceId]() -> QVariantMap {
        QVariantMap result;
        PyObject *pyResult = callInstanceMethod(instanceId, "rj1_get_demura_rlut_state");
        if (pyResult) {
            result = pyObjectToVariant(pyResult).toMap();
            Py_DECREF(pyResult);
        }
        return result;
    });
}

QJsonObject PythonBridge::subprocessCall(int instanceId, const QString &cmd,
                                          const QJsonObject &args, int timeoutMs)
{
    // Per-instance route. The per-device worker thread serializes calls
    // for one panel; PanelSubprocess itself isn't internally locked, but
    // the single-thread-per-panel guarantee from caller side is enough.
    PanelSubprocess *proc = nullptr;
    {
        QMutexLocker lock(&m_panelProcsMutex);
        proc = m_panelProcs.value(instanceId, nullptr);
    }
    if (!proc || !proc->isRunning()) {
        QJsonObject err{
            {"success", false},
            {"error", QString("subprocess for instance %1 not running").arg(instanceId)}
        };
        return err;
    }
    return proc->sendBlocking(cmd, args, timeoutMs);
}

void PythonBridge::flushPythonOutput()
{
#if defined(QT_DEBUG) && !defined(DISABLE_SIM)
    return;
#endif
    // Use QueuedConnection (non-blocking) so this never blocks the main thread.
    // When Python worker is busy with a long operation (e.g. powerOn ~10s),
    // BlockingQueuedConnection would freeze the UI until that operation finishes.
    //
    // Since v1.1.6 the interpreter releases the GIL after init
    // (PyEval_SaveThread), so this lambda MUST acquire it via
    // PyGILState_Ensure before touching any Python C API; otherwise it
    // will segfault on the very first 100 ms tick.
    QMetaObject::invokeMethod(m_pythonWorker, [this]() {
        if (!m_initialized) {
            return;
        }

        PyGILState_STATE gstate = PyGILState_Ensure();

        PyObject *mainModule = PyImport_AddModule("__main__");
        if (!mainModule) {
            PyGILState_Release(gstate);
            return;
        }

        PyObject *captureObj = PyObject_GetAttrString(mainModule, "_qt_output_capture");
        if (!captureObj) {
            PyErr_Clear();
            PyGILState_Release(gstate);
            return;
        }

        PyObject *getMethod = PyObject_GetAttrString(captureObj, "get_and_clear");
        if (!getMethod) {
            Py_DECREF(captureObj);
            PyErr_Clear();
            PyGILState_Release(gstate);
            return;
        }

        PyObject *result = PyObject_CallNoArgs(getMethod);
        Py_DECREF(getMethod);
        Py_DECREF(captureObj);

        if (!result) {
            PyErr_Clear();
            PyGILState_Release(gstate);
            return;
        }

        if (PyList_Check(result)) {
            Py_ssize_t size = PyList_Size(result);
            for (Py_ssize_t i = 0; i < size; ++i) {
                PyObject *item = PyList_GetItem(result, i);
                if (PyUnicode_Check(item)) {
                    QString message = QString::fromUtf8(PyUnicode_AsUTF8(item));
                    emit logMessage(message);
                }
            }
        }

        Py_DECREF(result);
        PyGILState_Release(gstate);
    }, Qt::QueuedConnection);
}
