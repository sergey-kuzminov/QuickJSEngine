//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef QUICKJSENGINE_P_H
#define QUICKJSENGINE_P_H

#include "QuickJSEngine.h"

#include <QByteArrayView>
#include <QHash>
#include <QMetaMethod>
#include <QMetaType>
#include <QPointer>
#include <QStringView>
#include <QVarLengthArray>

#include <atomic>
#include <concepts>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>   // _AddressOfReturnAddress() in currentStackPointer()
#endif
#include <functional>
#include <span>
#include <utility>

template <typename F>
concept NonBoolInvocable = std::invocable<F&> && !std::same_as<std::invoke_result_t<F&>, bool>;

// JS_GetOpaque vs JS_GetOpaque2 (vendored quickjs.c:12140-12147): the "2"
// variant throws a generic TypeError ("<ClassName> object expected") and
// returns NULL on a class mismatch. Every opaque-pointer site in this
// codebase deliberately uses plain JS_GetOpaque with an explicit null check
// instead, because:
//  - several dispatches probe TWO classes on the same value (a signal proxy
//    or a method proxy: js_signal_connect / js_signal_disconnect) and the
//    first probe must not leave a pending exception before the second gets
//    its chance;
//  - the error text is part of the wrapper's contract ("QObject method is
//    not a signal", "C++ object has been deleted" for a wrapper whose
//    QPointer died - note the opaque is still present there, only the C++
//    side is gone), not the vendor's class-name message;
//  - the remaining sites are non-throwing predicates and walkers
//    (isQObject, gc_mark, the conversion ladders) where a pending exception
//    left behind by a lookup would corrupt the caller's control flow.

// ---------------------------------------------------------------------------
// Stack-guard machinery shared by every engine TU (the guard itself lives in
// QuickJSStackGuard.cpp). warnOnce and currentStackPointer are inline: they
// sit on the hot path of every conversion node, and a cross-TU call for the
// SP read measurably slowed container conversions. threadStackFloor and
// guardTraceEnabled are declared here and defined in QuickJSStackGuard.cpp;
// the core (exception reporting) and conversion (recursion guards) TUs call
// them as well.
inline bool warnOnce(std::atomic<bool>& latch)
{
    return latch.exchange(true);
}

// Usable room assumed below the anchor when the thread floor is unknown
// (platform without a measurement API, or a garbage reading with no
// last-known-good fallback): clampedStackBudget() clamps the wanted budget
// to kUnknownFloorAssumedUsable - kSafetyMargin instead of releasing it
// unclamped. 256 KiB keeps the effective budget (128 KiB) above the
// emergency threshold - entries run, tightly clamped - while never handing
// out the full 512 KiB + allowance line that small thread stacks (1 MiB
// Windows workers, embedded targets) cannot honor.
constexpr size_t kUnknownFloorAssumedUsable = 256 * 1024;

// Raw machine stack pointer of the calling frame. Unlike
// __builtin_frame_address(0), which sanitizers may rebase into an ASan
// fake-stack frame (detect_stack_use_after_return), a direct register read
// always stays in the real stack address space.
//
// Architecture dispatch uses compiler macros (__x86_64__/__aarch64__), not
// Qt's Q_PROCESSOR_*: the ARM64 spelling differs between toolchains
// (Q_PROCESSOR_ARM_64 in Qt), and a silently dead branch here would fall the
// build back onto the frame-address path - exactly the rebasing-prone source
// this function exists to avoid on the guard's critical path.
inline uintptr_t currentStackPointer()
{
#if defined(_MSC_VER)
    // Points at the caller's return-address slot, i.e. the TOP of this
    // frame: an overestimate of SP by the current frame's size (hundreds
    // of bytes, a few KiB worst-case in Debug). clampedStackBudget absorbs
    // that inside kSafetyMargin; MSVC has no inline-asm register read that
    // survives optimization this cheaply. clang-cl also lands here (_MSC_VER
    // first), keeping GNU asm out of MSVC-compatible builds.
    return reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
#elif defined(__x86_64__)
    uintptr_t sp;
    asm volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
#elif defined(__aarch64__)
    uintptr_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
#elif defined(__GNUC__) || defined(__clang__)
    // Exotic architecture without an asm path: the frame address is the
    // best available approximation of SP. Under ASan
    // detect_stack_use_after_return clang may rebase it into a fake stack
    // (detected by warnIfFakeStackActive); gcc keeps the real address.
    return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
#else
#error "QQuickJSEngine: unsupported toolchain for the stack guard"
#endif
}

