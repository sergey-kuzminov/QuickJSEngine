//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSEngine_p.h"
#include "QuickJSConvert.h"
#include "QuickJSQObjectProxy.h"
#include "QuickJSSignalProxy.h"

#include <QCoreApplication>
#include <QDate>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStringList>
#include <QThread>
#include <QTime>

#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

QQuickJSEngine::QQuickJSEngine(QObject* parent) : QObject(parent),
    m_rt(nullptr), m_ctx(nullptr), m_globalObj(JS_UNDEFINED),
    m_qobjectClassID(0), m_methodClassID(0), m_signalClassID(0), m_objectClassID(0),
    m_executionTimeoutMs(0), m_interrupted(false), m_autoProcessPromises(true)
{
    qRegisterMetaType<QQuickJSValue>("QQuickJSValue");

    m_rt = JS_NewRuntime();
    if (!m_rt) {
        throw std::runtime_error("QQuickJSEngine: Failed to allocate JSRuntime (out of memory)");
    }

    m_ctx = JS_NewContext(m_rt);
    if (!m_ctx) {
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to allocate JSContext (out of memory)");
    }

    JS_SetMemoryLimit(m_rt, 64 * 1024 * 1024);
    JS_SetMaxStackSize(m_rt, 512 * 1024);
    JS_SetGCThreshold(m_rt, 1 * 1024 * 1024);
    // Explicit defensive default: Atomics.wait() must never block the host
    // thread in a console application. SharedArrayBuffer is not exposed
    // either (no JS_SetSharedArrayBufferFunctions call); the flag guards
    // against future intrinsic changes.
    JS_SetCanBlock(m_rt, false);

    JS_SetContextOpaque(m_ctx, this);
    JS_SetRuntimeOpaque(m_rt, this);
    JS_SetInterruptHandler(m_rt, &QQuickJSEngine::interruptHandler, this);
    JS_SetModuleLoaderFunc(m_rt, &QQuickJSEngine::moduleNormalize,
                            &QQuickJSEngine::moduleLoader, this);

    JS_SetHostPromiseRejectionTracker(m_rt, [](JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void* opaque) {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
        if (!engine) return;
        // quickjs-ng reports a rejection as unhandled at the reject moment and
        // retracts it (is_handled=true) when a handler attaches later in the
        // same tick. Buffering instead of reporting immediately avoids false
        // uncaughtException for rejections handled by a late .catch().
        if (is_handled) {
            engine->retractHandledRejection(ctx, promise);
        } else {
            engine->queueUnhandledRejection(ctx, promise, reason);
        }
    }, this);

    m_atoms.length = JS_NewAtom(m_ctx, "length");
    m_atoms.getTime = JS_NewAtom(m_ctx, "getTime");
    m_atoms.stack = JS_NewAtom(m_ctx, "stack");
    m_atoms.lineNumber = JS_NewAtom(m_ctx, "lineNumber");
    m_atoms.line = JS_NewAtom(m_ctx, "line");
    m_atoms.name = JS_NewAtom(m_ctx, "name");

    JS_NewClassID(m_rt, &m_qobjectClassID);
    JS_NewClassID(m_rt, &m_methodClassID);
    JS_NewClassID(m_rt, &m_signalClassID);

    m_qobjectExotic.set_property = &QuickJSQObjectProxy::qobject_set_property;
    m_qobjectExotic.get_own_property = &QuickJSQObjectProxy::qobject_get_own_property;
    m_qobjectExotic.get_own_property_names = &QuickJSQObjectProxy::qobject_get_own_property_names;
    m_qobjectExotic.delete_property = &QuickJSQObjectProxy::qobject_delete_property;

    JSClassDef qobjectDef = {
        .class_name = "QObject",
        .finalizer = [](JSRuntime* rt, JSValue val) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                auto data = static_cast<JSQObjectData*>(JS_GetOpaque(val, engine->m_qobjectClassID));
                if (data) {
                    if (!data->obj.isNull()) {
                        auto it = engine->m_qobjectWrapperCache.find(data->obj.data());
                        if (it != engine->m_qobjectWrapperCache.end()) {
                            engine->m_qobjectWrapperCache.erase(it);
                        }
                    }
                    data->clearCaches(rt);
                    delete data;
                }
            }
        },
        .gc_mark = [](JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                const auto* data = static_cast<const JSQObjectData*>(JS_GetOpaque(val, engine->m_qobjectClassID));
                if (data) {
                    for (const auto& entry : data->methodProxyCache) {
                        JS_MarkValue(rt, entry.value, mark_func);
                    }
                }
            }
        },
        .call = nullptr,
        .exotic = &m_qobjectExotic,
    };
    if (JS_NewClass(m_rt, m_qobjectClassID, &qobjectDef) < 0) {
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create QObject class");
    }

    // Single C call instead of the global -> Function -> prototype lookup
    // chain; returns a new reference.
    JSValue funcProto = JS_GetFunctionProto(m_ctx);

    if (!JS_IsObject(funcProto)) {
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr; m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to get Function prototype");
    }

    JSClassDef methodDef = {
        .class_name = "MethodProxy",
        .finalizer = [](JSRuntime* rt, JSValue val) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(val, engine->m_methodClassID));
                if (data) delete data;
            }
        },
        .gc_mark = nullptr,
        .call = &QuickJSQObjectProxy::js_method_invoke,
        .exotic = nullptr,
    };
    if (JS_NewClass(m_rt, m_methodClassID, &methodDef) < 0) {
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create MethodProxy class");
    }

    JS_SetClassProto(m_ctx, m_methodClassID, JS_DupValue(m_ctx, funcProto));

    JSClassDef signalDef = {
        .class_name = "SignalProxy",
        .finalizer = [](JSRuntime* rt, JSValue val) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(val, engine->m_signalClassID));
                if (data) delete data;
            }
        },
        .gc_mark = nullptr,
        .call = &QuickJSQObjectProxy::js_method_invoke,
        .exotic = nullptr,
    };
    if (JS_NewClass(m_rt, m_signalClassID, &signalDef) < 0) {
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create SignalProxy class");
    }

    JSValue signalProto = JS_NewObject(m_ctx);
    if (JS_IsException(signalProto)) {
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr; m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create signal prototype object");
    }

    if (JS_SetPrototype(m_ctx, signalProto, funcProto) < 0) {
        qWarning("QQuickJSEngine: Failed to set SignalProxy prototype");
        clearPendingException(m_ctx);
    }
    if (JS_SetPropertyStr(m_ctx, signalProto, "connect", JS_NewCFunction(m_ctx, &QuickJSSignalProxy::js_signal_connect, "connect", 1)) < 0) {
        qWarning("QQuickJSEngine: Failed to install SignalProxy.connect");
        clearPendingException(m_ctx);
    }
    if (JS_SetPropertyStr(m_ctx, signalProto, "disconnect", JS_NewCFunction(m_ctx, &QuickJSSignalProxy::js_signal_disconnect, "disconnect", 1)) < 0) {
        qWarning("QQuickJSEngine: Failed to install SignalProxy.disconnect");
        clearPendingException(m_ctx);
    }
    JS_SetClassProto(m_ctx, m_signalClassID, JS_DupValue(m_ctx, signalProto));

    JS_FreeValue(m_ctx, signalProto);
    JS_FreeValue(m_ctx, funcProto);

    m_globalObj = JS_GetGlobalObject(m_ctx);

    {
        JSValue objCtor = JS_GetPropertyStr(m_ctx, m_globalObj, "Object");
        if (JS_IsObject(objCtor)) {
            JSValue objProto = JS_GetPropertyStr(m_ctx, objCtor, "prototype");
            if (JS_IsObject(objProto)) {
                m_objectClassID = JS_GetClassID(objProto);
                JS_SetClassProto(m_ctx, m_qobjectClassID, JS_DupValue(m_ctx, objProto));
            } else {
                clearPendingException(m_ctx);
            }
            JS_FreeValue(m_ctx, objProto);
        } else {
            clearPendingException(m_ctx);
        }
        JS_FreeValue(m_ctx, objCtor);
    }
}

QQuickJSEngine::~QQuickJSEngine() {
    for (JSGenericReceiver* receiver : m_signalReceivers) delete receiver;
    m_signalReceivers.clear();
    for (JSGenericReceiver* receiver : m_garbageReceivers) delete receiver;
    m_garbageReceivers.clear();

    // Buffered unhandled rejections are freed without reporting: emitting
    // signals from the destructor would be unsafe for subscribers.
    for (const PendingRejection& e : m_pendingRejections) {
        JS_FreeValues(m_ctx, e.promise, e.reason);
    }
    m_pendingRejections.clear();

    QHash<QObject*, JSValue> wrapperCache = std::exchange(m_qobjectWrapperCache, {});

    for (auto it = wrapperCache.begin(); it != wrapperCache.end(); ++it) {
        JSValue jsObj = it.value();
        auto data = static_cast<JSQObjectData*>(JS_GetOpaque(jsObj, m_qobjectClassID));
        if (data) {
            data->clearCaches(m_rt);
            delete data;
            JS_SetOpaque(jsObj, nullptr);
        }
        JS_FreeValue(m_ctx, jsObj);
    }

    for (auto it = m_metaObjectCaches.begin(); it != m_metaObjectCaches.end(); ++it) {
        delete it.value();
    }
    m_metaObjectCaches.clear();

    for (auto& entry : m_evalCache) {
        JS_FreeValue(m_ctx, entry.bytecode);
    }
    m_evalCache.clear();

    for (auto it = m_atomNameCache.begin(); it != m_atomNameCache.end(); ++it) {
        JS_FreeAtom(m_ctx, it.key());
    }
    m_atomNameCache.clear();

    for (auto it = m_stringAtomCache.begin(); it != m_stringAtomCache.end(); ++it) {
        JS_FreeAtom(m_ctx, it.value());
    }
    m_stringAtomCache.clear();

    JS_FreeAtoms(m_ctx, m_atoms.length, m_atoms.getTime, m_atoms.stack,
                 m_atoms.lineNumber, m_atoms.line, m_atoms.name);

    JS_FreeValue(m_ctx, m_globalObj);
    JS_FreeContext(m_ctx);
    JS_FreeRuntime(m_rt);
}

