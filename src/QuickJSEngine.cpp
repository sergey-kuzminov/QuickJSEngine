//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

// Core of QQuickJSEngine (one class, several translation units - see
// QuickJSEngine_p.h): construction/teardown, limits and interrupt handling,
// error objects and exception plumbing, unhandled-rejection bookkeeping,
// evaluate(), the new*/setProperty value factories and QObject wrapping.
// The stack guard lives in QuickJSStackGuard.cpp, console/qsTr extensions
// in QuickJSExtensions.cpp, ES modules in QuickJSModules.cpp and the
// QVariant conversion ladders in QuickJSEngineConvert.cpp.

#include "QuickJSEngine_p.h"
#include "QuickJSConvert.h"
#include "QuickJSQObjectProxy.h"
#include "QuickJSSignalProxy.h"

#include <QDebug>
#include <QScopeGuard>
#include <QStringList>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

// Ceiling for the unhandled-rejection buffer (m_pendingRejections). The
// buffer only drains at the end of a tick when no jobs are pending; with
// setAutoProcessPromises(false) and a never-drained job queue it grows
// without bound (each entry pins a promise and its reason on the JS heap,
// a leak the JSMemoryUsage slope of the leakgate sees - "stuck job +
// fresh rejections" regime). 1024 entries matches the order
// of the other engine caches (kConsoleLabelLimit etc.); past the ceiling
// the OLDEST buffered entry is evicted and reported immediately to make
// room (see queueUnhandledRejection).
constexpr int kMaxPendingRejections = 1024;
// The engine's interrupt heartbeat, in counted operations per poll: the
// interpreter calls the interrupt handler after every 10000 counted
// operations (branch opcodes, function entries, polled builtin loop
// iterations - quickjs.c JS_INTERRUPT_COUNTER_INIT / js_poll_interrupts),
// and the regex engine after every 10000 backtracking steps (libregexp.c
// INTERRUPT_COUNTER_INIT / lre_poll_timeout -> lre_check_timeout, wired to
// the same handler). One "step" of setStepBudget() is one such counted
// operation; the accounting in interruptHandler() charges one full poll
// interval per handler invocation.
constexpr quint64 kStepPollInterval = 10000;

