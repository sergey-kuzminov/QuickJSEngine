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

#include <concepts>
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

template <NonBoolInvocable F>
static auto invokeSafe(JSContext* ctx, const char* contextName, F&& func) {
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

[[nodiscard]] static inline QVariant makeDefaultVariant(int metaType) {
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

[[nodiscard]] static inline JSValue qStringToJS(JSContext* ctx, QStringView str) {
    if (str.isEmpty()) return JS_NewStringLen(ctx, "", 0);
    return JS_NewStringUTF16(ctx, reinterpret_cast<const uint16_t*>(str.utf16()),
                             static_cast<size_t>(str.size()));
}

[[nodiscard]] static inline JSAtom atomFromUtf8(JSContext* ctx, QByteArrayView bytes) {
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

static inline void clearPendingException(JSContext* ctx) {
    if (!ctx) return;
    if (JS_HasException(ctx)) {
        ScopedJSValue exc(ctx, JS_GetException(ctx));
    }
}

[[nodiscard]] static inline bool consumeException(JSContext* ctx, JSValueConst val) {
    if (JS_IsException(val)) {
        clearPendingException(ctx);
        return true;
    }
    return false;
}

static inline bool safeMetacall(JSContext* ctx, QObject* obj, QMetaObject::Call call, int idx, void** args) {
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
    uint8_t isResettable;
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

struct JSMethodProxyData {
    QPointer<QObject> obj;
    QByteArray methodName;
    QVarLengthArray<int, 10> paramTypes;
    int paramCount = 0;
    int methodIdx = -1;
    int retType = QMetaType::UnknownType;
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
    bool m_isExecuting = false;

    JSGenericReceiver() {
        m_deleteLaterIdx = metaObject()->indexOfSlot("deleteLater()");
    }
    ~JSGenericReceiver() { if (m_ctx) JS_FreeValue(m_ctx, m_jsCallback); }
    int qt_metacall(QMetaObject::Call c, int id, void **a) override;
};

#endif // QUICKJSENGINE_P_H
