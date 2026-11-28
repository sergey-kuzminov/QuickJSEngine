//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

/**
 * Naming: the project is "QuickJSEngine" (repository, README, tarball);
 * public Qt classes carry the Q prefix per Qt convention, cf. QQmlEngine
 * for the QtQml module: QQuickJSEngine, QQuickJSValue,
 * QQuickJSValueIterator. Headers are named after the project module
 * (QuickJSEngine.h, QuickJSValue.h), not after the classes. Helpers
 * without the prefix (QuickJSConvert, QuickJSQObjectProxy,
 * QuickJSSignalProxy, JsErrorTranslator) are internal implementation
 * details, not public API - with one documented exception: the host-side
 * error-translation helper translateJsError() (JsErrorTranslator.h),
 * see the README.
 *
 * @note JavaScript does not support C++ method overloading by type.
 * If a QObject has overloaded Q_INVOKABLE methods or signals,
 * only the first one encountered will be exposed to JavaScript.
 * Provide unique names for methods intended to be called from JS.
 */

#ifndef QUICKJSENGINE_H
#define QUICKJSENGINE_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <chrono>
#include <limits>
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
// Default JS recursion budget of the stack guard, in bytes (see
// QQuickJSEngine::setMaxStackSize and reanchorStackGuard()).
constexpr size_t DefaultStackBudget = 512 * 1024;

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

// One imported file module. "instantiated" records that the def has been
// driven through JS_EvalFunction at least once (linking done): only then is
// JS_GetModuleNamespace() safe to call on it. A module loaded through the
// loader callback during a failed import chain sits in the cache compiled
// but not yet instantiated.
struct FileModuleEntry {
    JSModuleDef* def = nullptr;
    bool instantiated = false;
};

struct JSCommonAtoms {
    JSAtom length = JS_ATOM_NULL;
    JSAtom stack = JS_ATOM_NULL;
    JSAtom lineNumber = JS_ATOM_NULL;
    JSAtom line = JS_ATOM_NULL;
    JSAtom name = JS_ATOM_NULL;
};

// Intrinsics captured in the constructor, before any script runs: the error
// constructors' prototypes and Date.prototype.getTime. Scripts can replace
// global constructors, mutate a thrown error's 'name' property or override
// d.getTime AFTER construction; classification and conversion that compare
// against (or call) the CAPTURED references stay immune - matching QJSEngine,
// which branches on the internal error class and reads the internal date
// slot instead of the visible properties. errorProtos is indexed:
// 0 Error, 1 EvalError, 2 RangeError, 3 ReferenceError, 4 SyntaxError,
// 5 TypeError, 6 URIError.
struct JSIntrinsics {
    JSValue errorProtos[7];
    JSValue dateGetTime = JS_UNDEFINED;
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
 * Default resource limits (all adjustable, see the setter documentation):
 * the JS heap is capped at 64 MiB (setMemoryLimit()), and the JS recursion
 * budget is 512 KiB plus a calibrated C machinery allowance, clamped to the
 * real bounds of the engine thread's stack (setMaxStackSize()); the
 * execution timeout and the step budget are OFF by default.
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
    friend class QQuickJSValueIterator;
    friend struct JSQObjectData;
    friend class QuickJSQObjectProxy;
    friend class QuickJSSignalProxy;
    friend class QuickJSConvert;
    // The shared entry-bookkeeping RAII used by the friends above.
    friend class JsEntryScope;

    explicit QQuickJSEngine(QObject* parent = nullptr);
    ~QQuickJSEngine();

    QQuickJSValue evaluate(const QString& script, const QString& fileName = QString(),
                           int lineNumber = 1, QStringList* exceptionStackTrace = nullptr);

    /**
     * @brief Imports the ECMAScript module located at @p fileName.
     *
     * QJSEngine-compatible singleton semantics: the first import loads and
     * evaluates the module (local file or Qt resource ":/..." path, UTF-8);
     * subsequent imports of the same file - including imports issued from
     * other modules by relative specifier - return the already-imported
     * instance. The file name is normalized with QFileInfo::canonicalFilePath()
     * for the cache key. On error (missing file, syntax error, an exception
     * thrown by the module code) the error value is returned.
     *
     * Known divergences from QJSEngine are documented in the README: error
     * message texts differ, and circular imports follow the ECMAScript
     * specification (allowed) instead of failing like V4.
     */
    QQuickJSValue importModule(const QString& fileName);