void QQuickJSEngine::setInterrupted(bool interrupted) { m_interrupted.store(interrupted); }
void QQuickJSEngine::setExecutionTimeoutMs(quint32 ms) {
    m_executionTimeoutMs.store(ms);
    // Self-contained deadline: enabling the timeout mid-execution grants a
    // fresh full quota immediately (from any thread); disabling clears it.
    m_executionDeadlineMs.store(deadlineFromTimeout(ms));
}
void QQuickJSEngine::setMemoryLimit(size_t limit) { if (m_rt) JS_SetMemoryLimit(m_rt, limit); }
void QQuickJSEngine::setMaxStackSize(size_t size) { if (m_rt) JS_SetMaxStackSize(m_rt, size); }
void QQuickJSEngine::setAutoProcessPromises(bool enabled) { m_autoProcessPromises = enabled; }

QString QQuickJSEngine::version() {
    return QString::fromUtf8(JS_GetVersion());
}

JSMemoryUsage QQuickJSEngine::memoryUsage() const {
    ensureEngineThread();
    JSMemoryUsage usage{};
    if (m_rt) {
        JS_ComputeMemoryUsage(m_rt, &usage);
    }
    return usage;
}

void QQuickJSEngine::setGCThreshold(size_t threshold) {
    if (m_rt) {
        JS_SetGCThreshold(m_rt, threshold);
    }
}

void QQuickJSEngine::clearEvalCache() {
    ensureEngineThread();
    for (auto& entry : m_evalCache) {
        JS_FreeValue(m_ctx, entry.bytecode);
    }
    m_evalCache.clear();
}

void QQuickJSEngine::setEvalCacheEnabled(bool enabled) {
    ensureEngineThread();
    m_evalCacheEnabled = enabled;
    if (!enabled) clearEvalCache();
}

void QQuickJSEngine::setEvalCacheLimit(int limit) {
    ensureEngineThread();
    m_evalCacheLimit = limit > 0 ? limit : 0;
    if (!m_evalCacheEnabled) return;

    while (m_evalCache.size() > m_evalCacheLimit) {
        JS_FreeValue(m_ctx, m_evalCache.takeLast().bytecode);
    }
}

QQuickJSValue QQuickJSEngine::newErrorObject(QQuickJSValue::ErrorType errorType, QAnyStringView message) {
    ensureEngineThread();
    if (errorType == QQuickJSValue::NoError) {
        // QJSEngine parity: NoError yields an undefined value, not an Error.
        return QQuickJSValue();
    }

    // quickjs-ng provides C constructors with the built-in prototypes for
    // these five classes: engine-created errors stay immune to scripts
    // replacing globalThis.TypeError etc. (QJSEngine parity — V4 also uses
    // its internal prototypes). EvalError/URIError have no C helpers in
    // quickjs-ng 0.18 and keep the global-constructor fallback below.
    if (errorType != QQuickJSValue::EvalError && errorType != QQuickJSValue::URIError) {
        // "%s" stops at an embedded NUL; error messages are not expected
        // to contain one.
        const QByteArray utf8 = message.toString().toUtf8();
        JSValue err = JS_EXCEPTION;
        switch (errorType) {
        case QQuickJSValue::GenericError: err = JS_NewPlainError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::RangeError: err = JS_NewRangeError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::ReferenceError: err = JS_NewReferenceError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::SyntaxError: err = JS_NewSyntaxError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::TypeError: err = JS_NewTypeError(m_ctx, "%s", utf8.constData()); break;
        default: break;   // unreachable: the remaining types are handled above
        }
        if (JS_IsException(err)) return QQuickJSValue::fromException(this);
        QQuickJSValue result(this, err, true);
        result.m_isError = true;
        return result;
    }

    const char* ctorName = "Error";
    switch (errorType) {
    case QQuickJSValue::EvalError: ctorName = "EvalError"; break;
    case QQuickJSValue::RangeError: ctorName = "RangeError"; break;
    case QQuickJSValue::ReferenceError: ctorName = "ReferenceError"; break;
    case QQuickJSValue::SyntaxError: ctorName = "SyntaxError"; break;
    case QQuickJSValue::TypeError: ctorName = "TypeError"; break;
    case QQuickJSValue::URIError: ctorName = "URIError"; break;
    default: break;
    }

    ScopedJSValue global(m_ctx, JS_GetGlobalObject(m_ctx));
    ScopedJSValue ctor(m_ctx, JS_GetPropertyStr(m_ctx, global.value(), ctorName));
    if (consumeException(m_ctx, ctor.value())) return QQuickJSValue();

    ScopedJSValue msgVal(m_ctx, qStringToJS(m_ctx, message.toString()));
    if (JS_IsException(msgVal.value())) return QQuickJSValue::fromException(this);

    JSValueConst argv[1] = { msgVal.value() };
    ScopedJSValue err(m_ctx, JS_CallConstructor(m_ctx, ctor.value(), 1, argv));

    if (JS_IsException(err.value())) return QQuickJSValue::fromException(this);
    QQuickJSValue result(this, err.release(), true);
    result.m_isError = true;
    return result;
}

void QQuickJSEngine::throwError(const QString& message) {
    throwError(QQuickJSValue::GenericError, message);
}

void QQuickJSEngine::throwError(QQuickJSValue::ErrorType errorType, const QString& message) {
    ensureEngineThread();
    QQuickJSValue err = newErrorObject(errorType, message);
    if (!err.isValid()) return;   // construction failed and was already reported
    // JS_Throw() consumes the reference it is given; duplicate so that the
    // local QQuickJSValue keeps its own reference until the end of the scope.
    JS_Throw(m_ctx, JS_DupValue(m_ctx, err.jsVal()));
}

void QQuickJSEngine::throwError(const QQuickJSValue& error) {
    ensureEngineThread();
    if (!error.m_engine && JS_IsUndefined(error.m_val)) {
        // Default-constructed value: throw undefined, like QJSEngine does for
        // an uninitialized QJSValue.
        JS_Throw(m_ctx, JS_UNDEFINED);
        return;
    }
    if (error.m_engine.data() != this) {
        qWarning("QQuickJSEngine::throwError: value belongs to another engine or its engine is gone, ignored");
        return;
    }
    JS_Throw(m_ctx, JS_DupValue(m_ctx, error.m_val));
}

bool QQuickJSEngine::hasError() const {
    ensureEngineThread();
    return JS_HasException(m_ctx);
}

QQuickJSValue QQuickJSEngine::catchError() {
    ensureEngineThread();
    // fromException() checks the flag and returns an undefined (default
    // constructed) value when nothing is pending.
    return QQuickJSValue::fromException(this);
}

void QQuickJSEngine::collectGarbage() {
    ensureEngineThread();
    if (m_rt) JS_RunGC(m_rt);
}

void QQuickJSEngine::setDumpFlags(quint64 flags) {
    ensureEngineThread();
    if (m_rt) JS_SetDumpFlags(m_rt, static_cast<uint64_t>(flags));
}

int QQuickJSEngine::interruptHandler(JSContext * /*ctx*/, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    if (!engine) return 0;

    // QJSEngine parity: the flag is sticky - it stays set (aborting every
    // execution) until setInterrupted(false); it is never consumed here.
    // The execution timeout below remains a per-run quota.
    if (engine->m_interrupted.load()) {
        return 1;
    }

    const qint64 deadline = engine->m_executionDeadlineMs.load();
    if (deadline > 0 && steadyNowMs() > deadline) {
        return 1;
    }
    return 0;
}

QQuickJSValue QQuickJSEngine::globalObject() const {
    ensureEngineThread();
    return QQuickJSValue(const_cast<QQuickJSEngine*>(this), m_globalObj, false);
}

// ---------------------------------------------------------------------------
// Console/translation extension internals (QJSEngine parity; semantics
// verified against qtdeclarative 6.10 sources: qjsengine.cpp /
// qqmlbuiltinfunctions.cpp, and against live Qt 6.12 behavior)
// ---------------------------------------------------------------------------

namespace {

QQuickJSEngine* engineFromContext(JSContext* ctx) {
    return static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

// One console argument rendered as a display string: ToString semantics for
// most values; symbols (which refuse implicit conversion) render as
// "Symbol(description)"; a failed conversion renders as a readable placeholder.
QString consoleArgToString(JSContext* ctx, JSValueConst val) {
    QString part = QuickJSConvert::jsToQString(ctx, val);
    if (part.isNull()) {
        // jsToQString failed and already cleared the pending exception;
        // substitute a readable fallback instead of printing nothing.
        if (JS_IsSymbol(val)) {
            ScopedJSValue desc(ctx, JS_GetPropertyStr(ctx, val, "description"));
            QString descStr;
            if (JS_IsString(desc.value())) {
                descStr = QuickJSConvert::jsToQString(ctx, desc.value());
            } else {
                clearPendingException(ctx);
            }
            part = descStr.isNull() ? QStringLiteral("Symbol()")
                                    : QStringLiteral("Symbol(%1)").arg(descStr);
        } else {
            part = QStringLiteral("[unprintable]");
        }
    }
    return part;
}

// console.assert()/console.exception() message: the arguments joined with a
// single space, like V4's writeToConsole().
QString joinConsoleArgs(JSContext* ctx, int argc, JSValueConst* argv, int start) {
    QStringList parts;
    for (int i = start; i < argc; ++i) {
        parts.append(consoleArgToString(ctx, argv[i]));
    }
    return parts.join(u' ');
}

// Generic console method body for log/info/debug/warn/error: the severity
// arrives in the magic argument. Installed through the static function-list
// table in installExtensions() instead of a per-method creation loop.
static JSValue js_console_generic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    try {
        QStringList msgParts;
        for (int i = 0; i < argc; i++) {
            msgParts.append(consoleArgToString(ctx, argv[i]));
        }

        switch (static_cast<QtMsgType>(magic)) {
        case QtWarningMsg:
            qWarning().noquote() << "[JS console]" << msgParts.join(u' ');
            break;
        case QtCriticalMsg:
            qCritical().noquote() << "[JS console]" << msgParts.join(u' ');
            break;
        default:
            qDebug().noquote() << "[JS console]" << msgParts.join(u' ');
            break;
        }
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

// Caller stack of a console.* invocation, built from a freshly constructed
// Error object: its "stack" property lists this console.* frame first,
// followed by the JavaScript callers; the console frame is dropped.
QString consoleCallerStack(JSContext* ctx) {
    // JS_GetGlobalObject returns a new reference: scope it to free it.
    ScopedJSValue global(ctx, JS_GetGlobalObject(ctx));
    ScopedJSValue errorCtor(ctx, JS_GetPropertyStr(ctx, global.value(), "Error"));
    if (!JS_IsFunction(ctx, errorCtor.value())) {
        clearPendingException(ctx);
        return QString();
    }
    ScopedJSValue err(ctx, JS_CallConstructor(ctx, errorCtor.value(), 0, nullptr));
    if (JS_IsException(err.value())) {
        clearPendingException(ctx);
        return QString();
    }
    ScopedJSValue stack(ctx, JS_GetPropertyStr(ctx, err.value(), "stack"));
    if (JS_IsException(stack.value())) {
        clearPendingException(ctx);
        return QString();
    }
    const QString s = QuickJSConvert::jsToQString(ctx, stack.value());
    if (s.isNull()) return QString();
    const qsizetype nl = s.indexOf(u'\n');
    return nl < 0 ? QString() : s.mid(nl + 1);
}

} // namespace

// qsTr keeps the fixed "QQuickJSEngine" translation context: the calling
// script's URL (V4's context source) is not visible to C callbacks in
// quickjs-ng.
static JSValue js_qsTr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) return JS_ThrowPlainError(ctx, "qsTr() requires at least one argument");
        if (!JS_IsString(argv[0])) return JS_ThrowPlainError(ctx, "qsTr(): first argument (sourceText) must be a string");
        if (argc > 1 && !JS_IsString(argv[1])) return JS_ThrowPlainError(ctx, "qsTr(): second argument (disambiguation) must be a string");
        if (argc > 2 && !JS_IsNumber(argv[2])) return JS_ThrowPlainError(ctx, "qsTr(): third argument (n) must be a number");

