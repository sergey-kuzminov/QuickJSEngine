//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

/**
 * @note JavaScript does not support C++ method overloading by type.
 * If a QObject has overloaded Q_INVOKABLE methods or signals,
 * only the first one encountered will be exposed to JavaScript.
 * Provide unique names for methods intended to be called from JS.
 */

#ifndef QUICKJSENGINE_H
#define QUICKJSENGINE_H

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <type_traits>

#include <quickjs.h>

#include "QuickJSConvert.h"
#include "QuickJSValue.h"

// quickjs-ng only, no Bellard-core compatibility. Requires >= 0.9.0 for
// JS_NewArrayFrom, JS_IsArrayBuffer, JS_GetTypedArrayType and JS_SetDumpFlags.
// Also relies on ng-only API used without gates: JS_ThrowPlainError,
// JS_ToCStringLenUTF16/JS_FreeCStringUTF16, JS_NewStringUTF16,
// JS_IsStrictEqual, JS_Eval2, JS_GetVersion.
#if !defined(QJS_VERSION_MAJOR) || (QJS_VERSION_MAJOR == 0 && QJS_VERSION_MINOR < 9)
#  error "QQuickJSEngine requires quickjs-ng >= 0.9.0"
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
#  error "QQuickJSEngine requires Qt 6.10 or newer"
#endif

constexpr int DynamicPropCacheLimit = 64;
constexpr int MetaCallArgsLimit = 11;
constexpr int DefaultEvalCacheLimit = 8;
constexpr int MaxVariantConversionDepth = 64;
constexpr int AtomNameCacheLimit = 1024;

class JSGenericReceiver;
struct JSQObjectData;
struct MetaCacheEntry;
struct JSMetaValueHolder;
struct JSMetaObjectCache;

struct EvalCacheEntry {
    QString script;
    QString fileName;
    JSValue bytecode;
};

struct JSCommonAtoms {
    JSAtom length = JS_ATOM_NULL;
    JSAtom getTime = JS_ATOM_NULL;
    JSAtom stack = JS_ATOM_NULL;
    JSAtom lineNumber = JS_ATOM_NULL;
    JSAtom line = JS_ATOM_NULL;
};

// A promise rejection reported by the engine tracker as unhandled. Buffered
// until the end of the tick so a .catch() attached later in the same tick can
// retract it (identified by the promise object pointer).
struct PendingRejection {
    JSValue promise;
    JSValue reason;
};

/**
 * @brief The QQuickJSEngine class provides a lightweight wrapper around quickjs-ng.
 *
 * @warning All public slots of exposed QObject instances (including
 * QObject::deleteLater()) are accessible from JavaScript. If you are running
 * untrusted scripts, be aware that scripts can modify or delete the exposed
 * C++ objects. Consider exposing safe wrapper objects instead of raw domain objects.
 */