    /**
     * @brief Makes @p value importable from JavaScript under @p moduleName.
     *
     * QJSEngine-compatible registration: importers use the bare specifier
     * exactly as registered ("import { a } from \"api.mjs\""), from any
     * importer location; relative "./..." specifiers always resolve to files.
     * The value itself serves as the default export; its own enumerable
     * properties are snapshot (names and values, getters evaluated) at
     * registration time and become the named exports. Non-object values only
     * provide the default export. Re-registration with the same name replaces
     * the previous registration. Returns false when the value does not belong
     * to this engine or when a snapshot read throws.
     *
     * Re-registration replaces the module for NEW imports; the replaced
     * internal module record stays alive until the engine is destroyed
     * (quickjs-ng keeps module defs in a runtime-owned list with no public
     * free API), so avoid unbounded re-registration loops.
     */
    bool registerModule(const QString& moduleName, const QQuickJSValue& value);

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
    QQuickJSValue newSymbol(const QString& name);
    QQuickJSValue newArray(uint length = 0);
    QQuickJSValue newFunction(JSCFunction* func, const char* name, int argc);

    // Raw-value conversion entry: the only raw JSValue API kept public - a
    // meaningful low-level conversion for hosts that already hold JSValues
    // (benchmarks, FFI-style glue). Everything else that shuffles raw
    // JSValues is private machinery, reachable by the friend proxies.
    JSValue toJSValue(const QVariant& var);

    // QJSEngine-compatible conversion entry points. toScriptValue wraps any
    // registered metatype into a script value; fromScriptValue converts back
    // and requires a default-constructible T. Script values that cannot be
    // represented (functions, RegExp, ...) yield a default T.
    template<typename T>
    QQuickJSValue toScriptValue(const T& value) {
        JSValue v = toJSValueImpl(QVariant::fromValue(value), 0);
        if (JS_IsException(v)) return QQuickJSValue::fromException(this);
        return QQuickJSValue(this, v, true);
    }

    template<typename T>
    T fromScriptValue(const QQuickJSValue& value) {
        if constexpr (std::is_same_v<std::remove_cv_t<T>, QQuickJSValue>) {
            return value;
        } else if constexpr (std::is_same_v<T, QVariant>) {
            return value.toVariant();
        } else {
            QVariant v = value.toVariant();
            // QMetaType converts floating-point variants to integral types by
            // rounding to nearest, while QJSEngine truncates toward zero
            // (ToInteger semantics); mirror the reference behavior.
            if constexpr (std::is_integral_v<T>) {
                if (v.userType() == QMetaType::Double || v.userType() == QMetaType::Float) {
                    const double d = v.toDouble();
                    // The upper bound cast rounds up for 64-bit types, hence
                    // '<' to keep the in-range static_cast defined.
                    if (d >= static_cast<double>(std::numeric_limits<T>::min())
                        && d < static_cast<double>(std::numeric_limits<T>::max())) {
                        return static_cast<T>(d);
                    }
                }
            }
            return v.value<T>();
        }
    }

    // const_cast is safe: QPointer requires a non-const T*, but QQuickJSValue does not modify the engine when owned=false.
    QQuickJSValue globalObject() const;

    enum Extension : quint32 {
        // Values match QJSEngine::Extension so numeric combinations stay
        // portable between the engines.
        TranslationExtension = 0x1,
        ConsoleExtension = 0x2,
        GarbageCollectionExtension = 0x4,
        AllExtensions = 0xFFFFFFFF
    };
    Q_DECLARE_FLAGS(Extensions, Extension)
    /**
     * @brief Installs the selected extensions onto @p object (or the global
     * object). Values owned by another engine are rejected with a warning.
     */
    void installExtensions(Extensions extensions = AllExtensions,
                          const QQuickJSValue& object = QQuickJSValue());

    /**
     * @brief Interrupts or re-enables JavaScript execution.
     *
     * Thread-safe. QJSEngine-compatible semantics: while the flag is set, any
     * JavaScript execution (evaluate(), calls, signal callbacks) aborts
     * immediately and yields an "Interrupted" error object; the flag stays set
     * until this function is called again with @c false. The execution timeout
     * (setExecutionTimeoutMs) is unaffected: it remains a per-run quota.
     */
    void setInterrupted(bool interrupted);