        const QString source = QuickJSConvert::jsToQString(ctx, argv[0]);
        QString comment;
        if (argc > 1) comment = QuickJSConvert::jsToQString(ctx, argv[1]);
        int n = -1;
        if (argc > 2) {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[2]) == 0) n = static_cast<int>(d);
            else clearPendingException(ctx);
        }
        return qStringToJS(ctx, QCoreApplication::translate("QQuickJSEngine",
                                                            source.toUtf8().constData(),
                                                            comment.toUtf8().constData(),
                                                            n));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "qsTr: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTr"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static JSValue js_qsTrNoOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_qsTranslate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    try {
        if (argc < 2) return JS_ThrowPlainError(ctx, "qsTranslate() requires at least two arguments");
        if (!JS_IsString(argv[0])) return JS_ThrowPlainError(ctx, "qsTranslate(): first argument (context) must be a string");
        if (!JS_IsString(argv[1])) return JS_ThrowPlainError(ctx, "qsTranslate(): second argument (sourceText) must be a string");
        if (argc > 2 && !JS_IsString(argv[2])) return JS_ThrowPlainError(ctx, "qsTranslate(): third argument (disambiguation) must be a string");

        const QString context = QuickJSConvert::jsToQString(ctx, argv[0]);
        const QString text = QuickJSConvert::jsToQString(ctx, argv[1]);
        QString comment;
        if (argc > 2) comment = QuickJSConvert::jsToQString(ctx, argv[2]);

        int i = 3;
        if (argc > i && JS_IsString(argv[i])) {
            qWarning("qsTranslate(): specifying the encoding as fourth argument is deprecated");
            ++i;
        }
        int n = -1;
        if (argc > i) {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[i]) == 0) n = static_cast<int>(d);
            else clearPendingException(ctx);
        }

        return qStringToJS(ctx, QCoreApplication::translate(context.toUtf8().constData(),
                                                            text.toUtf8().constData(),
                                                            comment.toUtf8().constData(),
                                                            n));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "qsTranslate: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTranslate"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static JSValue js_qsTranslateNoOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[1]);
}

static JSValue js_qsTrId(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) return JS_ThrowPlainError(ctx, "qsTrId() requires at least one argument");
        if (!JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "qsTrId(): first argument (id) must be a string");
        if (argc > 1 && !JS_IsNumber(argv[1])) return JS_ThrowTypeError(ctx, "qsTrId(): second argument (n) must be a number");

        const QString id = QuickJSConvert::jsToQString(ctx, argv[0]);
        int n = -1;
        if (argc > 1) {
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[1]) == 0) n = static_cast<int>(d);
            else clearPendingException(ctx);
        }
        return qStringToJS(ctx, qtTrId(id.toUtf8().constData(), n));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "qsTrId: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTrId"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static JSValue js_qsTrIdNoOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

// String.prototype.arg(value): replaces the lowest numbered %N marker, like
// V4's GlobalExtensions::method_string_arg (int/double/bool/string values
// map to the corresponding QString::arg() overload).
static JSValue js_string_arg(JSContext* ctx, JSValueConst thisObj, int argc, JSValueConst* argv) {
    try {
        if (argc != 1) return JS_ThrowPlainError(ctx, "String.arg(): Invalid arguments");
        const QString value = QuickJSConvert::jsToQString(ctx, thisObj);
        if (value.isNull()) return JS_UNDEFINED;

        if (JS_IsNumber(argv[0])) {
            if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
                return qStringToJS(ctx, value.arg(JS_VALUE_GET_INT(argv[0])));
            }
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[0]) == 0) return qStringToJS(ctx, value.arg(d));
            clearPendingException(ctx);
            return JS_UNDEFINED;
        }
        if (JS_IsBool(argv[0])) {
            return qStringToJS(ctx, value.arg(JS_VALUE_GET_BOOL(argv[0]) != 0));
        }
        const QString argStr = QuickJSConvert::jsToQString(ctx, argv[0]);
        if (argStr.isNull()) return JS_UNDEFINED;
        return qStringToJS(ctx, value.arg(argStr));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "String.arg: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in String.arg"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static void parseStackLocation(const QString& loc, QString* file, int* lineNo, int* colNo) {
    *file = loc;
    const qsizetype c = loc.lastIndexOf(u':');
    if (c <= 0) return;
    bool okCol = false;
    const int col = QStringView{loc}.mid(c + 1).toString().toInt(&okCol);
    if (!okCol) return;
    const qsizetype l = loc.lastIndexOf(u':', c - 1);
    if (l < 0) return;
    bool okLine = false;
    const int lineNo2 = QStringView{loc}.mid(l + 1, c - l - 1).toString().toInt(&okLine);
    if (!okLine) return;
    *file = loc.left(l);
    *lineNo = lineNo2;
    *colNo = col;
}

// Translates the "stack" property of a caught exception into QJSEngine's
// exceptionStackTrace frame format "function:line:column:file" (native frames
// use -1/-1 with an empty file, like V4). quickjs-ng frames look like
// "    at name (file:line:col)", "    at name (native)" or "    at file:line:col";
// a bare token without parentheses or location is an eval filename frame and
// yields an empty function name.
void QQuickJSEngine::collectExceptionStackTrace(QQuickJSEngine* engine, JSValueConst errVal, QStringList* out) {
    if (!out) return;
    out->clear();
    JSContext* ctx = engine->m_ctx;
    ScopedJSValue stack(ctx, JS_GetProperty(ctx, errVal, engine->m_atoms.stack));
    if (JS_IsException(stack.value())) {
        clearPendingException(ctx);
        return;
    }
    const QString s = QuickJSConvert::jsToQString(ctx, stack.value());
    if (s.isNull()) return;

    for (QString rawLine : s.split(u'\n')) {
        QStringView t = QStringView{rawLine}.trimmed();
        if (!t.startsWith(u"at ")) continue;
        t = t.mid(3);

        QString func;
        QString file;
        int lineNo = -1;
        int colNo = -1;

        const qsizetype open = t.indexOf(u'(');
        if (open >= 0 && t.endsWith(u')')) {
            func = t.left(open).trimmed().toString();
            const QString loc = t.mid(open + 1, t.size() - open - 2).toString();
            if (loc != QLatin1String("native") && loc != QLatin1String("missing")) {
                parseStackLocation(loc, &file, &lineNo, &colNo);
            }
        } else {
            // The eval filename frame: "file", "file:line" or "file:line:col"
            parseStackLocation(t.toString(), &file, &lineNo, &colNo);
        }

        out->append(QStringLiteral("%1:%2:%3:%4")
                        .arg(func, QString::number(lineNo), QString::number(colNo), file));
    }
}