// Cached per-thread stack floor, 0 when unknown (QuickJSStackGuard.cpp).
uintptr_t threadStackFloor(uintptr_t sp);

// QJS_GUARD_TRACE=1 diagnostics switch (QuickJSStackGuard.cpp).
bool guardTraceEnabled();

template <NonBoolInvocable F>
auto invokeSafe(JSContext* ctx, const char* contextName, F&& func) {
    // Exception policy, uniform with safeMetacall: a known std::exception becomes
    // a catchable plain Error (deterministic state, the script can defend itself);
    // an unknown catch(...) becomes InternalError, which quickjs-ng marks
    // uncatchable - it is guaranteed to reach the C++ boundary.
    try {
        return func();
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "C++ exception in %s: %s", contextName, e.what()); } catch (...) {}
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in %s", contextName); } catch (...) {}
    }
    if constexpr (std::same_as<std::invoke_result_t<F&>, JSValue>) {
        return JS_EXCEPTION;
    } else {
        return -1;
    }
}

[[nodiscard]] inline QVariant makeDefaultVariant(int metaType) {
    return QVariant(QMetaType(metaType));
}

class EvalDepthGuard {
public:
    explicit EvalDepthGuard(int& depth) : m_depth(depth) { ++m_depth; }
    ~EvalDepthGuard() { --m_depth; }

    Q_DISABLE_COPY(EvalDepthGuard)

private:
    int& m_depth;
};

// RAII bookkeeping shared by EVERY path that can run user JavaScript:
// the engine entries (evaluate, importModule, registerModule, value calls
// and constructions, signal callbacks) and the QQuickJSValue accessors
// whose C API can invoke user code (getters and setters, valueOf/toString
// coercion, Proxy traps). At depth 0 it starts a fresh execution quota
// (timeout deadline, step budget) and re-anchors the stack guard at the
// actual entry frame, so JS entered from a deep host frame never runs
// against a deadline or a guard line measured at some earlier entry.
// Nested scopes only track depth.
//
// Ordering contract: the destructor flushes unhandled rejections BEFORE
// releasing the depth, so the end-of-tick report (which may run user JS
// through reason toString) still runs under the depth guard, exactly like
// the former per-entry EvalDepthGuard + rejectionFlush pairs.
//
// Starved-stack policy is two-tier. Bulky entries (evaluate, module
// import/registration, JS calls and constructions, signal callbacks)
// refuse up front through starved(): their compile/eval/trampoline
// machinery does not fit a starved window. The light accessors (property
// reads/writes, coercions, Proxy-trap queries) keep the scope for the
// fresh quota and the re-anchored guard but do NOT refuse: the re-anchor
// installs the emergency budget, under which the shallow C chain of a
// builtin coercion completes and a getter or trap too deep to fit throws
// its normal catchable RangeError. Error messages must stay readable on
// a starved stack - the starved refusal itself is diagnosed through
// QQuickJSValue::toString() by the very tests that pin it.
class JsEntryScope {
public:
    explicit JsEntryScope(QQuickJSEngine* engine)
        : m_engine(engine), m_topLevel(engine->m_evalDepth == 0)
    {
        if (m_topLevel) engine->resetExecutionState();
        ++engine->m_evalDepth;
    }

    ~JsEntryScope() {
        if (m_topLevel) m_engine->flushUnhandledRejections();
        --m_engine->m_evalDepth;
    }

    bool isTopLevel() const noexcept { return m_topLevel; }
    // A starved top-level entry must refuse before running any JS; the
    // caller decides the refusal shape (returned error object, pending
    // JS_ThrowRangeError, or a default value).
    bool starved() const noexcept { return m_topLevel && m_engine->m_stackStarved; }

    JsEntryScope(const JsEntryScope&) = delete;
    JsEntryScope& operator=(const JsEntryScope&) = delete;

private:
    QQuickJSEngine* m_engine;
    bool m_topLevel;
};

class ScopedJSValue {
public:
    explicit ScopedJSValue(JSContext* ctx = nullptr, JSValue val = JS_UNDEFINED) noexcept
        : m_ctx(ctx), m_val(val) {}
    ScopedJSValue(JSContext* ctx, JSValue val, bool dup) : m_ctx(ctx), m_val(val) {
        if (dup) JS_DupValue(m_ctx, m_val);
    }
    ~ScopedJSValue() { if (m_ctx) JS_FreeValue(m_ctx, m_val); }

    ScopedJSValue(const ScopedJSValue&) = delete;
    ScopedJSValue& operator=(const ScopedJSValue&) = delete;