    /**
     * @brief Sets the maximum execution time for JavaScript scripts.
     * @param ms Timeout in milliseconds. 0 disables the timeout.
     *
     * Thread-safe: the deadline is stored atomically. Enabling the timeout
     * while a script is already running (e.g. from a watchdog thread or a
     * C++ slot invoked by the script) grants a fresh full quota immediately;
     * a stale deadline from a previous run is never used. The quota covers
     * the whole run: the script plus the promise jobs drained at its end.
     *
     * @note This timeout is cooperative and polled, not preemptive. The
     * engine's interrupt heartbeat reaches the interpreter (checked every
     * 10000 branch opcodes and function entries) and the regex engine
     * (every 10000 backtracking steps), so on those paths the deadline
     * fires within microseconds of passing. Work that does not poll cannot
     * be interrupted: bulk C-level operations on large strings and
     * TypedArrays, JSON.parse/stringify, GC pauses, and long-blocking C++
     * methods invoked from JS (via Q_INVOKABLE or signals). Keep those
     * bounded, or check QQuickJSEngine::isInterrupted() periodically inside
     * your C++ loops.
     */
    void setExecutionTimeoutMs(quint32 ms);

    /**
     * @brief Sets the per-run budget of executed engine steps.
     * @param maxSteps Maximum number of steps; 0 disables the budget
     * (the default).
     *
     * A deterministic complement to setExecutionTimeoutMs(): the wall-clock
     * timeout depends on machine speed, the step budget does not. One
     * "step" is one counted operation of the engine's interrupt heartbeat:
     * an executed interpreter branch or function entry, a polled builtin
     * loop iteration (array iteration, prototype-chain walks), or a regex
     * backtracking step. The budget is charged one full poll interval
     * (10000 steps) at a time, so a run is interrupted only after at least
     * @p maxSteps steps have actually executed; use multiples of 10000 for
     * exact boundaries in tests. Two deterministic guarantees follow: a run
     * whose step total stays below the budget always completes, and a run
     * that would execute steps without bound (an infinite loop, a
     * catastrophic-backtracking regex) is interrupted after at most the
     * budget plus one poll interval - no timing involved.
     *
     * Like the execution timeout this is a per-run quota: every top-level
     * entry (evaluate(), QQuickJSValue::call(), signal callbacks, imports)
     * starts with a fresh quota, and the quota covers the script plus the
     * promise jobs drained at its end. Enabling the budget mid-run applies
     * to the current run from the next poll on (the steps executed before
     * the call were not counted and do not count against it). When
     * exceeded, the run aborts with the same uncatchable InternalError
     * "interrupted" as the execution timeout. Thread-safe (atomic state).
     *
     * @note The same poll model as setExecutionTimeoutMs() applies: bulk
     * C-level string/TypedArray operations, JSON.parse/stringify and GC
     * pauses consume no steps and cannot be interrupted by this budget;
     * C++ methods invoked from JS are only bounded by their own code.
     */
    void setStepBudget(quint64 maxSteps);

    /**
     * @warning These setters must be called from the engine's thread or
     * before any script execution to avoid data races inside QuickJS.
     */
    void setMemoryLimit(size_t limit);

    /**
     * @brief Sets the recursion budget of the JS stack guard.
     * @param size Budget in bytes; 0 disables the guard.
     *
     * The budget is measured downwards from the current stack position at
     * every top-level entry point (evaluate(), QQuickJSValue::call() and other
     * engine entries re-anchor the guard), so nested or unusually deep host
     * calls do not silently eat the budget. The effective guard budget is
     * this requested JS recursion share plus a calibrated machinery
     * allowance (the C entry chain down to the first JS frame), clamped to
     * the stack the engine's thread can actually spare: never below the
     * safety margin above the real stack bottom (an unreachable guard turns
     * runaway recursion into a hard process crash), and never above the
     * usable window below the entry. When the entry itself arrives deeper
     * than safety margin + 64 KiB above the bottom, a fixed emergency
     * budget keeps the engine-internal JS catchable and script entries are
     * refused with a catchable RangeError instead of entering JS at all.
     * Call from the engine's thread, like the other setters of this class.
     *
     * Limitation: the measured thread-stack floor is cached per thread and
     * is NOT invalidated when a host switches that thread onto another
     * stack (fibers, coroutines, ucontext swaps). A fiber stack located
     * above the previously measured floor sees an inflated usable window,
     * so the guard line may sit below the fiber stack's real bottom. Hosts
     * using cooperative stacks should create the engine while running on
     * the deepest stack the engine will ever execute on.
     */
    void setMaxStackSize(size_t size);
    void collectGarbage();
    void setAutoProcessPromises(bool enabled);