void QQuickJSEngine::installExtensions(QQuickJSEngine::Extensions extensions, const QQuickJSValue& object) {
    ensureEngineThread();

    // QJSEngine parity: reject values owned by another engine; non-object
    // targets (including the default argument) fall back to the global object.
    JSValue target = m_globalObj;
    if (object.isValid()) {
        if (object.m_engine.data() != this) {
            qWarning("QQuickJSEngine: Trying to install extensions from a different engine");
            return;
        }
        if (JS_IsObject(object.m_val)) target = object.m_val;
    }

    // Deduplicate by the actual property presence, not only by the installed
    // mask: scripts can delete console/gc/qsTr from the target object, and a
    // repeated request must reinstall what is missing.
    auto needsInstall = [this, target](Extension ext, const char* propName) -> bool {
        if (!m_installedExtensions.testFlag(ext)) return true;
        ScopedJSValue v(m_ctx, JS_GetPropertyStr(m_ctx, target, propName));
        if (JS_IsException(v.value())) {
            clearPendingException(m_ctx);
            return false;
        }
        return JS_IsUndefined(v.value());
    };

    Extensions toInstall = Extensions(0);
    if (extensions.testFlag(ConsoleExtension) && needsInstall(ConsoleExtension, "console")) toInstall |= ConsoleExtension;
    if (extensions.testFlag(GarbageCollectionExtension) && needsInstall(GarbageCollectionExtension, "gc")) toInstall |= GarbageCollectionExtension;
    if (extensions.testFlag(TranslationExtension) && needsInstall(TranslationExtension, "qsTr")) toInstall |= TranslationExtension;
    if (static_cast<int>(toInstall) == 0) return;

    if (toInstall.testFlag(ConsoleExtension)) {
        ScopedJSValue console(m_ctx, JS_NewObject(m_ctx));
        if (JS_IsException(console.value())) {
            clearPendingException(m_ctx);
        } else {
            // log/info/debug/warn/error share one generic C function with a
            // severity magic; the static table installs all five methods in
            // one JS_SetPropertyFunctionList() pass.
            static constexpr JSCFunctionListEntry kConsoleMethods[] = {
                JS_CFUNC_MAGIC_DEF("log", 1, js_console_generic, QtDebugMsg),
                JS_CFUNC_MAGIC_DEF("info", 1, js_console_generic, QtDebugMsg),
                JS_CFUNC_MAGIC_DEF("debug", 1, js_console_generic, QtDebugMsg),
                JS_CFUNC_MAGIC_DEF("warn", 1, js_console_generic, QtWarningMsg),
                JS_CFUNC_MAGIC_DEF("error", 1, js_console_generic, QtCriticalMsg),
            };
            if (JS_SetPropertyFunctionList(m_ctx, console.value(), kConsoleMethods,
                                            static_cast<int>(sizeof(kConsoleMethods) / sizeof(kConsoleMethods[0]))) < 0) {
                clearPendingException(m_ctx);
            }

            // QJSEngine ConsoleExtension parity: assert, count, time, timeEnd,
            // trace, exception. Timers and counters live on the engine (V4
            // stores them on the engine, too), not on the target object.
            auto addConsoleNative = [this](JSValue obj, const char* name, JSCFunction* fn, int length) {
                JSValue func = JS_NewCFunction(m_ctx, fn, name, length);
                if (JS_SetPropertyStr(m_ctx, obj, name, func) < 0) {
                    clearPendingException(m_ctx);
                }
            };

            auto jsConsoleAssert = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc == 0) return JS_ThrowPlainError(ctx, "console.assert(): Missing argument");
                    const int truthy = JS_ToBool(ctx, argv[0]);
                    if (truthy < 0) clearPendingException(ctx);
                    if (truthy == 1) return JS_UNDEFINED;
                    const QString message = joinConsoleArgs(ctx, argc, argv, 1);
                    const QString stack = consoleCallerStack(ctx);
                    qCritical().noquote() << "[JS console]" << (stack.isEmpty() ? message : message + u'\n' + stack);
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleCount = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    QQuickJSEngine* engine = engineFromContext(ctx);
                    if (!engine) return JS_UNDEFINED;
                    const QString label = argc > 0 ? consoleArgToString(ctx, argv[0]) : QString();
                    const int value = ++engine->m_consoleCounters[label];
                    qDebug().noquote() << "[JS console]" << QStringLiteral("%1: %2").arg(label).arg(value);
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleTime = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc != 1) return JS_ThrowPlainError(ctx, "console.time(): Invalid arguments");
                    QQuickJSEngine* engine = engineFromContext(ctx);
                    if (engine) {
                        QElapsedTimer timer;
                        timer.start();
                        engine->m_consoleTimers.insert(consoleArgToString(ctx, argv[0]), timer);
                    }
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleTimeEnd = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc != 1) return JS_ThrowPlainError(ctx, "console.timeEnd(): Invalid arguments");
                    QQuickJSEngine* engine = engineFromContext(ctx);
                    if (engine) {
                        const QString label = consoleArgToString(ctx, argv[0]);
                        const auto it = engine->m_consoleTimers.find(label);
                        if (it != engine->m_consoleTimers.end()) {
                            const qint64 elapsed = it.value().elapsed();
                            engine->m_consoleTimers.erase(it);
                            // V4 format: qDebug("%s: %llims", name, elapsed).
                            qDebug().noquote() << "[JS console]" << QStringLiteral("%1: %2ms").arg(label).arg(elapsed);
                        }
                    }
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleTrace = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* /*argv*/) -> JSValue {
                try {
                    if (argc != 0) return JS_ThrowPlainError(ctx, "console.trace(): Invalid arguments");
                    const QString stack = consoleCallerStack(ctx);
                    if (!stack.isEmpty()) {
                        qDebug().noquote() << "[JS console]" << stack;
                    }
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleException = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc == 0) return JS_ThrowPlainError(ctx, "console.exception(): Missing argument");
                    const QString message = joinConsoleArgs(ctx, argc, argv, 0);
                    const QString stack = consoleCallerStack(ctx);
                    qCritical().noquote() << "[JS console]" << (stack.isEmpty() ? message : message + u'\n' + stack);
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            addConsoleNative(console.value(), "assert", jsConsoleAssert, 1);
            addConsoleNative(console.value(), "count", jsConsoleCount, 0);
            addConsoleNative(console.value(), "time", jsConsoleTime, 1);
            addConsoleNative(console.value(), "timeEnd", jsConsoleTimeEnd, 1);
            addConsoleNative(console.value(), "trace", jsConsoleTrace, 0);
            addConsoleNative(console.value(), "exception", jsConsoleException, 1);

            // print(): a console.log() alias installed on the target object
            // itself, like QJSEngine installs it next to console.
            ScopedJSValue logFn(m_ctx, JS_GetPropertyStr(m_ctx, console.value(), "log"));
            if (JS_SetPropertyStr(m_ctx, target, "console", console.release()) < 0) {
                clearPendingException(m_ctx);
            } else if (JS_IsFunction(m_ctx, logFn.value())) {
                if (JS_SetPropertyStr(m_ctx, target, "print", JS_DupValue(m_ctx, logFn.value())) < 0) {
                    clearPendingException(m_ctx);
                }
            } else {
                clearPendingException(m_ctx);
            }
        }
    }

    if (toInstall.testFlag(GarbageCollectionExtension)) {
        JSValue func = JS_NewCFunction(m_ctx, [](JSContext* ctx, JSValueConst, int, JSValueConst*) -> JSValue {
            try {
                JSRuntime* rt = JS_GetRuntime(ctx);
                QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
                if (engine) engine->collectGarbage();
                return JS_UNDEFINED;
            } catch (const std::exception& e) {
                try { JS_ThrowPlainError(ctx, "gc: %s", e.what()); } catch (...) {}
                return JS_EXCEPTION;
            } catch (...) {
                try { JS_ThrowInternalError(ctx, "Unknown C++ exception in gc"); } catch (...) {}
                return JS_EXCEPTION;
            }
        }, "gc", 0);

        if (JS_SetPropertyStr(m_ctx, target, "gc", func) < 0) {
            clearPendingException(m_ctx);
        }
    }

    if (toInstall.testFlag(TranslationExtension)) {
        auto addTargetFn = [this, target](const char* name, JSCFunction* fn, int length) {
            JSValue func = JS_NewCFunction(m_ctx, fn, name, length);
            if (JS_SetPropertyStr(m_ctx, target, name, func) < 0) {
                clearPendingException(m_ctx);
            }
        };
        addTargetFn("qsTr", js_qsTr, 1);
        addTargetFn("QT_TR_NOOP", js_qsTrNoOp, 1);
        addTargetFn("qsTranslate", js_qsTranslate, 2);
        addTargetFn("QT_TRANSLATE_NOOP", js_qsTranslateNoOp, 2);
        addTargetFn("qsTrId", js_qsTrId, 1);
        addTargetFn("QT_TRID_NOOP", js_qsTrIdNoOp, 1);

        // String.prototype.arg() is installed on the engine-wide string
        // prototype (like V4), regardless of the target object.
        ScopedJSValue stringCtor(m_ctx, JS_GetPropertyStr(m_ctx, m_globalObj, "String"));
        if (JS_IsException(stringCtor.value())) {
            clearPendingException(m_ctx);
        } else {
            ScopedJSValue proto(m_ctx, JS_GetPropertyStr(m_ctx, stringCtor.value(), "prototype"));
            if (JS_IsException(proto.value())) {
                clearPendingException(m_ctx);
            } else if (JS_IsObject(proto.value())) {
                JSValue func = JS_NewCFunction(m_ctx, js_string_arg, "arg", 1);
                if (JS_SetPropertyStr(m_ctx, proto.value(), "arg", func) < 0) {
                    clearPendingException(m_ctx);
                }
            }
        }
    }

    m_installedExtensions |= toInstall;
}

void QQuickJSEngine::processAndEmitException(JSContext* ctx, JSValueConst exc) {
    QString errStr = QuickJSConvert::jsToQString(ctx, exc);
    if (errStr.isNull()) {
        // toString() of the thrown value itself failed. A legitimately thrown
        // empty string is a non-null empty QString and keeps its empty message.
        if (JS_HasException(ctx)) {
            clearPendingException(ctx);
        }
        errStr = QStringLiteral("Unknown exception (toString failed)");
    }

    if (receivers(SIGNAL(uncaughtException(QString,int))) == 0) {
        qWarning().noquote() << "QQuickJSEngine: Uncaught JS exception:" << errStr;
        clearPendingException(ctx);
        return;
    }

    emit uncaughtException(errStr, QuickJSConvert::extractExceptionLine(ctx, exc));
}

void QQuickJSEngine::queueUnhandledRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason) {
    PendingRejection entry{JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)};
    try {
        m_pendingRejections.append(entry);
    } catch (...) {
        // Cannot buffer (OOM): free the duplicates and report immediately.
        JS_FreeValues(ctx, entry.promise, entry.reason);
        processAndEmitException(ctx, reason);
    }
}

void QQuickJSEngine::retractHandledRejection(JSContext* ctx, JSValueConst promise) {
    const void* ptr = JS_VALUE_GET_PTR(promise);
    for (qsizetype i = 0; i < m_pendingRejections.size(); ++i) {
        if (JS_VALUE_GET_PTR(m_pendingRejections[i].promise) == ptr) {
            JS_FreeValue(ctx, m_pendingRejections[i].promise);
            JS_FreeValue(ctx, m_pendingRejections[i].reason);
            m_pendingRejections.removeAt(i);
            return;
        }
    }
}

void QQuickJSEngine::flushUnhandledRejections() {
    ensureEngineThread();
    if (m_pendingRejections.isEmpty()) return;
    if (m_rt && JS_IsJobPending(m_rt)) return;   // handlers may still attach
    // Take the list out first: reporting runs user JS (reason toString) which
    // may buffer new rejections for the next tick.
    const QList<PendingRejection> pending = std::exchange(m_pendingRejections, {});
    for (const PendingRejection& e : pending) {
        processAndEmitException(m_ctx, e.reason);
        JS_FreeValue(m_ctx, e.promise);
        JS_FreeValue(m_ctx, e.reason);
    }
}