    ScopedJSValue(ScopedJSValue&& o) noexcept
        : m_ctx(std::exchange(o.m_ctx, nullptr)), m_val(std::exchange(o.m_val, JS_UNDEFINED)) {}
    ScopedJSValue& operator=(ScopedJSValue&& o) noexcept {
        if (this != &o) {
            if (m_ctx) JS_FreeValue(m_ctx, m_val);
            m_ctx = std::exchange(o.m_ctx, nullptr);
            m_val = std::exchange(o.m_val, JS_UNDEFINED);
        }
        return *this;
    }

    [[nodiscard]] JSValue value() const noexcept { return m_val; }
    explicit operator JSValue() const noexcept { return m_val; }
    bool isValid() const noexcept { return m_ctx != nullptr; }
    bool isUndefined() const noexcept { return JS_IsUndefined(m_val); }

    [[nodiscard]] JSValue release() noexcept {
        JSValue v = m_val;
        m_ctx = nullptr;
        m_val = JS_UNDEFINED;
        return v;
    }

private:
    JSContext* m_ctx;
    JSValue m_val;
};

// Frees every JSValue in the viewed array on destruction.
class ScopedJSValueArray {
public:
    explicit ScopedJSValueArray(JSContext* ctx, std::span<const JSValue> values)
        : m_ctx(ctx), m_values(values) {}

    ~ScopedJSValueArray() {
        for (JSValue v : m_values) JS_FreeValue(m_ctx, v);
    }

    Q_DISABLE_COPY(ScopedJSValueArray)

private:
    JSContext* m_ctx;
    std::span<const JSValue> m_values;
};

class ScopedCString {
public:
    ScopedCString(JSContext* ctx, const char* str) : m_ctx(ctx), m_str(str) {}
    ~ScopedCString() { if (m_ctx && m_str) JS_FreeCString(m_ctx, m_str); }

    ScopedCString(const ScopedCString&) = delete;
    ScopedCString& operator=(const ScopedCString&) = delete;

    [[nodiscard]] const char* value() const { return m_str; }
    operator const char*() const { return m_str; }
    bool isValid() const { return m_str != nullptr; }

private:
    JSContext* m_ctx;
    const char* m_str;
};

[[nodiscard]] inline JSValue qStringToJS(JSContext* ctx, QStringView str) {
    if (str.isEmpty()) return JS_NewStringLen(ctx, "", 0);
    return JS_NewStringUTF16(ctx, reinterpret_cast<const uint16_t*>(str.utf16()),
                             static_cast<size_t>(str.size()));
}

[[nodiscard]] inline JSAtom atomFromUtf8(JSContext* ctx, QByteArrayView bytes) {
    return JS_NewAtomLen(ctx, bytes.constData(), static_cast<size_t>(bytes.size()));
}

// Owns a JSAtom for the scope lifetime. The char* constructor is zero-copy.
class ScopedJSAtom {
public:
    ScopedJSAtom(JSContext* ctx, const char* str)
        : m_ctx(ctx), m_atom((ctx && str) ? JS_NewAtom(ctx, str) : JS_ATOM_NULL) {}

    ~ScopedJSAtom() {
        if (m_ctx && m_atom != JS_ATOM_NULL) JS_FreeAtom(m_ctx, m_atom);
    }

    ScopedJSAtom(const ScopedJSAtom&) = delete;
    ScopedJSAtom& operator=(const ScopedJSAtom&) = delete;

    [[nodiscard]] JSAtom value() const noexcept { return m_atom; }
    bool isValid() const noexcept { return m_atom != JS_ATOM_NULL; }
    operator JSAtom() const noexcept { return m_atom; }

private:
    JSContext* m_ctx;
    JSAtom m_atom;
};

inline void clearPendingException(JSContext* ctx) {
    if (!ctx) return;
    if (JS_HasException(ctx)) {
        ScopedJSValue exc(ctx, JS_GetException(ctx));
    }
}

[[nodiscard]] inline bool consumeException(JSContext* ctx, JSValueConst val) {
    if (JS_IsException(val)) {
        clearPendingException(ctx);
        return true;
    }
    return false;
}

inline bool safeMetacall(JSContext* ctx, QObject* obj, QMetaObject::Call call, int idx, void** args) {
    try {
        QMetaObject::metacall(obj, call, idx, args);
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "C++ exception in meta-call: %s", e.what()); } catch (...) {}
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in meta-call"); } catch (...) {}
    }
    // The called C++ code may have thrown a JS exception via
    // QQuickJSEngine::throwError(). Propagate it: callers translate false
    // into JS_EXCEPTION, which keeps the pending exception intact for the
    // interpreter instead of returning a value that would silently swallow
    // it (a stale pending exception corrupts hasError()/catchError()).
    return !JS_HasException(ctx);
}