    static QString version();
    JSMemoryUsage memoryUsage() const;

    /**
     * @brief Sets the initial GC trigger threshold, in bytes. quickjs-ng
     * re-seeds the threshold after every automatically triggered collection
     * (to 1.5x the bytes allocated at that point), so this value governs
     * the first cycles rather than pinning a permanent limit;
     * collectGarbage() runs a full collection regardless of it.
     */
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

    /**
     * @brief Throws a run-time error (exception) with the given @p message.
     *
     * QJSEngine-compatible error delivery: when called from C++ code invoked
     * by a script (a slot or property accessor), the error is thrown into the
     * script where try/catch can handle it. When called outside any script
     * execution, the error stays pending: hasError() returns true, the next
     * evaluate()/call() returns it without executing, and catchError()
     * retrieves and clears it.
     */
    void throwError(const QString& message);
    /** @overload throwError(). Throws an error of the given @p errorType. */
    void throwError(QQuickJSValue::ErrorType errorType, const QString& message = QString());
    /** @overload throwError(). Throws the pre-constructed @p error value. */
    void throwError(const QQuickJSValue& error);

    /**
     * @brief Returns true if an exception is currently pending on the engine
     * (a throwError() call or a not-yet-caught script exception).
     *
     * evaluate() and QQuickJSValue::call() catch exceptions of the code they
     * run, so hasError() is false after such a call returned.
     */
    bool hasError() const;
    /**
     * @brief Catches the pending exception and returns it as a value, or an
     * undefined value when nothing is pending. After the call hasError()
     * returns false.
     */
    QQuickJSValue catchError();

private:
    // ---------------------------------------------------------------------
    // Internal machinery (raw JSValues, exception processing, guard/timer
    // resets). Private on purpose: the friend classes (QQuickJSValue,
    // QQuickJSValueIterator, the proxies, QuickJSConvert) are exactly the
    // in-library callers; a public resetExecutionState()/resetExecutionTimer()
    // would let a host slot invoked by a running script reset the execution
    // quota mid-script or re-anchor the guard from a deep frame - the states
    // these entry points deliberately own. The friend list is the internal
    // API surface; adding an in-library consumer means adding a friend, a
    // visible reviewable change.
    // ---------------------------------------------------------------------
    JSValue toJSValueImpl(const QVariant& var, int depth);
    QVariant toVariant(JSValueConst val);
    QVariant toVariant(JSValueConst val, QQuickJSValue::ObjectConversionBehavior behavior);
    JSValue wrapQObject(QObject* obj);
    // Engine-wide metaobject cache lookup/build for a QMetaObject; nullptr
    // means the build failed with a pending JS exception. Used by
    // wrapQObject() and the proxy's runtime metaobject-change refresh.
    JSMetaObjectCache* metaObjectCacheFor(const QMetaObject* meta);
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
    // starts with a fresh execution-timeout quota; nested calls never reset.
    // The interrupt flag itself is deliberately not cleared here: it is
    // sticky until setInterrupted(false), like QJSEngine.
    void resetExecutionState() {
        ensureEngineThread();
        resetExecutionTimer();
        // Fresh step-budget quota for the run starting here (same lifecycle
        // as the timeout deadline above).
        m_stepsUsed.store(0, std::memory_order_relaxed);
        reanchorStackGuard();
    }
    JSValue toJSValueFromVoid(void* data, int metaType);