void QQuickJSEngine::processPendingJobs() {
    ensureEngineThread();
    if (!m_rt) return;
    JSContext* ctx = m_ctx;
    int jobCount = 0;
    constexpr int MaxPendingJobs = 100000;

    while (jobCount < MaxPendingJobs) {
        int ret = JS_ExecutePendingJob(m_rt, &ctx);
        if (ret == 0) break;
        if (ret < 0) {
            ScopedJSValue exc(ctx, JS_GetException(ctx));
            if (!JS_IsNull(exc.value()) && !JS_IsUndefined(exc.value())) {
                processAndEmitException(ctx, exc.value());
            } else {
                clearPendingException(ctx);
            }
        }
        jobCount++;
    }

    if (jobCount == MaxPendingJobs && JS_IsJobPending(m_rt)) {
        qWarning("QQuickJSEngine: Reached maximum pending jobs limit (%d). Some microtasks might be dropped.", MaxPendingJobs);
    }

    // A direct call from C++ is a top-level entry point: the tick ends here.
    if (m_evalDepth == 0) {
        flushUnhandledRejections();
    }
}

QQuickJSValue QQuickJSEngine::evaluate(const QString& script, const QString& fileName,
                                       int lineNumber, QStringList* exceptionStackTrace) {
    ensureEngineThread();
    if (exceptionStackTrace) exceptionStackTrace->clear();

    // QJSEngine parity: a pending exception (e.g. throwError() called from C++
    // between script executions) preempts evaluation: the script is not
    // executed and the pending error is returned, reported like any other
    // uncaught exception surfaced by evaluate().
    if (JS_HasException(m_ctx)) {
        QQuickJSValue errVal = QQuickJSValue::fromException(this);
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
        return errVal;
    }

    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
    }
    EvalDepthGuard depthGuard(m_evalDepth);
    // Declared after depthGuard so the flush (which may run user JS via reason
    // toString) happens while the depth guard is still held. Top-level calls
    // only: nested evaluates must not report the enclosing tick.
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

    if (m_evalCacheEnabled && lineNumber == 1) {
        for (int i = 0; i < m_evalCache.size(); ++i) {
            if (m_evalCache[i].script == script && m_evalCache[i].fileName == fileName) {
                m_evalCache.move(i, 0);
                const EvalCacheEntry& entry = m_evalCache.first();

                JSValue bc = JS_DupValue(m_ctx, entry.bytecode);
                JSValue ret = JS_EvalFunction(m_ctx, bc);
                ScopedJSValue retScoped(m_ctx, ret);

                const bool hadException = JS_IsException(retScoped.value());
                QQuickJSValue errVal;
                if (hadException) errVal = QQuickJSValue::fromException(this);

                if (m_autoProcessPromises) processPendingJobs();

                if (hadException) {
                    collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
                    processAndEmitException(m_ctx, errVal.jsVal());
                }
                // QJSEngine parity: a sticky interrupt flag yields an "Interrupted"
                // error object even when the script completed without hitting
                // an interrupt check.
                if (isInterrupted()) {
                    return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
                }
                if (hadException) return errVal;
                return QQuickJSValue(this, retScoped.release(), true);
            }
        }
    }

    const QByteArray utf8Script = script.toUtf8();
    const QByteArray utf8FileName = fileName.toUtf8();

    JSEvalOptions options = {
        .version = JS_EVAL_OPTIONS_VERSION,
        .eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY,
        .filename = utf8FileName.constData(),
        .line_num = lineNumber,
    };

    JSValue bc = JS_Eval2(m_ctx, utf8Script.constData(), static_cast<size_t>(utf8Script.size()), &options);

    if (JS_IsException(bc)) {
        QQuickJSValue errVal = QQuickJSValue::fromException(this);
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
        if (isInterrupted()) {
            return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
        }
        return errVal;
    }

    if (m_evalCacheEnabled && lineNumber == 1) {
        JSValue dupBc = JS_DupValue(m_ctx, bc);
        try {
            m_evalCache.prepend(EvalCacheEntry{.script = script, .fileName = fileName, .bytecode = dupBc});
            while (m_evalCache.size() > m_evalCacheLimit) {
                JS_FreeValue(m_ctx, m_evalCache.takeLast().bytecode);
            }
        } catch (...) {
            JS_FreeValue(m_ctx, dupBc);
        }
    }

    JSValue ret = JS_EvalFunction(m_ctx, bc);
    ScopedJSValue retScoped(m_ctx, ret);

    const bool hadException = JS_IsException(retScoped.value());
    QQuickJSValue errVal;
    if (hadException) errVal = QQuickJSValue::fromException(this);

    if (m_autoProcessPromises) processPendingJobs();

    if (hadException) {
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
    }
    // QJSEngine parity: a sticky interrupt flag yields an "Interrupted" error
    // object even when the script completed without hitting an interrupt check.
    if (isInterrupted()) {
        return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
    }
    if (hadException) return errVal;

    return QQuickJSValue(this, retScoped.release(), true);
}

// ---------------------------------------------------------------------------
// ES modules: importModule() / registerModule() (QJSEngine parity; semantics
// verified against live Qt 6.12: canonical-path singleton cache, registered
// modules matched by the bare specifier, default export = the value itself,
// snapshot of enumerable own properties at registration time)
// ---------------------------------------------------------------------------

// Canonical cache key for a module file: QFileInfo::canonicalFilePath() with
// an absolute-path fallback for files that do not exist yet. Qt resource
// paths (":/...") are already canonical.
static QString canonicalModulePath(const QString& fileName) {
    if (fileName.startsWith(QLatin1Char(':'))) return fileName;
    const QFileInfo fi(fileName);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
}

char* QQuickJSEngine::moduleNormalize(JSContext* ctx, const char* base_name,
                                      const char* module_name, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    const QString spec = QString::fromUtf8(module_name);

    if (!spec.startsWith(QLatin1Char('.')) && !spec.startsWith(QLatin1Char('/'))) {
        // Bare specifier (or a ":/..." resource path): registered modules are
        // matched by the exact name, like QJSEngine; everything else is passed
        // through unchanged for the module loader.
        const auto it = engine->m_registeredModules.find(spec);
        if (it != engine->m_registeredModules.end()) {
            return js_strdup(ctx, it.value().constData());
        }
        return js_strdup(ctx, module_name);
    }

    // Relative or root-absolute specifier: resolve against the importing
    // module's directory and canonicalize, so that different spellings of the
    // same file resolve to one module instance.
    QString combined = spec;
    if (spec.startsWith(QLatin1Char('.')) && base_name && *base_name) {
        const QString baseDir = QFileInfo(QString::fromUtf8(base_name)).absolutePath();
        if (!baseDir.isEmpty()) combined = baseDir + QLatin1Char('/') + spec;
    }
    const QString canonical = combined.startsWith(QLatin1Char(':'))
                                  ? QDir::cleanPath(combined)
                                  : canonicalModulePath(combined);
    const QByteArray utf8 = canonical.toUtf8();
    return js_strdup(ctx, utf8.constData());
}

JSModuleDef* QQuickJSEngine::moduleLoader(JSContext* ctx, const char* module_name, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    if (!engine) return nullptr;

    // The same file may already be loaded under a different spelling (for
    // example a bare name from one importer and a relative path from another);
    // unify on the canonical-keyed singleton instance. The entry may still be
    // uninstantiated (compiled only); the importer's linking instantiates it.
    const QString cacheKey = canonicalModulePath(QString::fromUtf8(module_name));
    if (JSModuleDef* existing = engine->m_fileModules.value(cacheKey).def) {
        return existing;
    }

    // QFile understands Qt resource paths (":/...").
    QFile file(cacheKey);
    if (!file.open(QIODevice::ReadOnly)) {
        // QJSEngine reports all module-loading failures as SyntaxError
        // objects; match the error class (the message text is our own).
        JS_ThrowSyntaxError(ctx, "could not load module filename '%s'", cacheKey.toUtf8().constData());
        return nullptr;
    }
    const QByteArray source = file.readAll();

    const QByteArray nameUtf8 = cacheKey.toUtf8();
    JSEvalOptions options = {
        .version = JS_EVAL_OPTIONS_VERSION,
        .eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY,
        .filename = nameUtf8.constData(),
        .line_num = 1,
    };
    JSValue mod = JS_Eval2(ctx, source.constData(), static_cast<size_t>(source.size()), &options);
    if (JS_IsException(mod)) return nullptr;
    JSModuleDef* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod));
    JS_FreeValue(ctx, mod);
    engine->m_fileModules.insert(cacheKey, FileModuleEntry{def, false});
    return def;
}

int QQuickJSEngine::registeredModuleInit(JSContext* ctx, JSModuleDef* m) {
    // The snapshot (an internal object holding "default" plus the captured
    // named exports) was stored as the module's private value by
    // registerModule(); instantiation hands the values to the engine.
    ScopedJSValue snap(ctx, JS_GetModulePrivateValue(ctx, m));
    if (!JS_IsObject(snap.value())) return 0;

    JSPropertyEnum* props = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, snap.value(), JS_GPN_STRING_MASK) < 0) {
        return -1;
    }

    int ret = 0;
    for (uint32_t i = 0; i < count && ret == 0; ++i) {
        ScopedCString name(ctx, JS_AtomToCString(ctx, props[i].atom));
        JSValue val = JS_GetProperty(ctx, snap.value(), props[i].atom);
        if (!name.isValid() || JS_IsException(val)) {
            // JS_SetModuleExport() consumes the value in all cases; free the
            // failed read result manually before bailing out.
            JS_FreeValue(ctx, val);
            ret = -1;
            break;
        }
        // Ownership of val passes to the module on success and is freed by
        // the engine on failure.
        if (JS_SetModuleExport(ctx, m, name.value(), val) < 0) ret = -1;
    }
    JS_FreePropertyEnum(ctx, props, count);
    return ret;
}