enum class PropertyCacheState { Invalid = -1, Dynamic = -2 };

struct MetaCacheEntry {
    enum class Kind : uint8_t { Method, Property };
    Kind kind;
    uint8_t methodType;
    uint8_t isWritable;
    uint8_t isReadable;
    uint8_t paramCount;
    int index;
    int type;
    QVarLengthArray<int, 10> paramTypes;
};

struct JSMetaObjectCache {
    JSContext* ctx;
    // Guards the cache key against QMetaObject address reuse (dynamic
    // metaobjects can be freed while the cache entry survives).
    QByteArray className;
    QHash<JSAtom, MetaCacheEntry> entries;
    QList<JSAtom> propertyAtoms;
    QList<JSAtom> methodAtoms;

    explicit JSMetaObjectCache(JSContext* c) : ctx(c) {}
    ~JSMetaObjectCache() {
        if (!ctx) return;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            JS_FreeAtom(ctx, it.key());
        }
    }

    [[nodiscard]] const MetaCacheEntry* find(JSAtom atom) const noexcept {
        auto it = entries.find(atom);
        return it != entries.end() ? &it.value() : nullptr;
    }
    bool contains(JSAtom atom) const noexcept { return find(atom) != nullptr; }
    void insert(JSAtom atom, const MetaCacheEntry& meta) {
        entries.insert(atom, meta);
    }
};

struct MethodProxyEntry { JSAtom atom; JSValue value; };
struct DynamicPropEntry {
    JSAtom atom;
    PropertyCacheState state;
    QByteArray name;
};

struct JSQObjectData {
    QPointer<QObject> obj;
    const QMetaObject* metaObj = nullptr;
    JSMetaObjectCache* metaCache = nullptr;
    QMetaObject::Connection destroyedConn;

    QVarLengthArray<MethodProxyEntry, 16> methodProxyCache;
    QVarLengthArray<DynamicPropEntry, 16> dynamicPropertyCache;

    void clearCaches(JSRuntime* rt) {
        for (auto& e : methodProxyCache) {
            JS_FreeAtomRT(rt, e.atom);
            JS_FreeValueRT(rt, e.value);
        }
        methodProxyCache.clear();

        for (auto& e : dynamicPropertyCache) {
            JS_FreeAtomRT(rt, e.atom);
        }
        dynamicPropertyCache.clear();

        if (destroyedConn) {
            QObject::disconnect(destroyedConn);
            destroyedConn = {};
        }
    }
};

// One same-name overload candidate of a method or signal. V4 selects
// among ALL overloads by argument count and conversion cost, not "first
// declared wins"; moc lists the full signature before the default-argument
// truncated synthetic entries it generates, so the declaration order in the
// candidate list matches the metaobject.
struct JSMethodCandidate {
    QVarLengthArray<int, 10> paramTypes;
    int paramCount = 0;
    int methodIdx = -1;
    int retType = QMetaType::UnknownType;
};

struct JSMethodProxyData {
    QPointer<QObject> obj;
    QByteArray methodName;
    QVarLengthArray<JSMethodCandidate, 2> candidates;

    // The first declared overload: the signal-connect wiring and the error
    // messages use it (connect keeps targeting the first signal, like the
    // pre-overload-selection behavior).
    const JSMethodCandidate& first() const { return candidates.first(); }
};

class JSGenericReceiver : public QObject {
public:
    using Callback = std::function<void(void**)>;
    Callback callback;
    JSContext* m_ctx = nullptr;
    JSValue m_jsCallback = JS_UNDEFINED;
    QPointer<QObject> m_sender;
    QMetaObject::Connection m_destroyedConn;
    QVarLengthArray<int, 10> paramTypes;
    int paramCount = 0;
    int m_methodIdx = -1;
    int m_deleteLaterIdx = 0;
    // Re-entrancy depth: a callback that re-emits the same signal (directly
    // or through a property change) must not let the nested exit clear the
    // "executing" state while the outer frame is still on the stack - a
    // disconnect inside the outer frame would then delete the receiver
    // under the live ExecGuard.
    int m_execDepth = 0;

    JSGenericReceiver() {
        m_deleteLaterIdx = metaObject()->indexOfSlot("deleteLater()");
    }
    ~JSGenericReceiver() { if (m_ctx) JS_FreeValue(m_ctx, m_jsCallback); }
    int qt_metacall(QMetaObject::Call c, int id, void **a) override;
};

#endif // QUICKJSENGINE_P_H