    JSClassExoticMethods m_qobjectExotic{};
    // Deadline in ms since the steady_clock epoch; 0 = no deadline. Atomic so
    // that setExecutionTimeoutMs() from a watchdog thread is race-free.
    std::atomic<qint64> m_executionDeadlineMs{0};
    int m_evalDepth = 0;
    int m_evalCacheLimit = DefaultEvalCacheLimit;
    bool m_evalCacheEnabled = true;
    // Requested JS recursion budget (0 = guard disabled); the effective value
    // is re-clamped at every top-level entry, see reanchorStackGuard().
    size_t m_stackBudgetBytes = DefaultStackBudget;
    // Measured C machinery descent below a top-level entry (compile + eval +
    // first JS_CallInternal + C trampolines), in bytes, granted on top of the
    // requested budget at every reanchor; calibrated once by
    // calibrateMachineryAllowance() (m_allowanceCalibrated gates the probe).
    size_t m_machineryAllowance = 0;
    bool m_allowanceCalibrated = false;
    // Set by reanchorStackGuard() when the entry anchor sits deeper than
    // safety margin + 64 KiB above the real thread stack bottom: the stack
    // is too starved to enter JS safely (even the throw path may not fit),
    // so every top-level entry (evaluate(), executeJsCall(), signal
    // callbacks) refuses with a catchable RangeError before running any
    // script; engine-internal JS stays under the emergency guard budget.
    bool m_stackStarved = false;
    // Diagnostics (QJS_GUARD_TRACE=1): last anchor SP and effective stack
    // size, logged at the entry and at a caught stack overflow.
    uintptr_t m_guardAnchorSp = 0;
    size_t m_guardStackSize = 0;
    // SP of the depth-0 QVariant conversion entry (both directions): the
    // descent bound for the conversion floor checks when the real thread
    // floor is unknown (see kUnknownFloorAssumedUsable in QuickJSEngine_p.h). A
    // depth-0 entry resets it to its own frame; a strictly-nested chain
    // inherits the outer anchor, which only ever errs conservative.
    uintptr_t m_conversionAnchorSp = 0;

    JSRuntime* m_rt;
    JSContext* m_ctx;
    JSValue m_globalObj;

    JSClassID m_qobjectClassID;
    JSClassID m_methodClassID;
    JSClassID m_signalClassID;
    JSClassID m_objectClassID;

    QHash<JSAtom, QString> m_atomNameCache;
    QHash<QString, JSAtom> m_stringAtomCache;
    QHash<QObject*, JSValue> m_qobjectWrapperCache;
    QHash<const QMetaObject*, JSMetaObjectCache*> m_metaObjectCaches;

    // Console-extension state (console.time/timeEnd and console.count). The
    // counters are keyed by the printed label: quickjs-ng exposes no call-site
    // information to C callbacks, unlike V4 which keys by script and line.
    QHash<QString, QElapsedTimer> m_consoleTimers;
    QHash<QString, int> m_consoleCounters;

    // ES-module registry. File modules keyed by the canonical path: the
    // importModule() singleton cache, shared with the module loader callback
    // so that both entry points agree on one instance per file. Registered
    // modules map the public name to the unique internal module name created
    // by registerModule(); re-registration replaces the mapping, matching the
    // live QJSEngine behavior.
    QHash<QString, FileModuleEntry> m_fileModules;
    QHash<QString, QByteArray> m_registeredModules;
    int m_moduleSerial = 0;

    // Keyed by sender: connect/disconnect and the destroyed-cleanup walk the
    // per-sender bucket instead of scanning the whole list (O(N) total
    // receivers with tens of thousands of connections made disconnect and
    // sender destruction quadratic).
    QMultiHash<QObject*, JSGenericReceiver*> m_signalReceivers;
    QList<JSGenericReceiver*> m_garbageReceivers;

    QList<PendingRejection> m_pendingRejections;
    // One-shot latch for the rejection-buffer ceiling warning, PER ENGINE
    // (a process-global latch would swallow the warning for every engine
    // after the first one).
    std::atomic<bool> m_rejectionCapWarned{false};

    QList<EvalCacheEntry> m_evalCache;

    std::atomic<quint32> m_executionTimeoutMs;
    std::atomic<bool> m_interrupted;
    // Step budget (setStepBudget): 0 = off (default). m_stepsUsed is charged
    // kStepPollInterval steps per interrupt-handler invocation while a
    // budget is set and re-armed at every top-level entry
    // (resetExecutionState()). Atomics for the same reason as the deadline:
    // the setter may run on a watchdog thread.
    std::atomic<quint64> m_stepBudget;
    std::atomic<quint64> m_stepsUsed;
    bool m_autoProcessPromises;
    Extensions m_installedExtensions = Extensions(0);