class QQuickJSEngine : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(QQuickJSEngine)
public:
    friend class QQuickJSValue;
    friend struct JSQObjectData;
    friend class QuickJSQObjectProxy;
    friend class QuickJSSignalProxy;
    friend class QuickJSConvert;

    explicit QQuickJSEngine(QObject* parent = nullptr);
    ~QQuickJSEngine();

    QQuickJSValue evaluate(const QString& script, const QString& fileName = QString());
    QQuickJSValue evaluate(const QString& script, const QString& fileName, int lineNumber);

    void setProperty(const QString& name, const QQuickJSValue& value);
    void setProperty(const QByteArray& name, const QQuickJSValue& value);
    void setProperty(const char* name, const QQuickJSValue& value);
    void setProperty(QStringView name, const QQuickJSValue& value);
    void setProperty(const QString& name, const QVariant& value);
    void setProperty(const char* name, const QVariant& value);
    void setProperty(QStringView name, const QVariant& value);

    template<typename T>
    void setProperty(const QString& name, const T& value) {
        if constexpr (std::is_convertible_v<T, const char*>) {
            setProperty(name, QString::fromUtf8(value));
        } else {
            setProperty(name, QVariant::fromValue(value));
        }
    }
    template<typename T>
    void setProperty(const char* name, const T& value) {
        if constexpr (std::is_convertible_v<T, const char*>) {
            setProperty(name, QString::fromUtf8(value));
        } else {
            setProperty(name, QVariant::fromValue(value));
        }
    }

    /**
     * @brief Wraps a QObject for use in JavaScript.
     * @warning Direct access to properties or methods of a QObject living in a
     * different thread is not thread-safe. Ensure the QObject belongs to the
     * engine's thread, or use it only to pass references to C++ methods that
     * handle thread-safety themselves (e.g., via QMetaObject::invokeMethod).
     */
    QQuickJSValue newQObject(QObject* obj);
    QQuickJSValue newObject();
    QQuickJSValue newArray(uint length = 0);
    QQuickJSValue newFunction(JSCFunction* func, const char* name, int argc);

    JSValue toJSValue(const QVariant& var);
    JSValue toJSValueImpl(const QVariant& var, int depth);
    QVariant toVariant(JSValueConst val);

    // const_cast is safe: QPointer requires a non-const T*, but QQuickJSValue does not modify the engine when owned=false.
    QQuickJSValue globalObject() const;

    enum Extension : quint32 {
        ConsoleExtension = 0x1,
        GarbageCollectionExtension = 0x2,
        TranslationExtension = 0x4,
        AllExtensions = 0xFFFFFFFF
    };
    Q_DECLARE_FLAGS(Extensions, Extension)
    void installExtensions(Extensions extensions = AllExtensions);

    /**
     * @brief Requests an interrupt of the running script.
     *
     * Thread-safe. The flag targets the currently running execution: a new
     * top-level entry point (evaluate(), QQuickJSValue::call(), a signal
     * callback) always starts with a clean state.
     */
    void setInterrupted(bool interrupted);

    /**
     * @brief Sets the maximum execution time for JavaScript scripts.
     * @param ms Timeout in milliseconds. 0 disables the timeout.
     *
     * Thread-safe: the deadline is stored atomically. Enabling the timeout
     * while a script is already running (e.g. from a watchdog thread or a
     * C++ slot invoked by the script) grants a fresh full quota immediately;
     * a stale deadline from a previous run is never used.
     *
     * @note This timeout is cooperative. It triggers between bytecode instructions
     * and during regex evaluation. If a JavaScript function calls a long-blocking
     * C++ method (via Q_INVOKABLE or signals), the timeout cannot interrupt it
     * until the C++ method returns control to the JS engine. To keep the engine
     * responsive, avoid long-blocking operations in exposed C++ methods or check
     * QQuickJSEngine::isInterrupted() periodically inside your C++ loops.
     */
    void setExecutionTimeoutMs(quint32 ms);

    /**
     * @warning These setters must be called from the engine's thread or
     * before any script execution to avoid data races inside QuickJS.
     */
    void setMemoryLimit(size_t limit);
    void setMaxStackSize(size_t size);
    void collectGarbage();
    void setAutoProcessPromises(bool enabled);

    static QString version();
    JSMemoryUsage memoryUsage() const;
    void setGCThreshold(size_t threshold);

    /**
     * @brief Debug: engine dump flags (JS_DUMP_ATOM_LEAKS | JS_DUMP_LEAKS |
     * JS_ABORT_ON_LEAKS etc.). Applied in JS_FreeRuntime: the engine dumps
     * leaked atoms/objects/strings and aborts the process with the ABORT flag.
     */
    void setDumpFlags(quint64 flags);

    void clearEvalCache();
    void setEvalCacheEnabled(bool enabled);
    void setEvalCacheLimit(int limit);

    bool isInterrupted() const { return m_interrupted.load(); }

    QQuickJSValue newErrorObject(QQuickJSValue::ErrorType errorType, QAnyStringView message);

    // Exposed for proxies
    JSValue wrapQObject(QObject* obj);
    void processAndEmitException(JSContext* ctx, JSValueConst exc);
    void processPendingJobs();
    // End-of-tick: report promise rejections that stayed unhandled after the
    // microtask queue drained (Node-like semantics). A no-op when jobs are
    // still pending or when nothing is buffered.
    void flushUnhandledRejections();
    void resetExecutionTimer() {
        ensureEngineThread();
        m_executionDeadlineMs.store(deadlineFromTimeout(m_executionTimeoutMs.load()));
    }
    // Every top-level entry point (evaluate, executeJsCall, signal callbacks)
    // starts with a clean interrupt/deadline state; nested calls never reset.
    void resetExecutionState() {
        ensureEngineThread();
        m_interrupted.store(false);
        resetExecutionTimer();
    }
    JSValue toJSValueFromVoid(void* data, int metaType);