// Constructor failure paths free the context before throwing; the atoms
// and intrinsics captured so far must go with it, or JS_FreeRuntime's
// leak check aborts a debug build (a throwing constructor never reaches
// the destructor's cleanup).
void releaseStartupInternals(JSContext* ctx, JSCommonAtoms& atoms, JSIntrinsics& intrinsics)
{
    JS_FreeAtoms(ctx, atoms.length, atoms.stack,
                 atoms.lineNumber, atoms.line, atoms.name);
    for (JSValue& p : intrinsics.errorProtos) {
        JS_FreeValue(ctx, p);
        p = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, intrinsics.dateGetTime);
    intrinsics.dateGetTime = JS_UNDEFINED;
}
} // namespace

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
        // The tracker runs on pure C frames of the job/GC machinery; an
        // escaping C++ exception would unwind through them. Report-loss in
        // the (OOM) failure regime is the only option a void callback has.
        try {
            // quickjs-ng reports a rejection as unhandled at the reject moment and
            // retracts it (is_handled=true) when a handler attaches later in the
            // same tick. Buffering instead of reporting immediately avoids false
            // uncaughtException for rejections handled by a late .catch().
            if (is_handled) {
                engine->retractHandledRejection(ctx, promise);
            } else {
                engine->queueUnhandledRejection(ctx, promise, reason);
            }
        } catch (...) {
            // swallowed: see above
        }
    }, this);

    m_atoms.length = JS_NewAtom(m_ctx, "length");
    m_atoms.stack = JS_NewAtom(m_ctx, "stack");
    m_atoms.lineNumber = JS_NewAtom(m_ctx, "lineNumber");
    m_atoms.line = JS_NewAtom(m_ctx, "line");
    m_atoms.name = JS_NewAtom(m_ctx, "name");
    // OOM policy for the startup atoms (deliberate degradation, no throw):
    // quickjs is out of memory by the time JS_NewAtom returns JS_ATOM_NULL,
    // and a throwing constructor would strand the already-created context.
    // The engine survives with degraded services instead: stack traces lose
    // their "stack" frames, error line numbers fall back to the parsed
    // "line" property - both diagnostics, not control flow. The warning is
    // the operator's only signal.
    if (Q_UNLIKELY(m_atoms.length == JS_ATOM_NULL || m_atoms.stack == JS_ATOM_NULL
                   || m_atoms.lineNumber == JS_ATOM_NULL || m_atoms.line == JS_ATOM_NULL
                   || m_atoms.name == JS_ATOM_NULL)) {
        qWarning("QQuickJSEngine: out of memory allocating engine atoms - "
                 "stack traces and error line numbers will be degraded");
    }

    // Capture the intrinsics (see JSIntrinsics) before anything script-visible
    // can change them: errorType() then classifies by prototype identity
    // instead of the mutable 'name' property, and the Date conversion calls
    // the captured getTime instead of whatever the instance carries. A failed
    // lookup (exotic context) leaves JS_UNDEFINED entries, which the consumers
    // treat as "no match / not captured"; no lookup may leave a pending
    // exception behind for the first evaluate().
    for (JSValue& p : m_intrinsics.errorProtos) p = JS_UNDEFINED;
    {
        static const char* const kErrorCtors[] = {
            "Error", "EvalError", "RangeError", "ReferenceError",
            "SyntaxError", "TypeError", "URIError",
        };
        ScopedJSValue global(m_ctx, JS_GetGlobalObject(m_ctx));
        for (size_t i = 0; i < std::size(kErrorCtors); ++i) {
            ScopedJSValue ctor(m_ctx, JS_GetPropertyStr(m_ctx, global.value(), kErrorCtors[i]));
            if (JS_IsFunction(m_ctx, ctor.value())) {
                ScopedJSValue proto(m_ctx, JS_GetPropertyStr(m_ctx, ctor.value(), "prototype"));
                if (JS_IsObject(proto.value()))
                    m_intrinsics.errorProtos[i] = JS_DupValue(m_ctx, proto.value());
            }
        }
        ScopedJSValue dateCtor(m_ctx, JS_GetPropertyStr(m_ctx, global.value(), "Date"));
        if (JS_IsFunction(m_ctx, dateCtor.value())) {
            ScopedJSValue proto(m_ctx, JS_GetPropertyStr(m_ctx, dateCtor.value(), "prototype"));
            if (JS_IsObject(proto.value())) {
                JSValue fn = JS_GetPropertyStr(m_ctx, proto.value(), "getTime");
                if (JS_IsFunction(m_ctx, fn)) {
                    m_intrinsics.dateGetTime = fn;   // owns the reference
                } else {
                    JS_FreeValue(m_ctx, fn);
                }
            }
        }
        clearPendingException(m_ctx);
    }

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
        releaseStartupInternals(m_ctx, m_atoms, m_intrinsics);
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
        releaseStartupInternals(m_ctx, m_atoms, m_intrinsics);
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
        releaseStartupInternals(m_ctx, m_atoms, m_intrinsics);
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
        releaseStartupInternals(m_ctx, m_atoms, m_intrinsics);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create SignalProxy class");
    }

    JSValue signalProto = JS_NewObject(m_ctx);
    if (JS_IsException(signalProto)) {
        JS_FreeValue(m_ctx, funcProto);
        releaseStartupInternals(m_ctx, m_atoms, m_intrinsics);
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

    // Stack guard initialization comes LAST, on the fully constructed
    // engine: calibrateMachineryAllowance() evaluates real JS (its probe
    // script), and the probe must not meet a half-initialized engine - the
    // opaque pointers, the interrupt handler, the module loader, the class
    // table and the global object must all be live before any JS runs. The
    // earlier placement "worked" only because the probe was self-contained;
    // any future probe that touches engineFromContext(), a QObject proxy or
    // console would have dereferenced null. calibrateMachineryAllowance()
    // installs its own provisional guard for the probe, so no pre-anchor is
    // needed; the final reanchorStackGuard() then applies the calibrated
    // budget (512 KiB JS recursion share + machinery allowance, see
    // reanchorStackGuard()) anchored at the constructor-depth entry.
    calibrateMachineryAllowance();
    reanchorStackGuard();
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

    releaseStartupInternals(m_ctx, m_atoms, m_intrinsics);

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
void QQuickJSEngine::setStepBudget(quint64 maxSteps) {
    // Self-contained like the timeout: enabling the budget mid-run applies
    // from the next poll on (steps executed while no budget was set were not
    // counted and do not count against the new quota); disabling stops the
    // accounting. The quota itself is re-armed fresh at every top-level
    // entry (resetExecutionState()).
    m_stepBudget.store(maxSteps, std::memory_order_relaxed);
}
void QQuickJSEngine::setMemoryLimit(size_t limit) {
    ensureEngineThread();
    if (m_rt) JS_SetMemoryLimit(m_rt, limit);
}

void QQuickJSEngine::setAutoProcessPromises(bool enabled) {
    ensureEngineThread();
    m_autoProcessPromises = enabled;
}

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
    ensureEngineThread();
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

namespace {
// The "%s" C error constructors stop at an embedded NUL, while error
// messages CAN carry one (QAnyStringView over NUL-bearing data; QJSEngine
// keeps the full payload). Restore it through a plain data-property write -
// the same way quickjs-ng's own error constructors install the message; no
// script runs. JS_SetPropertyStr frees the value on every path (verified in
// the vendored source), so a failure only leaves the truncated message.
void restoreFullErrorMessage(JSContext* ctx, JSValue err, const QString& message)
{
    const QByteArray utf8 = message.toUtf8();
    if (!utf8.contains('\0')) return;
    JSValue msg = qStringToJS(ctx, message);
    if (JS_IsException(msg)) {
        clearPendingException(ctx);
        return;
    }
    if (JS_SetPropertyStr(ctx, err, "message", msg) < 0) {
        clearPendingException(ctx);
    }
}
} // namespace

QQuickJSValue QQuickJSEngine::newErrorObject(QQuickJSValue::ErrorType errorType, QAnyStringView message) {
    ensureEngineThread();
    if (errorType == QQuickJSValue::NoError) {
        // QJSEngine parity: NoError yields an undefined value, not an Error.
        return QQuickJSValue();
    }

    // Materialize the message once (N1, r14): all three constructor
    // branches below need the QString (and most the UTF-8) form.
    const QString msg = message.toString();

    // quickjs-ng provides C constructors with the built-in prototypes for
    // these five classes: engine-created errors stay immune to scripts
    // replacing globalThis.TypeError etc. (QJSEngine parity — V4 also uses
    // its internal prototypes). EvalError/URIError have no C helpers in
    // quickjs-ng 0.18 and keep the global-constructor fallback below.
    if (errorType != QQuickJSValue::EvalError && errorType != QQuickJSValue::URIError) {
        // "%s" stops at an embedded NUL; error messages are not expected
        // to contain one.
        const QByteArray utf8 = msg.toUtf8();
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
        restoreFullErrorMessage(m_ctx, err, msg);
        QQuickJSValue result(this, err, true);
        result.m_isError = true;
        return result;
    }

    // EvalError/URIError land here (no C helpers). The fallback resolves
    // the global constructor and CALLS it (JS_CallConstructor) - script-
    // visible execution, the only such branch in this function. The call
    // therefore needs the full entry bookkeeping (fresh timeout/step quota,
    // guard re-anchor, rejection flush on exit) - without it, a
    // throwError(EvalError, ...) from C++ between runs under an enabled
    // setExecutionTimeoutMs would inherit the previous run's expired
    // deadline and abort with a non-cached InternalError "interrupted"
    // instead of the requested error class. On a starved stack that is
    // exactly what the entry refusal prevents (script execution under the
    // emergency budget, where even the throw path may not fit); degrade to
    // the plain C-constructed error instead (GenericError class,
    // engine-internal prototype, message preserved): an error's class is
    // metadata the host branches on, the message is the payload, and the C
    // path can neither recurse nor run script.
    if (Q_UNLIKELY(m_stackStarved)) {
        const QByteArray utf8 = msg.toUtf8();
        JSValue err = JS_NewPlainError(m_ctx, "%s", utf8.constData());
        if (JS_IsException(err)) return QQuickJSValue::fromException(this);
        restoreFullErrorMessage(m_ctx, err, msg);
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

    // Script-visible execution below (the global constructor call): full
    // entry bookkeeping. Nested (depth > 0) this is a pair of integer ops.
    JsEntryScope entryScope(this);

    ScopedJSValue global(m_ctx, JS_GetGlobalObject(m_ctx));
    ScopedJSValue ctor(m_ctx, JS_GetPropertyStr(m_ctx, global.value(), ctorName));
    if (consumeException(m_ctx, ctor.value())) return QQuickJSValue();

    ScopedJSValue msgVal(m_ctx, qStringToJS(m_ctx, msg));
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

    // Step budget (the deterministic complement to the wall-clock deadline
    // above): each invocation of this handler marks one full poll interval
    // of executed work (kStepPollInterval counted operations, interpreter or
    // regex - whichever machinery polled). Skipped entirely when no budget
    // is set: zero overhead for runs that do not use the API.
    const quint64 stepBudget = engine->m_stepBudget.load(std::memory_order_relaxed);
    if (stepBudget > 0) {
        const quint64 used = engine->m_stepsUsed.fetch_add(kStepPollInterval, std::memory_order_relaxed)
                             + kStepPollInterval;
        if (used >= stepBudget) {
            return 1;
        }
    }
    return 0;
}

QQuickJSValue QQuickJSEngine::globalObject() const {
    ensureEngineThread();
    return QQuickJSValue(const_cast<QQuickJSEngine*>(this), m_globalObj, false);
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

    // Guard diagnostics (QJS_GUARD_TRACE=1): when a stack overflow surfaces,
    // report the catch point relative to the anchor/line. NOTE: this SP is
    // where the exception reached the host catch frame AFTER the interpreter
    // unwound - the guard's fire point itself was at (or below) the line and
    // necessarily deeper than the value printed here, so this number can only
    // bound the fire depth from below; it cannot prove a false fire (a
    // near-anchor catch point can be misread as "fired inside the machinery
    // allowance" while the actual cause is a degenerate budget window, see
    // the warning in reanchorStackGuard()).
    if (guardTraceEnabled()
            && errStr.contains(QLatin1String("Maximum call stack size exceeded"))) {
        const uintptr_t sp = currentStackPointer();
        const qint64 belowAnchor = m_guardAnchorSp > sp
            ? qint64(m_guardAnchorSp - sp) : -qint64(sp - m_guardAnchorSp);
        qWarning("QQuickJSEngine: [guard] stack overflow caught at SP=%llx: %lld KiB "
                 "below the entry anchor; guard line %lld KiB below it (stackSize=%zu KiB); "
                 "the fire point was at or below the line, deeper than the catch point",
                 static_cast<unsigned long long>(sp),
                 static_cast<long long>(belowAnchor / 1024),
                 static_cast<long long>(belowAnchor / 1024
                     - static_cast<qint64>(m_guardStackSize) / 1024),
                 m_guardStackSize / 1024);
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
        // Soft ceiling (kMaxPendingRejections): the buffer only drains at the
        // end of a tick with an empty job queue, so auto-processing disabled
        // plus a never-drained job means unbounded growth. Make room by
        // evicting the OLDEST entry and reporting it immediately: the oldest
        // has had the longest retraction window, every rejection is still
        // reported exactly once - the report just arrives earlier than
        // end-of-tick for overflow entries (a late .catch() on an already
        // evicted promise can no longer retract it; pathological regime
        // either way). Reporting may re-enter this function (the reason's
        // toString can reject promises): the evicted slot is already free at
        // that point, so the re-entrant append fits and the list stays
        // bounded (worst case one entry over the ceiling until the next
        // rejection evicts again).
        if (m_pendingRejections.size() >= kMaxPendingRejections) {
            if (Q_UNLIKELY(!warnOnce(m_rejectionCapWarned))) {
                qWarning("QQuickJSEngine: unhandled promise rejection buffer full (%d entries); "
                         "reporting the oldest buffered rejection immediately - "
                         "setAutoProcessPromises(false) with pending jobs is holding the buffer",
                         kMaxPendingRejections);
            }
            PendingRejection evicted = m_pendingRejections.takeFirst();
            processAndEmitException(ctx, evicted.reason);
            JS_FreeValues(ctx, evicted.promise, evicted.reason);
        }
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

// Shared tail of both evaluate() execution paths - the eval-cache hit and
// the freshly compiled run (S2, r14): takes the raw JS_EvalFunction
// result, wraps it exception-safely, drains the microtask queue on
// top-level entries (run-to-completion), reports an uncaught exception
// and applies the sticky-interrupt check. One copy instead of two that
// used to drift apart comment by comment.
QQuickJSValue QQuickJSEngine::finishEvaluationRun(JSValue ret, bool topLevel,
                                                  QStringList* exceptionStackTrace) {
    ScopedJSValue retScoped(m_ctx, ret);

    const bool hadException = JS_IsException(retScoped.value());
    QQuickJSValue errVal;
    if (hadException) errVal = QQuickJSValue::fromException(this);

    // Run-to-completion: only a top-level entry drains the microtask queue;
    // a nested evaluate (from a getter or a host slot below a running
    // script) leaves the drain to the outer entry, like V4.
    if (topLevel && m_autoProcessPromises) processPendingJobs();

    if (hadException) {
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
    }
    // QJSEngine parity: a sticky interrupt flag yields an "Interrupted"
    // error object even when the script completed without hitting an
    // interrupt check.
    if (isInterrupted()) {
        return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
    }
    if (hadException) return errVal;
    return QQuickJSValue(this, retScoped.release(), true);
}

QQuickJSValue QQuickJSEngine::evaluate(const QString& script, const QString& fileName,
                                       int lineNumber, QStringList* exceptionStackTrace) {
    ensureEngineThread();
    if (exceptionStackTrace) exceptionStackTrace->clear();

    // Anchor the stack guard (and the fresh timeout quota) before anything
    // else: reporting a stale pending exception below also runs JS
    // (Error.prototype.toString in the uncaught report), which must happen
    // below a guard measured from THIS entry, not from a stale anchor.
    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
        // Starved entry: the usable window below this anchor is inside the
        // guard's safety margin, so even the C++ exception-throw path may
        // not fit below it. Fail fast with the same catchable RangeError the
        // stack guard raises, BEFORE entering JS (engine compile/run or
        // the pending-exception report below) - the engine is unusable on
        // such a stack either way, but the process must stay alive.
        if (Q_UNLIKELY(m_stackStarved)) {
            // A pending exception preempts the refusal exactly like it
            // preempts the evaluation itself: consume it through the pure C
            // path (JS_GetException runs no script - the guarantee this
            // refusal makes) and return it instead of the RangeError, so the
            // error does not stay pending behind every subsequent starved
            // entry. The full report (stack collection, uncaughtException)
            // stays skipped: it runs JS and needs the stack this entry
            // refused. With nothing pending, the refusal RangeError itself.
            if (JS_HasException(m_ctx)) {
                return QQuickJSValue::fromException(this);
            }
            return newErrorObject(QQuickJSValue::RangeError,
                                   QStringLiteral("Maximum call stack size exceeded"));
        }
    }

    // Entry bookkeeping BEFORE the pending-exception report below: the
    // report itself runs user JS - a hostile "stack" getter, an overridden
    // Error.prototype.toString, or a C++ slot behind uncaughtException() can
    // re-enter the engine. Without the depth guard such a re-entrant entry
    // sees m_evalDepth == 0, treats itself as a fresh top level and calls
    // resetExecutionState() mid-JS: the stack guard would re-anchor below the
    // already-running JS frames (the exact hazard reanchorStackGuard()'s
    // m_evalDepth check exists to prevent) and the timeout/step quota of the
    // still-active report would be reset. The rejection flush is declared
    // after depthGuard so the flush (which may run user JS via reason
    // toString) happens while the depth guard is still held. Top-level calls
    // only: nested evaluates must not report the enclosing tick.
    EvalDepthGuard depthGuard(m_evalDepth);
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

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

    if (m_evalCacheEnabled && lineNumber == 1) {
        for (int i = 0; i < m_evalCache.size(); ++i) {
            if (m_evalCache[i].script == script && m_evalCache[i].fileName == fileName) {
                m_evalCache.move(i, 0);
                const EvalCacheEntry& entry = m_evalCache.first();

                JSValue bc = JS_DupValue(m_ctx, entry.bytecode);
                return finishEvaluationRun(JS_EvalFunction(m_ctx, bc), topLevel, exceptionStackTrace);
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

    return finishEvaluationRun(JS_EvalFunction(m_ctx, bc), topLevel, exceptionStackTrace);
}

void QQuickJSEngine::setProperty(const char* name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (!name) return;
    if (value.m_engine.data() != this) {
        if (!(value.m_engine.isNull() && JS_IsUndefined(value.m_val))) return;
    }

    // JS_SetPropertyStr runs user JS when the global object holds an
    // accessor property (a script-installed setter): entry bookkeeping
    // applies, like in the QString overload.
    JsEntryScope entryScope(this);

    JSValue v = JS_DupValue(m_ctx, value.jsVal());
    if (JS_SetPropertyStr(m_ctx, m_globalObj, name, v) < 0) {
        clearPendingException(m_ctx);
    }
}
void QQuickJSEngine::setProperty(const QByteArray& name, const QQuickJSValue& value) {
    // Route through the QString overload (atom cache): a QByteArray with an
    // embedded NUL would be truncated at constData(), installing a property
    // whose name stops at the NUL byte.
    setProperty(QString::fromUtf8(name), value);
}
void QQuickJSEngine::setProperty(QStringView name, const QQuickJSValue& value) { setProperty(name.toString(), value); }
// QString and QByteArray names go through the shared string->atom cache:
// this skips the engine atom-table probe on every call and keeps embedded
// NULs intact. const char* stays on JS_SetPropertyStr, which is already
// allocation-free (a C string cannot contain a NUL by definition).
void QQuickJSEngine::setProperty(const QString& name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (value.m_engine.data() != this) {
        if (!(value.m_engine.isNull() && JS_IsUndefined(value.m_val))) return;
    }

    // JS_SetProperty on the global object runs user JS when the property
    // is a script-installed accessor (Object.defineProperty(globalThis,
    // "x", {set: ...})): the setter needs the fresh quota and the
    // re-anchored guard of a real entry, and a re-entrant engine call from
    // inside it must see a nested depth, not a fresh top level
    // (QQuickJSValue::setProperty has kept the same scope since D-57).
    JsEntryScope entryScope(this);

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
    // Symmetric with newObject/newSymbol/newArray/newFunction: a failed wrap
    // surfaces as an error value instead of an invalid one with a silently
    // pending exception.
    JSValue v = wrapQObject(obj);
    if (JS_IsException(v)) return QQuickJSValue::fromException(this);
    return QQuickJSValue(this, v, true);
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
            // A failed length install leaves a silently truncated array;
            // surface the error instead (symmetric with the other new* APIs).
            return QQuickJSValue::fromException(this);
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

// Engine-wide metaobject cache lookup/build (L-12): returns the cache for
// the metaobject, building it on first sight. nullptr means the build
// failed with a pending JS exception (OOM). Callers: wrapQObject() and the
// proxy's runtime metaobject-change refresh.
JSMetaObjectCache* QQuickJSEngine::metaObjectCacheFor(const QMetaObject* meta) {
    auto cacheIt = m_metaObjectCaches.find(meta);
    if (cacheIt != m_metaObjectCaches.end() && cacheIt.value()->className != meta->className()) {
        // The address was reused by a different QMetaObject (dynamic
        // metaobjects can be freed while the cache entry survives):
        // rebuild instead of serving a foreign cache.
        delete cacheIt.value();
        m_metaObjectCaches.erase(cacheIt);
        cacheIt = m_metaObjectCaches.end();
    }
    if (cacheIt != m_metaObjectCaches.end()) return cacheIt.value();

    auto cache = std::make_unique<JSMetaObjectCache>(m_ctx);
    cache->className = meta->className();
    const int methodCount = meta->methodCount();
    const int propCount = meta->propertyCount();

    cache->entries.reserve(methodCount + propCount);
    cache->methodAtoms.reserve(methodCount);
    cache->propertyAtoms.reserve(propCount);

    for (int i = 0; i < methodCount; ++i) {
        QMetaMethod m = meta->method(i);
        if (m.access() == QMetaMethod::Private) continue;

        const QByteArray name = m.name();
        JSAtom atom = atomFromUtf8(m_ctx, name);
        if (atom == JS_ATOM_NULL) { JS_ThrowOutOfMemory(m_ctx); return nullptr; }
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
            return nullptr;
        }
    }

    for (int i = 0; i < propCount; ++i) {
        QMetaProperty p = meta->property(i);
        if (p.isReadable() || p.isWritable()) {
            JSAtom atom = JS_NewAtom(m_ctx, p.name());
            if (atom == JS_ATOM_NULL) {
                JS_ThrowOutOfMemory(m_ctx);
                return nullptr;
            }

            int propType = p.userType();
            if (!QMetaType(propType).isRegistered()) propType = QMetaType::UnknownType;

            MetaCacheEntry entry{
                .kind = MetaCacheEntry::Kind::Property,
                .methodType = 0,
                .isWritable = p.isWritable(),
                .isReadable = p.isReadable(),
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
                return nullptr;
            }
        }
    }

    JSMetaObjectCache* rawCache = cache.release();
    try {
        m_metaObjectCaches.insert(meta, rawCache);
    } catch (...) {
        delete rawCache;
        JS_ThrowOutOfMemory(m_ctx);
        return nullptr;
    }
    return rawCache;
}

JSValue QQuickJSEngine::wrapQObject(QObject* obj) {
    ensureEngineThread();

    if (!obj) return JS_NULL;

    try {
        auto it = m_qobjectWrapperCache.find(obj);
        if (it != m_qobjectWrapperCache.end()) {
            // Address-reuse guard (the destroyed hook is delivered QUEUED
            // when the QObject lived in another thread, so a stale entry can
            // survive until its event arrives): a cached wrapper whose
            // QPointer no longer points at THIS object is a corpse of a
            // previously destroyed object that happened to share the
            // address. Serving it would report a live object as deleted.
            // Drop the stale entry and re-wrap; the pending hook of the old
            // object then finds either nothing or the fresh entry, and its
            // liveness check (below) keeps it from touching the fresh one.
            const auto staleData = static_cast<JSQObjectData*>(JS_GetOpaque(it.value(), m_qobjectClassID));
            if (Q_UNLIKELY(!staleData || staleData->obj.data() != obj)) {
                JS_FreeValue(m_ctx, it.value());
                m_qobjectWrapperCache.erase(it);
            } else {
                return JS_DupValue(m_ctx, it.value());
            }
        }

        auto data = std::make_unique<JSQObjectData>();
        data->obj = obj;

        ScopedJSValue jsObjScoped(m_ctx, JS_NewObjectClass(m_ctx, m_qobjectClassID));
        if (JS_IsException(jsObjScoped.value())) return JS_EXCEPTION;

        const QMetaObject* meta = obj->metaObject();
        data->metaObj = meta;
        JSMetaObjectCache* metaCache = metaObjectCacheFor(meta);
        if (!metaCache) return JS_EXCEPTION;
        data->metaCache = metaCache;

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

                auto data = static_cast<JSQObjectData*>(JS_GetOpaque(jsObj, m_qobjectClassID));
                // Address-reuse guard: after the stale-entry cleanup in
                // wrapQObject(), the cache slot for this address may already
                // hold the wrapper of a NEW live object (the old one was
                // destroyed and the address reused before this queued
                // delivery ran). Only clean up when the found wrapper's
                // QPointer is dead (ours, or another dead wrapper at the
                // same address - the cleanup is idempotent for both).
                if (data && !data->obj.isNull()) return;

                m_qobjectWrapperCache.erase(it);
                if (data) {
                    // Drop the heavy per-wrapper caches (method proxies hold
                    // JSValues) but KEEP the wrapper a QObject wrapper with
                    // its opaque data: isQObject() then stays true for the
                    // zombie, like QJSEngine (V4 keeps the wrapper class
                    // after the C++ object died; toQObject() returns nullptr
                    // through the nulled QPointer). The exotic handlers'
                    // obj.isNull() checks route every access to the "QObject
                    // has been deleted" TypeError, and the JS-object
                    // finalizer deletes the data for good.
                    data->clearCaches(m_rt);
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