QQuickJSValue QQuickJSEngine::importModule(const QString& fileName) {
    ensureEngineThread();

    // QJSEngine parity: a pending exception preempts the import and is
    // returned as the result instead of executing anything (the leaked
    // exception of a failed registration surfaces here in live Qt 6.12).
    if (JS_HasException(m_ctx)) {
        return QQuickJSValue::fromException(this);
    }

    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
    }
    EvalDepthGuard depthGuard(m_evalDepth);
    // Declared after depthGuard so the flush (which may run user JS via reason
    // toString) happens while the depth guard is still held. Top-level calls
    // only: nested imports must not report the enclosing tick.
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

    // Singleton cache: the first import evaluates the module; every later
    // import - from C++ or from JavaScript - returns the same instance. A
    // cached entry that has never been driven through JS_EvalFunction (a
    // dependency whose import chain failed before linking) is evaluated now.
    const QString cacheKey = canonicalModulePath(fileName);
    const auto cached = m_fileModules.constFind(cacheKey);
    if (cached != m_fileModules.cend() && cached->instantiated) {
        JSValue ns = JS_GetModuleNamespace(m_ctx, cached->def);
        if (JS_IsException(ns)) {
            return QQuickJSValue::fromException(this);
        }
        return QQuickJSValue(this, ns, true);
    }

    JSModuleDef* def = nullptr;
    if (cached != m_fileModules.cend()) {
        def = cached->def;
    } else {
        QFile file(cacheKey);
        if (!file.open(QIODevice::ReadOnly)) {
            // QJSEngine reports all module-loading failures as SyntaxError
            // objects; match the error class (the message text is our own).
            JS_ThrowSyntaxError(m_ctx, "could not load module filename '%s'", cacheKey.toUtf8().constData());
            return QQuickJSValue::fromException(this);
        }
        const QByteArray source = file.readAll();

        const QByteArray nameUtf8 = cacheKey.toUtf8();
        JSEvalOptions options = {
            .version = JS_EVAL_OPTIONS_VERSION,
            .eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY,
            .filename = nameUtf8.constData(),
            .line_num = 1,
        };
        JSValue mod = JS_Eval2(m_ctx, source.constData(), static_cast<size_t>(source.size()), &options);
        if (JS_IsException(mod)) {
            // Syntax error and the like; Qt reports this as a SyntaxError, too.
            return QQuickJSValue::fromException(this);
        }
        def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod));
        JS_FreeValue(m_ctx, mod);
        // Register before evaluation: a module that throws stays cached, and a
        // re-import returns its namespace without re-running the code.
        m_fileModules.insert(cacheKey, FileModuleEntry{def, false});
    }

    // JS_EvalFunction consumes the module value it is given; hand it a fresh
    // reference to the def. Evaluation failures come in two shapes: linking
    // errors raise a context exception, while a module that throws in its
    // top-level code rejects the evaluation promise.
    JSValue ret = JS_EvalFunction(m_ctx, JS_DupValue(m_ctx, JS_MKPTR(JS_TAG_MODULE, def)));
    ScopedJSValue retScoped(m_ctx, ret);

    if (m_autoProcessPromises) processPendingJobs();

    QQuickJSValue errVal;
    bool linked = true;
    if (JS_IsException(ret)) {
        // A linking failure leaves the def uninstantiated: a later import
        // retries the whole evaluation.
        errVal = QQuickJSValue::fromException(this);
        linked = false;
    } else if (JS_PromiseState(m_ctx, ret) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(m_ctx, ret);
        // The rejection was reported to the unhandled-rejection tracker when
        // it happened; the error is handed to the C++ caller instead, so
        // retract the buffered report to avoid a duplicate uncaughtException.
        retractHandledRejection(m_ctx, ret);
        errVal = QQuickJSValue(this, reason, true);
    }

    if (linked) {
        const auto entry = m_fileModules.find(cacheKey);
        if (entry != m_fileModules.end()) entry->instantiated = true;
    }

    // QJSEngine parity: a sticky interrupt flag yields an "Interrupted" error
    // object even when the module code failed for another reason (the same
    // precedence evaluate() applies).
    if (isInterrupted()) {
        return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
    }
    if (errVal.isValid()) {
        return errVal;
    }

    JSValue ns = JS_GetModuleNamespace(m_ctx, def);
    if (JS_IsException(ns)) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, ns, true);
}

bool QQuickJSEngine::registerModule(const QString& moduleName, const QQuickJSValue& value) {
    ensureEngineThread();

    // A pending exception preempts the registration, like evaluate().
    if (JS_HasException(m_ctx)) {
        return false;
    }

    if (!value.isValid() || value.engine() != this) {
        // Passing a foreign value is undefined behavior in QJSEngine; refuse
        // it with a warning instead of corrupting this engine.
        qWarning("QQuickJSEngine::registerModule: the value is invalid or belongs to another engine");
        return false;
    }

    // The snapshot reads property values, which may run user JavaScript
    // (getters): start a fresh execution-timeout quota and keep the tick
    // bookkeeping consistent with the other JS entry points.
    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
    }
    EvalDepthGuard depthGuard(m_evalDepth);
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

    // Snapshot: "default" is the value itself; every own enumerable string
    // property becomes a named export. Values - including getters - are read
    // exactly once, here; later modifications of the value are not visible to
    // importers, matching the documented QJSEngine semantics.
    JSValueConst val = value.jsVal();
    JSValue snap = JS_NewObject(m_ctx);
    if (JS_IsException(snap)) {
        clearPendingException(m_ctx);
        return false;
    }

    bool ok = true;
    QVarLengthArray<QByteArray, 16> exportNames;
    if (JS_IsObject(val)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(m_ctx, &props, &count, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            clearPendingException(m_ctx);
            JS_FreeValue(m_ctx, snap);
            return false;
        }
        for (uint32_t i = 0; i < count && ok; ++i) {
            ScopedCString name(m_ctx, JS_AtomToCString(m_ctx, props[i].atom));
            if (!name.isValid()) {
                ok = false;
                break;
            }
            JSValue pv = JS_GetProperty(m_ctx, val, props[i].atom);
            if (JS_IsException(pv)) {
                // A throwing getter leaves the exception pending and fails
                // the registration, like live Qt 6.12 (the next import or
                // evaluate call surfaces the error).
                ok = false;
                break;
            }
            // Ownership of pv passes to the snapshot object in both cases.
            if (JS_SetProperty(m_ctx, snap, props[i].atom, pv) < 0) {
                ok = false;
                continue;
            }
            // "default" is declared separately (it is the value itself,
            // like QJSEngine): a same-named property of the value is ignored.
            if (qstrcmp(name.value(), "default") != 0) {
                exportNames.append(QByteArray(name.value()));
            }
        }
        JS_FreePropertyEnum(m_ctx, props, count);
        if (!ok) {
            JS_FreeValue(m_ctx, snap);
            return false;
        }
    }

    if (JS_SetPropertyStr(m_ctx, snap, "default", JS_DupValue(m_ctx, val)) < 0) {
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, snap);
        return false;
    }

    // A unique internal name lets re-registration replace the module for new
    // imports (the previous def stays in the engine's list, unreachable). The
    // public name is mapped by the normalizer.
    const QByteArray internal = "quickjsengine::module::" + QByteArray::number(++m_moduleSerial)
                                + "::" + moduleName.toUtf8();
    JSModuleDef* mod = JS_NewCModule(m_ctx, internal.constData(),
                                     &QQuickJSEngine::registeredModuleInit);
    if (!mod) {
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, snap);
        return false;
    }

    if (JS_AddModuleExport(m_ctx, mod, "default") < 0) {
        ok = false;
    }
    for (const QByteArray& exportName : std::as_const(exportNames)) {
        if (!ok) break;
        if (JS_AddModuleExport(m_ctx, mod, exportName.constData()) < 0) {
            ok = false;
        }
    }

    if (!ok) {
        // The half-registered module stays in the list under its unique name;
        // it is never referenced by the normalizer and cannot be imported.
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, snap);
        return false;
    }

    // JS_SetModulePrivateValue() takes ownership of the snapshot value.
    JS_SetModulePrivateValue(m_ctx, mod, snap);
    m_registeredModules.insert(moduleName, internal);
    return true;
}

void QQuickJSEngine::setProperty(const char* name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (!name) return;
    if (value.m_engine.data() != this) {
        if (!(value.m_engine.isNull() && JS_IsUndefined(value.m_val))) return;
    }

    JSValue v = JS_DupValue(m_ctx, value.jsVal());
    if (JS_SetPropertyStr(m_ctx, m_globalObj, name, v) < 0) {
        clearPendingException(m_ctx);
    }
}
void QQuickJSEngine::setProperty(const QByteArray& name, const QQuickJSValue& value) { setProperty(name.constData(), value); }
void QQuickJSEngine::setProperty(QStringView name, const QQuickJSValue& value) { setProperty(name.toString(), value); }
// QString names go through the shared string->atom cache: this skips the
// toUtf8() copy and the engine atom-table probe on every call. const char*/
// and QByteArray stay on JS_SetPropertyStr, which is already allocation-free.
void QQuickJSEngine::setProperty(const QString& name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (value.m_engine.data() != this) {
        if (!(value.m_engine.isNull() && JS_IsUndefined(value.m_val))) return;
    }

    JSAtom atom = QuickJSConvert::cachedStringAtom(this, m_ctx, name);
    if (atom == JS_ATOM_NULL) {
        clearPendingException(m_ctx);
        return;
    }

    JSValue v = JS_DupValue(m_ctx, value.jsVal());
    if (JS_SetProperty(m_ctx, m_globalObj, atom, v) < 0) {
        clearPendingException(m_ctx);
    }
    JS_FreeAtom(m_ctx, atom);
}

void QQuickJSEngine::setProperty(const QString& name, const QVariant& value) {
    ensureEngineThread();
    JSValue v = toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(m_ctx);
        return;
    }
    QQuickJSValue val(this, v, true);
    setProperty(name, val);
}

void QQuickJSEngine::setProperty(const char* name, const QVariant& value) {
    ensureEngineThread();
    if (!name) return;
    JSValue v = toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(m_ctx);
        return;
    }
    QQuickJSValue val(this, v, true);
    setProperty(name, val);
}

void QQuickJSEngine::setProperty(QStringView name, const QVariant& value) {
    setProperty(name.toString(), value);
}

QQuickJSValue QQuickJSEngine::newQObject(QObject* obj) {
    ensureEngineThread();
    return QQuickJSValue(this, wrapQObject(obj), true);
}

QQuickJSValue QQuickJSEngine::newObject() {
    ensureEngineThread();
    ScopedJSValue obj(m_ctx, JS_NewObject(m_ctx));
    if (JS_IsException(obj.value())) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, obj.release(), true);
}