    JSCommonAtoms m_atoms;
    JSIntrinsics m_intrinsics;

    static int interruptHandler(JSContext * ctx, void* opaque);

    // Re-measures the JS stack guard from the current stack position and
    // re-clamps it against the real bounds of the engine's thread. Called at
    // every top-level engine entry (through resetExecutionState()) and from
    // setMaxStackSize(): a guard anchored only at engine creation misses the
    // C++ machinery below deeper entry points. The effective budget always
    // includes the calibrated machinery allowance (see
    // calibrateMachineryAllowance()): on fat Debug/ASan builds a single
    // JS_CallInternal frame can take tens of KiB, so a bare recursion share
    // would make the guard fire inside the legitimate entry chain and every
    // check-hitting operation (promise jobs, typed array construction,
    // apply(), async resume, module linking) would throw a false
    // "Maximum call stack size exceeded" while plain scripts without stack
    // checks keep running. Refuses to re-anchor from inside running JS
    // (m_evalDepth != 0): the next top-level entry re-measures.
    void reanchorStackGuard();
    // One-shot (per engine) measurement of the real C machinery descent
    // below a top-level entry: evaluates a single-JS-frame probe script
    // through a hidden C function that records its own stack pointer, then
    // derives the machinery allowance (x1.5 headroom for deeper entry paths
    // such as module linking and promise jobs) used by reanchorStackGuard().
    // Runs at the end of the constructor, on the fully initialized engine,
    // under a provisional margin-only guard.
    void calibrateMachineryAllowance();

    // ES-module callbacks installed on the runtime; see the definitions in
    // the .cpp (name resolution, file loading and registered-module init).
    static char* moduleNormalize(JSContext* ctx, const char* base_name,
                                 const char* module_name, void* opaque);
    static JSModuleDef* moduleLoader(JSContext* ctx, const char* module_name, void* opaque);
    static int registeredModuleInit(JSContext* ctx, JSModuleDef* m);

    // Translates the "stack" property of a caught exception into QJSEngine's
    // exceptionStackTrace frame format; see the definition in the .cpp.
    static void collectExceptionStackTrace(QQuickJSEngine* engine, JSValueConst errVal, QStringList* out);

    // Shared post-processing of a JS_EvalFunction result in evaluate(),
    // common to the eval-cache hit and the freshly compiled path: the
    // exception-safe wrap, the run-to-completion microtask drain, the
    // uncaught-exception report and the sticky-interrupt check.
    QQuickJSValue finishEvaluationRun(JSValue ret, bool topLevel, QStringList* exceptionStackTrace);

    static qint64 steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static qint64 deadlineFromTimeout(quint32 timeoutMs) {
        return timeoutMs > 0 ? steadyNowMs() + static_cast<qint64>(timeoutMs) : 0;
    }

    void queueUnhandledRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason);
    void retractHandledRejection(JSContext* ctx, JSValueConst promise);

    QVariant toVariantImpl(JSValueConst val, JSVisitedStack& visited, int depth,
                           QQuickJSValue::ObjectConversionBehavior behavior);

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
        // Null test through a real pointer: `value` is a reference, and for
        // an array T (a string literal) GCC's -Wnonnull-compare flags a
        // direct test even though the decayed pointer is never null.
        const char* const s = value;
        if (s) {
            setProperty(name, QVariant::fromValue(QString::fromUtf8(s)));
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
        // See the QString overload for why the null test goes through a
        // real pointer (-Wnonnull-compare on array T instantiations).
        const char* const s = value;
        if (s) {
            setProperty(name, QVariant::fromValue(QString::fromUtf8(s)));
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
        // See the QString overload for why the null test goes through a
        // real pointer (-Wnonnull-compare on array T instantiations).
        const char* const s = value;
        if (s) {
            setProperty(arrayIndex, QVariant::fromValue(QString::fromUtf8(s)));
        } else {
            setProperty(arrayIndex, QQuickJSValue(jsEngine, JS_NULL, true));
        }
    } else {
        setProperty(arrayIndex, QVariant::fromValue(value));
    }
}

#endif // QUICKJSENGINE_H
