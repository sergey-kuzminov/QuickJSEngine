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

    JS_SetContextOpaque(m_ctx, this);
    JS_SetRuntimeOpaque(m_rt, this);
    JS_SetInterruptHandler(m_rt, &QQuickJSEngine::interruptHandler, this);

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

    JSValue global = JS_GetGlobalObject(m_ctx);
    JSValue funcCtor = JS_GetPropertyStr(m_ctx, global, "Function");
    JSValue funcProto = JS_GetPropertyStr(m_ctx, funcCtor, "prototype");
    JS_FreeValue(m_ctx, funcCtor);
    JS_FreeValue(m_ctx, global);

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
        JS_FreeValue(m_ctx, e.promise);
        JS_FreeValue(m_ctx, e.reason);
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

    JS_FreeAtom(m_ctx, m_atoms.length);
    JS_FreeAtom(m_ctx, m_atoms.getTime);
    JS_FreeAtom(m_ctx, m_atoms.stack);
    JS_FreeAtom(m_ctx, m_atoms.lineNumber);
    JS_FreeAtom(m_ctx, m_atoms.line);

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
    const char* ctorName = "Error";
    switch (errorType) {
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

    if (engine->m_interrupted.exchange(false)) {
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

void QQuickJSEngine::installExtensions(QQuickJSEngine::Extensions extensions) {
    ensureEngineThread();

    // Deduplicate by the actual property presence, not only by the installed
    // mask: scripts can delete console/gc/qsTr from the global object, and a
    // repeated request must reinstall what is missing.
    auto needsInstall = [this](Extension ext, const char* propName) -> bool {
        if (!m_installedExtensions.testFlag(ext)) return true;
        ScopedJSValue v(m_ctx, JS_GetPropertyStr(m_ctx, m_globalObj, propName));
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
            auto addConsoleMethod = [this](JSValue obj, const char* name, QtMsgType msgType) {
                JSValue func = JS_NewCFunctionMagic(
                    m_ctx,
                    [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) -> JSValue {
                        try {
                            QStringList msgParts;
                            for (int i = 0; i < argc; i++) {
                                QString part = QuickJSConvert::jsToQString(ctx, argv[i]);
                                if (part.isNull()) {
                                    // jsToQString failed and already cleared the
                                    // pending exception; substitute a readable
                                    // fallback instead of printing nothing.
                                    if (JS_IsSymbol(argv[i])) {
                                        ScopedJSValue desc(ctx, JS_GetPropertyStr(ctx, argv[i], "description"));
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
                                msgParts.append(part);
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
                    },
                    name, 1, JS_CFUNC_generic_magic, static_cast<int>(msgType)
                    );

                if (JS_SetPropertyStr(m_ctx, obj, name, func) < 0) {
                    clearPendingException(m_ctx);
                }
            };

            addConsoleMethod(console.value(), "log", QtDebugMsg);
            addConsoleMethod(console.value(), "info", QtDebugMsg);
            addConsoleMethod(console.value(), "debug", QtDebugMsg);
            addConsoleMethod(console.value(), "warn", QtWarningMsg);
            addConsoleMethod(console.value(), "error", QtCriticalMsg);

            if (JS_SetPropertyStr(m_ctx, m_globalObj, "console", console.release()) < 0) {
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

        if (JS_SetPropertyStr(m_ctx, m_globalObj, "gc", func) < 0) {
            clearPendingException(m_ctx);
        }
    }

    if (toInstall.testFlag(TranslationExtension)) {
        JSValue func = JS_NewCFunction(m_ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            try {
                if (argc > 0 && JS_IsString(argv[0])) {
                    QString source = QuickJSConvert::jsToQString(ctx, argv[0]);
                    QString translated = QCoreApplication::translate("QQuickJSEngine", source.toUtf8().constData());
                    return qStringToJS(ctx, translated);
                }
                return JS_UNDEFINED;
            } catch (const std::exception& e) {
                try { JS_ThrowPlainError(ctx, "qsTr: %s", e.what()); } catch (...) {}
                return JS_EXCEPTION;
            } catch (...) {
                try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTr"); } catch (...) {}
                return JS_EXCEPTION;
            }
        }, "qsTr", 1);

        if (JS_SetPropertyStr(m_ctx, m_globalObj, "qsTr", func) < 0) {
            clearPendingException(m_ctx);
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
        JS_FreeValue(ctx, entry.promise);
        JS_FreeValue(ctx, entry.reason);
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

QQuickJSValue QQuickJSEngine::evaluate(const QString& script, const QString& fileName, int lineNumber) {
    ensureEngineThread();

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
                    processAndEmitException(m_ctx, errVal.jsVal());
                    return errVal;
                }
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
        processAndEmitException(m_ctx, errVal.jsVal());
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
        processAndEmitException(m_ctx, errVal.jsVal());
        return errVal;
    }

    return QQuickJSValue(this, retScoped.release(), true);
}

QQuickJSValue QQuickJSEngine::evaluate(const QString& script, const QString& fileName) {
    return evaluate(script, fileName, 1);
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
    case QMetaType::Long: case QMetaType::LongLong: {
        qint64 val = var.toLongLong();
        if (val >= std::numeric_limits<int32_t>::min() && val <= std::numeric_limits<int32_t>::max()) {
            return JS_NewInt32(m_ctx, static_cast<int32_t>(val));
        }
        if (val >= -9007199254740991LL && val <= 9007199254740991LL) {
            return JS_NewFloat64(m_ctx, static_cast<double>(val));
        }
        return JS_NewBigInt64(m_ctx, val);
    }
    case QMetaType::UInt: case QMetaType::UShort: case QMetaType::UChar: {
        quint32 val = var.toUInt();
        if (val <= static_cast<quint32>(std::numeric_limits<int32_t>::max())) return JS_NewInt32(m_ctx, static_cast<int32_t>(val));
        return JS_NewFloat64(m_ctx, static_cast<double>(val));
    }
    case QMetaType::ULong: case QMetaType::ULongLong: {
        quint64 val = var.toULongLong();
        if (val <= static_cast<quint64>(std::numeric_limits<int32_t>::max())) {
            return JS_NewInt32(m_ctx, static_cast<int32_t>(val));
        }
        if (val <= 9007199254740991ULL) {
            return JS_NewFloat64(m_ctx, static_cast<double>(val));
        }
        return JS_NewBigUint64(m_ctx, val);
    }
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
    case QMetaType::QByteArray: {
        const QByteArray ba = var.toByteArray();
        return JS_NewArrayBufferCopy(m_ctx, reinterpret_cast<const uint8_t*>(ba.constData()), static_cast<size_t>(ba.size()));
    }
    case QMetaType::QDateTime: {
        qint64 msecs = var.toDateTime().toMSecsSinceEpoch();
        return JS_NewDate(m_ctx, static_cast<double>(msecs));
    }
    case QMetaType::QDate: {
        QDateTime dt(var.toDate(), QTime(0,0));
        qint64 msecs = dt.toMSecsSinceEpoch();
        return JS_NewDate(m_ctx, static_cast<double>(msecs));
    }
    case QMetaType::QTime: {
        return qStringToJS(m_ctx, var.toTime().toString(Qt::ISODate));
    }
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
    return toVariantImpl(val, visited, 0);
}

QVariant QQuickJSEngine::toVariantImpl(JSValueConst val, JSVisitedStack& visited, int depth) {
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
                result = QuickJSConvert::variantFromJSObject(this, m_ctx, val, m_qobjectClassID, m_objectClassID, visited, depth + 1);
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
    if (metaType == QMetaType::QString) return qStringToJS(m_ctx, *static_cast<QString*>(data));
    if (metaType == QMetaType::QObjectStar) return wrapQObject(*static_cast<QObject**>(data));
    if (metaType == QMetaType::QVariant) return toJSValue(*static_cast<QVariant*>(data));
    if (metaType == QMetaType::UnknownType || metaType == QMetaType::Void) return JS_UNDEFINED;

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