QQuickJSValue QQuickJSEngine::newSymbol(const QString& name) {
    ensureEngineThread();
    // The description is stored as given: an empty name yields a symbol
    // with an empty description, like QJSEngine::newSymbol().
    const QByteArray utf8 = name.toUtf8();
    ScopedJSValue sym(m_ctx, JS_NewSymbol(m_ctx, utf8.constData(), false));
    if (JS_IsException(sym.value())) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, sym.release(), true);
}

QQuickJSValue QQuickJSEngine::newArray(uint length) {
    ensureEngineThread();
    ScopedJSValue arr(m_ctx, JS_NewArray(m_ctx));
    if (JS_IsException(arr.value())) {
        return QQuickJSValue::fromException(this);
    }

    if (length > 0) {
        ScopedJSValue lenVal(m_ctx, JS_NewUint32(m_ctx, length));
        if (JS_SetProperty(m_ctx, arr.value(), m_atoms.length, lenVal.release()) < 0) {
            clearPendingException(m_ctx);
        }
    }
    return QQuickJSValue(this, arr.release(), true);
}

QQuickJSValue QQuickJSEngine::newFunction(JSCFunction* func, const char* name, int argc) {
    ensureEngineThread();
    ScopedJSValue funcVal(m_ctx, JS_NewCFunction(m_ctx, func, name, argc));
    if (JS_IsException(funcVal.value())) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, funcVal.release(), true);
}

JSValue QQuickJSEngine::toJSValue(const QVariant& var) {
    ensureEngineThread();
    return toJSValueImpl(var, 0);
}

JSValue QQuickJSEngine::toJSValueImpl(const QVariant& var, int depth) {
    ensureEngineThread();

    if (depth > MaxVariantConversionDepth) {
        JS_ThrowRangeError(m_ctx, "Variant nesting too deep");
        return JS_EXCEPTION;
    }

    if (!var.isValid()) return JS_UNDEFINED;

    // Never-null types first: numeric, bool, pointer and container variants are
    // never null, so they skip the isNull() call and the enum-type lookup.
    switch (var.userType()) {
    case QMetaType::Bool: return JS_NewBool(m_ctx, var.toBool());
    case QMetaType::Int: case QMetaType::Short: case QMetaType::SChar: case QMetaType::Char:
        return JS_NewInt32(m_ctx, var.toInt());
    case QMetaType::Long: case QMetaType::LongLong:
        return QuickJSConvert::int64ToJS(m_ctx, var.toLongLong());
    case QMetaType::UInt: case QMetaType::UShort: case QMetaType::UChar: {
        quint32 val = var.toUInt();
        if (val <= static_cast<quint32>(std::numeric_limits<int32_t>::max())) return JS_NewInt32(m_ctx, static_cast<int32_t>(val));
        return JS_NewFloat64(m_ctx, static_cast<double>(val));
    }
    case QMetaType::ULong: case QMetaType::ULongLong:
        return QuickJSConvert::uint64ToJS(m_ctx, var.toULongLong());
    case QMetaType::Double: case QMetaType::Float: return JS_NewFloat64(m_ctx, var.toDouble());
    case QMetaType::QStringList:
        return QuickJSConvert::stringListToJSArray(this, m_ctx, static_cast<const QStringList*>(var.constData()));
    case QMetaType::QObjectStar:
        return wrapQObject(*static_cast<QObject* const*>(var.constData()));
    case QMetaType::QVariantList:
        return QuickJSConvert::listToJSArray(this, m_ctx, static_cast<const QVariantList*>(var.constData()), depth);
    case QMetaType::QVariantMap:
        return QuickJSConvert::mapToJSObject(this, m_ctx, static_cast<const QVariantMap*>(var.constData()), depth);
    case QMetaType::QVariantHash:
        return QuickJSConvert::hashToJSObject(this, m_ctx, static_cast<const QVariantHash*>(var.constData()), depth);
    default: break;
    }

    if (var.isNull()) return JS_NULL;
    if (var.userType() == QMetaType::QVariant) return toJSValueImpl(var.value<QVariant>(), depth);

    if (QMetaType(var.userType()).flags() & QMetaType::IsEnumeration) {
        return JS_NewInt32(m_ctx, var.toInt());
    }

    switch (var.userType()) {
    case QMetaType::QString: return qStringToJS(m_ctx, var.toString());
    case QMetaType::QChar: return qStringToJS(m_ctx, QString(var.toChar()));
    case QMetaType::QByteArray:
        return QuickJSConvert::byteArrayToJS(m_ctx, var.toByteArray());
    case QMetaType::QDateTime:
        return QuickJSConvert::dateTimeToJS(m_ctx, var.toDateTime());
    case QMetaType::QDate:
        return QuickJSConvert::dateToJS(m_ctx, var.toDate());
    case QMetaType::QTime:
        return QuickJSConvert::timeToJS(m_ctx, var.toTime());
    default: break;
    }

    if (var.userType() == qMetaTypeId<QQuickJSValue>()) {
        const auto* jsValPtr = static_cast<const QQuickJSValue*>(var.constData());
        if (jsValPtr && jsValPtr->m_engine == this) return JS_DupValue(m_ctx, jsValPtr->jsVal());
    }

    if (var.canConvert<QVariantList>()) {
        QVariantList list = var.value<QVariantList>();
        return QuickJSConvert::listToJSArray(this, m_ctx, &list, depth);
    }

    if (var.canConvert<QVariantHash>()) {
        QVariantHash hash = var.value<QVariantHash>();
        return QuickJSConvert::hashToJSObject(this, m_ctx, &hash, depth);
    }

    if (var.canConvert<QObject*>()) return wrapQObject(var.value<QObject*>());

    return JS_UNDEFINED;
}

QVariant QQuickJSEngine::toVariant(JSValueConst val) {
    ensureEngineThread();
    JSVisitedStack visited;
    return toVariantImpl(val, visited, 0, QQuickJSValue::ConvertJSObjects);
}

QVariant QQuickJSEngine::toVariant(JSValueConst val, QQuickJSValue::ObjectConversionBehavior behavior) {
    ensureEngineThread();
    JSVisitedStack visited;
    return toVariantImpl(val, visited, 0, behavior);
}

QVariant QQuickJSEngine::toVariantImpl(JSValueConst val, JSVisitedStack& visited, int depth,
                                       QQuickJSValue::ObjectConversionBehavior behavior) {
    ensureEngineThread();

    switch (JS_VALUE_GET_TAG(val)) {
    case JS_TAG_UNDEFINED: return QVariant();
    case JS_TAG_NULL: {
        void* nullPtr = nullptr;
        return QVariant(QMetaType(QMetaType::VoidStar), &nullPtr);
    }
    case JS_TAG_BOOL: return JS_VALUE_GET_BOOL(val) != 0;
    case JS_TAG_INT: return JS_VALUE_GET_INT(val);
    case JS_TAG_FLOAT64: {
        double d = JS_VALUE_GET_FLOAT64(val);
        return d;
    }
    case JS_TAG_SHORT_BIG_INT: {
        return QVariant::fromValue(static_cast<qint64>(JS_VALUE_GET_SHORT_BIG_INT(val)));
    }
    case JS_TAG_BIG_INT: {
        size_t len;
        const char* s = JS_ToCStringLen(m_ctx, &len, val);
        if (s) {
            QByteArray ba(s, len);
            JS_FreeCString(m_ctx, s);

            bool ok = false;
            qint64 i64 = ba.toLongLong(&ok);
            if (ok) return QVariant::fromValue(i64);

            quint64 u64 = ba.toULongLong(&ok);
            if (ok) return QVariant::fromValue(u64);

            return QVariant(QString::fromUtf8(ba));
        }
        clearPendingException(m_ctx);
        return QVariant();
    }
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return QuickJSConvert::jsToQString(m_ctx, val);
    case JS_TAG_SYMBOL: {
        // RetainJSObjects wraps the symbol itself (QJSEngine parity). The
        // lossy conversion deliberately yields an invalid QVariant instead of
        // QJSEngine's "Symbol(name)" QString: a string-shaped variant would
        // silently coerce in metacall conversions (e.g. a nullptr written into
        // a QObject* property) instead of throwing a TypeError.
        if (behavior == QQuickJSValue::RetainJSObjects) {
            return QVariant::fromValue(QQuickJSValue(this, JS_DupValue(m_ctx, val), true));
        }
        return QVariant();
    }
    case JS_TAG_OBJECT: {
        void* ptr = JS_VALUE_GET_PTR(val);
        if (visited.contains(ptr)) return QVariant();   // cycle: invalid QVariant instead of a copy

        if (depth > MaxVariantConversionDepth) {
            return QVariant();
        }

        visited.push(ptr);

        QVariant result;
        try {
            if (JS_IsDate(val)) {
                ScopedJSValue timeVal(m_ctx, JS_GetProperty(m_ctx, val, m_atoms.getTime));
                if (JS_IsFunction(m_ctx, timeVal.value())) {
                    ScopedJSValue ret(m_ctx, JS_Call(m_ctx, timeVal.value(), val, 0, nullptr));
                    if (consumeException(m_ctx, ret.value())) {
                        result = QVariant();
                    } else {
                        double msecs = 0;
                        if (JS_ToFloat64(m_ctx, &msecs, ret.value()) == 0 && std::isfinite(msecs)) {
                            result = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(msecs));
                        } else {
                            clearPendingException(m_ctx);
                            result = QVariant();
                        }
                    }
                } else {
                    result = QVariant();
                }
            } else {
                result = QuickJSConvert::variantFromJSObject(this, m_ctx, val, m_qobjectClassID, m_objectClassID, visited, depth + 1, behavior);
            }
        } catch (...) {
            visited.pop();
            throw;
        }

        visited.pop();
        return result;
    }
    default: break;
    }
    return QVariant();
}