private:
    JSClassExoticMethods m_qobjectExotic{};
    // Deadline in ms since the steady_clock epoch; 0 = no deadline. Atomic so
    // that setExecutionTimeoutMs() from a watchdog thread is race-free.
    std::atomic<qint64> m_executionDeadlineMs{0};
    int m_evalDepth = 0;
    int m_evalCacheLimit = DefaultEvalCacheLimit;
    bool m_evalCacheEnabled = true;

    JSRuntime* m_rt;
    JSContext* m_ctx;
    JSValue m_globalObj;

    JSClassID m_qobjectClassID;
    JSClassID m_methodClassID;
    JSClassID m_signalClassID;
    JSClassID m_objectClassID;

    QHash<JSAtom, QString> m_atomNameCache;
    QString m_atomNameFallback;   // last uncached atom name, see QuickJSConvert::atomName()
    QHash<QString, JSAtom> m_stringAtomCache;
    QHash<QObject*, JSValue> m_qobjectWrapperCache;
    QHash<const QMetaObject*, JSMetaObjectCache*> m_metaObjectCaches;

    QList<JSGenericReceiver*> m_signalReceivers;
    QList<JSGenericReceiver*> m_garbageReceivers;

    QList<PendingRejection> m_pendingRejections;

    QList<EvalCacheEntry> m_evalCache;

    std::atomic<quint32> m_executionTimeoutMs;
    std::atomic<bool> m_interrupted;
    bool m_autoProcessPromises;
    Extensions m_installedExtensions = Extensions(0);

    JSCommonAtoms m_atoms;

    static int interruptHandler(JSContext * ctx, void* opaque);

    static qint64 steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static qint64 deadlineFromTimeout(quint32 timeoutMs) {
        return timeoutMs > 0 ? steadyNowMs() + static_cast<qint64>(timeoutMs) : 0;
    }

    void queueUnhandledRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason);
    void retractHandledRejection(JSContext* ctx, JSValueConst promise);

    QVariant toVariantImpl(JSValueConst val, JSVisitedStack& visited, int depth);

    void ensureEngineThread() const {
        Q_ASSERT_X(thread() == QThread::currentThread(), "QQuickJSEngine", "Engine methods must be called in the engine's thread");
    }

signals:
    void uncaughtException(const QString& errorMessage, int lineNumber);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(QQuickJSEngine::Extensions)

template<typename T>
void QQuickJSValue::setProperty(const QString& name, const T& value) {
    if (!m_engine) return;
    QQuickJSEngine* jsEngine = m_engine.data();

    if constexpr (std::is_null_pointer_v<T>) {
        setProperty(name, QQuickJSValue(jsEngine, JS_NULL, true));
    } else if constexpr (std::is_convertible_v<T, const char*>) {
        if (value) {
            setProperty(name, QVariant::fromValue(QString::fromUtf8(value)));
        } else {
            setProperty(name, QQuickJSValue(jsEngine, JS_NULL, true));
        }
    } else {
        setProperty(name, QVariant::fromValue(value));
    }
}

template<typename T>
void QQuickJSValue::setProperty(const char* name, const T& value) {
    if (!m_engine || !name) return;
    QQuickJSEngine* jsEngine = m_engine.data();

    if constexpr (std::is_null_pointer_v<T>) {
        setProperty(name, QQuickJSValue(jsEngine, JS_NULL, true));
    } else if constexpr (std::is_convertible_v<T, const char*>) {
        if (value) {
            setProperty(name, QVariant::fromValue(QString::fromUtf8(value)));
        } else {
            setProperty(name, QQuickJSValue(jsEngine, JS_NULL, true));
        }
    } else {
        setProperty(name, QVariant::fromValue(value));
    }
}

template<typename T>
void QQuickJSValue::setProperty(quint32 arrayIndex, const T& value) {
    if (!m_engine) return;
    QQuickJSEngine* jsEngine = m_engine.data();

    if constexpr (std::is_null_pointer_v<T>) {
        setProperty(arrayIndex, QQuickJSValue(jsEngine, JS_NULL, true));
    } else if constexpr (std::is_convertible_v<T, const char*>) {
        if (value) {
            setProperty(arrayIndex, QVariant::fromValue(QString::fromUtf8(value)));
        } else {
            setProperty(arrayIndex, QQuickJSValue(jsEngine, JS_NULL, true));
        }
    } else {
        setProperty(arrayIndex, QVariant::fromValue(value));
    }
}

#endif // QUICKJSENGINE_H