JSValue QQuickJSEngine::toJSValueFromVoid(void* data, int metaType) {
    if (metaType == QMetaType::Int) return JS_NewInt32(m_ctx, *static_cast<int*>(data));
    if (metaType == QMetaType::Bool) return JS_NewBool(m_ctx, *static_cast<bool*>(data));
    if (metaType == QMetaType::Double) return JS_NewFloat64(m_ctx, *static_cast<double*>(data));
    if (metaType == QMetaType::Float) return JS_NewFloat64(m_ctx, *static_cast<float*>(data));
    if (metaType == QMetaType::QString) return qStringToJS(m_ctx, *static_cast<QString*>(data));
    if (metaType == QMetaType::QObjectStar) return wrapQObject(*static_cast<QObject**>(data));
    if (metaType == QMetaType::QVariant) return toJSValue(*static_cast<QVariant*>(data));
    if (metaType == QMetaType::UnknownType || metaType == QMetaType::Void) return JS_UNDEFINED;

    // Same scalar fast paths as toJSValueImpl(): metacall argument storage
    // points at a value of exactly the declared type.
    if (metaType == QMetaType::LongLong || metaType == QMetaType::Long)
        return QuickJSConvert::int64ToJS(m_ctx, *static_cast<qint64*>(data));
    if (metaType == QMetaType::ULongLong || metaType == QMetaType::ULong)
        return QuickJSConvert::uint64ToJS(m_ctx, *static_cast<quint64*>(data));
    if (metaType == QMetaType::UInt) return QuickJSConvert::uint64ToJS(m_ctx, *static_cast<uint*>(data));
    if (metaType == QMetaType::UShort) return JS_NewInt32(m_ctx, *static_cast<ushort*>(data));
    if (metaType == QMetaType::UChar) return JS_NewInt32(m_ctx, *static_cast<uchar*>(data));
    if (metaType == QMetaType::Short) return JS_NewInt32(m_ctx, *static_cast<short*>(data));
    if (metaType == QMetaType::Char) return JS_NewInt32(m_ctx, *static_cast<char*>(data));
    if (metaType == QMetaType::SChar) return JS_NewInt32(m_ctx, *static_cast<signed char*>(data));
    if (metaType == QMetaType::QDateTime) return QuickJSConvert::dateTimeToJS(m_ctx, *static_cast<QDateTime*>(data));
    if (metaType == QMetaType::QDate) return QuickJSConvert::dateToJS(m_ctx, *static_cast<QDate*>(data));
    if (metaType == QMetaType::QTime) return QuickJSConvert::timeToJS(m_ctx, *static_cast<QTime*>(data));
    if (metaType == QMetaType::QByteArray) return QuickJSConvert::byteArrayToJS(m_ctx, *static_cast<QByteArray*>(data));
    if (metaType == qMetaTypeId<QVariantHash>())
        return QuickJSConvert::hashToJSObject(this, m_ctx, static_cast<const QVariantHash*>(data), 0);
    if (metaType == qMetaTypeId<QStringList>())
        return QuickJSConvert::stringListToJSArray(this, m_ctx, static_cast<const QStringList*>(data));
    {
        const QMetaType mt(metaType);
        if ((mt.flags() & QMetaType::IsEnumeration) && mt.sizeOf() == sizeof(int))
            return JS_NewInt32(m_ctx, *static_cast<int*>(data));
    }

    QVariant var(QMetaType(metaType), data);
    return toJSValue(var);
}

JSValue QQuickJSEngine::wrapQObject(QObject* obj) {
    ensureEngineThread();

    if (!obj) return JS_NULL;

    try {
        auto it = m_qobjectWrapperCache.find(obj);
        if (it != m_qobjectWrapperCache.end()) return JS_DupValue(m_ctx, it.value());

        auto data = std::make_unique<JSQObjectData>();
        data->obj = obj;

        ScopedJSValue jsObjScoped(m_ctx, JS_NewObjectClass(m_ctx, m_qobjectClassID));
        if (JS_IsException(jsObjScoped.value())) return JS_EXCEPTION;

        const QMetaObject* meta = obj->metaObject();
        data->metaObj = meta;

        auto cacheIt = m_metaObjectCaches.find(meta);
        if (cacheIt != m_metaObjectCaches.end() && cacheIt.value()->className != meta->className()) {
            // The address was reused by a different QMetaObject (dynamic
            // metaobjects can be freed while the cache entry survives):
            // rebuild instead of serving a foreign cache.
            delete cacheIt.value();
            m_metaObjectCaches.erase(cacheIt);
            cacheIt = m_metaObjectCaches.end();
        }
        if (cacheIt == m_metaObjectCaches.end()) {
            auto cache = std::make_unique<JSMetaObjectCache>(m_ctx);
            cache->className = meta->className();
            const int methodCount = meta->methodCount();
            const int propCount = meta->propertyCount();

            cache->entries.reserve(methodCount + propCount);

            for (int i = 0; i < methodCount; ++i) {
                QMetaMethod m = meta->method(i);
                if (m.access() == QMetaMethod::Private) continue;

                const QByteArray name = m.name();
                JSAtom atom = atomFromUtf8(m_ctx, name);
                if (atom == JS_ATOM_NULL) { JS_ThrowOutOfMemory(m_ctx); return JS_EXCEPTION; }
                if (cache->contains(atom)) { JS_FreeAtom(m_ctx, atom); continue; }

                bool ownedByHash = false;
                try {
                    int retType = m.returnType();
                    if (!QMetaType(retType).isRegistered()) retType = QMetaType::UnknownType;

                    const int pCount = m.parameterCount();
                    if (pCount > 255) {
                        // paramCount is stored in a uint8_t; without this check a
                        // release build would silently truncate (256 -> 0 args).
                        qWarning("QQuickJSEngine: Method '%s' has more than 255 parameters (%d), skipping it", name.constData(), pCount);
                        JS_FreeAtom(m_ctx, atom);
                        continue;
                    }

                    MetaCacheEntry entry{
                        .kind = MetaCacheEntry::Kind::Method,
                        .methodType = static_cast<uint8_t>(m.methodType()),
                        .isWritable = false,
                        .isReadable = true,
                        .isResettable = false,
                        .paramCount = static_cast<uint8_t>(pCount),
                        .index = i,
                        .type = retType,
                        .paramTypes = {},
                    };
                    entry.paramTypes.resize(entry.paramCount);
                    for (int j = 0; j < entry.paramCount; ++j) {
                        int paramType = m.parameterType(j);
                        if (!QMetaType(paramType).isRegistered()) paramType = QMetaType::UnknownType;
                        entry.paramTypes[j] = QuickJSConvert::adjustEnumType(paramType);
                    }

                    cache->insert(atom, entry);
                    ownedByHash = true;
                    // Hide QObject::destroyed()/deleteLater() from enumeration
                    // (QJSEngine parity). They remain reachable by direct name
                    // access - documented @warning.
                    static const int s_destroyedIdx1 =
                        QObject::staticMetaObject.indexOfSignal("destroyed(QObject*)");
                    static const int s_destroyedIdx2 =
                        QObject::staticMetaObject.indexOfSignal("destroyed()");
                    static const int s_deleteLaterIdx =
                        QObject::staticMetaObject.indexOfSlot("deleteLater()");
                    const bool isQObjectSubclass = meta->inherits(&QObject::staticMetaObject);
                    if (!isQObjectSubclass
                        || (i != s_destroyedIdx1 && i != s_destroyedIdx2
                            && i != s_deleteLaterIdx)) {
                        cache->methodAtoms.append(atom);
                    }
                } catch (...) {
                    if (!ownedByHash) JS_FreeAtom(m_ctx, atom);
                    JS_ThrowOutOfMemory(m_ctx);
                    return JS_EXCEPTION;
                }
            }

            for (int i = 0; i < propCount; ++i) {
                QMetaProperty p = meta->property(i);
                if (p.isReadable() || p.isWritable()) {
                    JSAtom atom = JS_NewAtom(m_ctx, p.name());
                    if (atom == JS_ATOM_NULL) {
                        JS_ThrowOutOfMemory(m_ctx);
                        return JS_EXCEPTION;
                    }

                    int propType = p.userType();
                    if (!QMetaType(propType).isRegistered()) propType = QMetaType::UnknownType;

                    MetaCacheEntry entry{
                        .kind = MetaCacheEntry::Kind::Property,
                        .methodType = 0,
                        .isWritable = p.isWritable(),
                        .isReadable = p.isReadable(),
                        .isResettable = p.isResettable(),
                        .paramCount = 0,
                        .index = i,
                        .type = QuickJSConvert::adjustEnumType(propType),
                        .paramTypes = {},
                    };

                    const bool existed = cache->contains(atom);
                    bool ownedByHash = false;
                    try {
                        cache->insert(atom, entry);
                        ownedByHash = true;
                        if (existed) {
                            cache->methodAtoms.removeOne(atom);
                            JS_FreeAtom(m_ctx, atom);
                        }
                        cache->propertyAtoms.append(atom);
                    } catch (...) {
                        if (!ownedByHash) {
                            JS_FreeAtom(m_ctx, atom);
                        }
                        JS_ThrowOutOfMemory(m_ctx);
                        return JS_EXCEPTION;
                    }
                }
            }

            JSMetaObjectCache* rawCache = cache.release();
            try {
                cacheIt = m_metaObjectCaches.insert(meta, rawCache);
            } catch (...) {
                delete rawCache;
                JS_ThrowOutOfMemory(m_ctx);
                return JS_EXCEPTION;
            }
        }
        data->metaCache = cacheIt.value();

        JSQObjectData* rawData = data.release();

        if (JS_SetOpaque(jsObjScoped.value(), rawData) != 0) {
            delete rawData;
            JS_ThrowInternalError(m_ctx, "Failed to set QObject opaque data");
            return JS_EXCEPTION;
        }

        JSValue valToInsert = JS_DupValue(m_ctx, jsObjScoped.value());
        try {
            m_qobjectWrapperCache.insert(obj, valToInsert);
        } catch (...) {
            JS_FreeValue(m_ctx, valToInsert);
            JS_ThrowOutOfMemory(m_ctx);
            return JS_EXCEPTION;
        }

        rawData->destroyedConn = QObject::connect(obj, &QObject::destroyed, this, [this, obj](QObject*) {
            auto it = m_qobjectWrapperCache.find(obj);
            if (it != m_qobjectWrapperCache.end()) {
                JSValue jsObj = it.value();
                m_qobjectWrapperCache.erase(it);

                auto data = static_cast<JSQObjectData*>(JS_GetOpaque(jsObj, m_qobjectClassID));
                if (data) {
                    data->clearCaches(m_rt);
                    delete data;
                    JS_SetOpaque(jsObj, nullptr);
                }
                JS_FreeValue(m_ctx, jsObj);
            }
        });
        Q_ASSERT(rawData->destroyedConn);

        return jsObjScoped.release();
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(m_ctx, "C++ exception in wrapQObject: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(m_ctx, "Unknown C++ exception in wrapQObject"); } catch (...) {}
        return JS_EXCEPTION;
    }
}
