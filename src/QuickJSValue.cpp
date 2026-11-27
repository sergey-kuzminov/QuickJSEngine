//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSEngine_p.h"
#include "QuickJSConvert.h"

#include <QScopeGuard>
#include <QThread>

#include <algorithm>
#include <limits>
#include <string_view>

inline JSContext* QQuickJSValue::ctx() const {
    return m_engine ? m_engine->m_ctx : nullptr;
}

void QQuickJSValue::ensureEngineThread() const {
    Q_ASSERT_X(!m_engine || m_engine->thread() == QThread::currentThread(),
               "QQuickJSValue", "QQuickJSValue accessed in a different thread");
}

QQuickJSValue QQuickJSValue::executeJsCall(QQuickJSEngine* engine, JSContext* c, JSValueConst funcVal,
                                           JSValueConst* jsArgs, int argc,
                                           const QQuickJSValue& thisObj, bool isConstructor) {
    // QJSEngine parity: a pending exception (e.g. throwError() called from
    // C++ between executions) preempts the call: the function is not invoked
    // and the pending error is returned. Like QJSValue::call(), the error is
    // only returned, never reported through uncaughtException.
    if (JS_HasException(c)) {
        return QQuickJSValue::fromException(engine);
    }

    // Entry bookkeeping shared by every JS-running path: at depth 0 a fresh
    // quota (timeout deadline, step budget), a stack-guard re-anchor at the
    // actual entry frame, and the end-of-tick rejection flush on the way out.
    JsEntryScope entryScope(engine);
    // Starved entry (see QQuickJSEngine::evaluate()): the window below this
    // anchor is inside the guard's safety margin; refuse with the same
    // catchable RangeError before entering JS_Call.
    if (Q_UNLIKELY(entryScope.starved())) {
        return engine->newErrorObject(QQuickJSValue::RangeError,
                                       QStringLiteral("Maximum call stack size exceeded"));
    }

    // QJSEngine parity: an omitted thisObject binds the engine global object,
    // even for strict callees (which would otherwise see `this === undefined`).
    JSValueConst thisVal = thisObj.m_engine ? thisObj.jsVal() : engine->m_globalObj;

    JSValue ret = isConstructor
                      ? JS_CallConstructor(c, funcVal, argc, jsArgs)
                      : JS_Call(c, funcVal, thisVal, argc, jsArgs);

    ScopedJSValue retScoped(c, ret);

    const bool hadException = JS_IsException(ret);
    QQuickJSValue errVal;
    if (hadException) {
        errVal = QQuickJSValue::fromException(engine);
    }

    // Run-to-completion: only a TOP-LEVEL call drains the microtask queue;
    // a call nested inside a running script (a host slot calling back into
    // JS) leaves the drain to the outer entry, like V4.
    if (entryScope.isTopLevel() && engine->m_autoProcessPromises && JS_IsJobPending(engine->m_rt)) {
        engine->processPendingJobs();
    }

    // QJSEngine parity (probed against live Qt 6.12): a sticky interrupt
    // flag yields an "Interrupted" error object for calls and constructions,
    // even when the function body itself completed without hitting an
    // interrupt check - and it WINS over a real exception the call produced:
    // V4 also replaces a pending real error with the plain "Interrupted"
    // error in this combination (live probe: a getter that sets the
    // interrupt flag and then throws reports only "Interrupted").
    if (engine->isInterrupted()) {
        return engine->newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
    }

    if (hadException) {
        return errVal;
    }

    return QQuickJSValue(engine, retScoped.release(), true);
}

QQuickJSValue::QQuickJSValue() : m_engine(nullptr), m_val(JS_UNDEFINED), m_owned(false), m_isError(false) {}

QQuickJSValue::QQuickJSValue(QQuickJSEngine* engine, JSValue val, bool owned)
    : m_engine(engine), m_val(val), m_owned(owned), m_isError(false) {}

QQuickJSValue::~QQuickJSValue() {
    JSContext* c = ctx();
    if (m_owned && c) {
        ensureEngineThread();
        JS_FreeValue(c, m_val);
    }
}

QQuickJSValue::QQuickJSValue(const QQuickJSValue& o)
    : m_engine(o.m_engine), m_val(o.m_val), m_owned(o.m_owned), m_isError(o.m_isError)
{
    JSContext* c = ctx();
    if (m_owned && c) {
        ensureEngineThread();
        JS_DupValue(c, m_val);
    }
}

QQuickJSValue& QQuickJSValue::operator=(const QQuickJSValue& o) {
    if (this != &o) {
        JSContext* c = ctx();
        if (m_owned && c) {
            ensureEngineThread();
            JS_FreeValue(c, m_val);
        }
        m_engine = o.m_engine;
        m_val = o.m_val;
        m_owned = o.m_owned;
        m_isError = o.m_isError;
        c = ctx();
        if (m_owned && c) {
            ensureEngineThread();
            JS_DupValue(c, m_val);
        }
    }
    return *this;
}

QQuickJSValue::QQuickJSValue(QQuickJSValue&& o) noexcept
    : m_engine(std::exchange(o.m_engine, nullptr)),
      m_val(std::exchange(o.m_val, JS_UNDEFINED)),
      m_owned(std::exchange(o.m_owned, false)),
      m_isError(std::exchange(o.m_isError, false))
{}

QQuickJSValue& QQuickJSValue::operator=(QQuickJSValue&& o) noexcept {
    if (this != &o) {
        JSContext* c = ctx();
        if (m_owned && c) {
            ensureEngineThread();
            JS_FreeValue(c, m_val);
        }

        m_engine = std::exchange(o.m_engine, nullptr);
        m_val = std::exchange(o.m_val, JS_UNDEFINED);
        m_owned = std::exchange(o.m_owned, false);
        m_isError = std::exchange(o.m_isError, false);
    }
    return *this;
}

JSValue QQuickJSValue::jsVal() const { return m_val; }

QQuickJSEngine* QQuickJSValue::engine() const {
    return m_engine.data();
}

bool QQuickJSValue::isValid() const {
    return ctx() != nullptr && !JS_IsException(m_val);
}

bool QQuickJSValue::isError() const {
    if (m_isError) return true;
    if (!ctx()) return false;
    return JS_IsError(m_val);
}

bool QQuickJSValue::isCallable() const {
    JSContext* c = ctx();
    if (!c) return false;
    return JS_IsFunction(c, m_val);
}

bool QQuickJSValue::isObject() const {
    if (!ctx()) return false;
    return JS_IsObject(m_val);
}

bool QQuickJSValue::isArray() const {
    if (!ctx()) return false;
    return JS_IsArray(m_val);
}

bool QQuickJSValue::isString() const {
    if (!ctx()) return false;
    return JS_IsString(m_val);
}

bool QQuickJSValue::isNumber() const {
    if (!ctx()) return false;
    return JS_IsNumber(m_val);
}

bool QQuickJSValue::isBool() const {
    if (!ctx()) return false;
    return JS_IsBool(m_val);
}

bool QQuickJSValue::isNull() const {
    return JS_IsNull(m_val);
}

bool QQuickJSValue::isUndefined() const {
    return JS_IsUndefined(m_val);
}

bool QQuickJSValue::isRegExp() const {
    if (!ctx()) return false;
    return JS_IsRegExp(m_val);
}

bool QQuickJSValue::isDate() const {
    if (!ctx()) return false;
    return JS_IsDate(m_val);
}

bool QQuickJSValue::isQObject() const {
    JSContext* c = ctx();
    if (!c || !JS_IsObject(m_val)) return false;
    return JS_GetOpaque(m_val, m_engine->m_qobjectClassID) != nullptr;
}

bool QQuickJSValue::isSymbol() const {
    return JS_IsSymbol(m_val);
}

bool QQuickJSValue::isPromise() const {
    if (!ctx()) return false;
    return JS_IsPromise(m_val);
}

QQuickJSValue::ErrorType QQuickJSValue::errorType() const {
    ensureEngineThread();

    // QJSEngine parity: only Error objects carry a class. A thrown
    // non-Error value ("throw 42") classifies as NoError even though
    // isError() is true for it - a deliberate wrapper divergence documented
    // in the README. JS_IsError() inspects the value itself instead of the
    // any-exception flag.
    JSContext* c = ctx();
    if (!c) return NoError;
    if (!JS_IsError(m_val)) return NoError;

    // Classification by prototype identity, NOT by the 'name' property:
    // QJSEngine branches on the internal error class, while reading 'name'
    // would both run script (a getter/toString) and misclassify trivial
    // tampering (e.name = "RangeError" on a plain Error). The engine
    // captured the intrinsic error prototypes at construction, before any
    // script ran; walking this value's prototype chain against them is
    // immune to 'name' mutation and to scripts replacing the global error
    // constructors afterwards. Chain order is most-derived first, so a
    // subclass of RangeError classifies as RangeError, like V4's
    // construction-time class. Divergence: V4 keeps that class even after
    // Object.setPrototypeOf() re-parents the instance; the chain walk then
    // reports the new ancestry's class, or GenericError if none matches.
    static const QQuickJSValue::ErrorType kClasses[7] = {
        GenericError, EvalError, RangeError, ReferenceError,
        SyntaxError, TypeError, URIError,
    };
    ScopedJSValue proto(c, JS_GetPrototype(c, m_val));
    if (JS_IsException(proto.value())) {
        // The FIRST prototype fetch can itself throw (a Proxy
        // getPrototypeOf trap): clear the pending exception and classify
        // conservatively, exactly like a throwing trap deeper in the chain -
        // leaving it pending would poison the next entry (invariant 11).
        clearPendingException(c);
        return GenericError;
    }
    for (int hop = 0; hop < 16 && JS_IsObject(proto.value()); ++hop) {
        const void* protoPtr = JS_VALUE_GET_PTR(proto.value());
        const JSValue* const table = m_engine.data()->m_intrinsics.errorProtos;
        for (int i = 0; i < 7; ++i) {
            if (JS_IsObject(table[i]) && JS_VALUE_GET_PTR(table[i]) == protoPtr)
                return kClasses[i];
        }
        JSValue next = JS_GetPrototype(c, proto.value());
        if (JS_IsException(next)) {
            // A Proxy getPrototypeOf trap in the chain threw: clear the
            // pending exception and classify conservatively instead of
            // leaking engine state into the next entry.
            clearPendingException(c);
            return GenericError;
        }
        proto = ScopedJSValue(c, next);
    }
    return GenericError;
}

QQuickJSValue QQuickJSValue::fromException(QQuickJSEngine* engine) {
    if (!engine) return QQuickJSValue();
    JSContext* c = engine->m_ctx;

    // quickjs-ng stores JS_UNINITIALIZED in current_exception by default;
    // JS_GetException() returns it even when there is no actual exception,
    // so the JS_HasException flag must be checked before extracting.
    if (!JS_HasException(c)) {
        return QQuickJSValue();
    }
    JSValue excVal = JS_GetException(c);
    QQuickJSValue val(engine, excVal, true);
    val.m_isError = true;
    return val;
}

bool QQuickJSValue::isSameContext(const QQuickJSValue& other) const {
    if (other.m_engine == m_engine) return true;
    return !other.m_engine && JS_IsUndefined(other.m_val);
}

bool QQuickJSValue::areArgsValid(const QList<QQuickJSValue>& args) const {
    return std::ranges::all_of(args, [this](const QQuickJSValue& arg) { return isSameContext(arg); });
}

bool QQuickJSValue::tryExtractErrorLineNumber(const char* name, QQuickJSValue& outResult) const {
    JSContext* c = ctx();
    if (!c) return false;
    // Public API: a null name must not reach std::string_view(nullptr).
    if (!name) return false;

    QQuickJSEngine* jsEngine = m_engine.data();

    const JSAtom atom = (std::string_view(name) == "lineNumber")
                            ? jsEngine->m_atoms.lineNumber
                            : jsEngine->m_atoms.line;

    ScopedJSValue lineVal(c, JS_GetProperty(c, m_val, atom));

    if (JS_IsException(lineVal.value())) {
        outResult = fromException(m_engine.data());
        return true;
    }

    if (JS_IsNumber(lineVal.value())) {
        outResult = QQuickJSValue(m_engine.data(), JS_DupValue(c, lineVal.value()), true);
        return true;
    }

    ScopedJSValue stackVal(c, JS_GetProperty(c, m_val, jsEngine->m_atoms.stack));
    if (JS_IsException(stackVal.value())) {
        outResult = fromException(m_engine.data());
        return true;
    }

    if (JS_IsString(stackVal.value())) {
        size_t len;
        const char* str = JS_ToCStringLen(c, &len, stackVal.value());
        if (str) {
            QString stack = QString::fromUtf8(str, len);
            JS_FreeCString(c, str);
            int lineNum = QuickJSConvert::parseLineNumberFromStack(stack);
            if (lineNum != -1) {
                outResult = QQuickJSValue(m_engine.data(), JS_NewInt32(c, lineNum), true);
                return true;
            }
        } else {
            clearPendingException(c);
        }
    }
    return false;
}

QQuickJSValue QQuickJSValue::property(const QString& name) const {
    ensureEngineThread();
    if (!isValid()) return QQuickJSValue();

    // Property access can run user JS (getters on the prototype chain,
    // Proxy traps): full entry bookkeeping, like the other JS entries.
    QQuickJSEngine* jsEngine = m_engine.data();
    JsEntryScope entryScope(jsEngine);

    if (JS_IsError(m_val) && (name == QLatin1String("lineNumber") || name == QLatin1String("line"))) {
        QQuickJSValue lineResult;
        if (tryExtractErrorLineNumber(name.toUtf8().constData(), lineResult)) {
            return lineResult;
        }
    }

    JSContext* c = ctx();

    JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
    if (atom == JS_ATOM_NULL) return fromException(m_engine.data());

    JSValue propVal = JS_GetProperty(c, m_val, atom);
    JS_FreeAtom(c, atom);
    if (JS_IsException(propVal)) return fromException(m_engine.data());
    return QQuickJSValue(m_engine.data(), propVal, true);
}

QQuickJSValue QQuickJSValue::property(const char* name) const {
    ensureEngineThread();
    if (!isValid() || !name) return QQuickJSValue();

    QQuickJSEngine* jsEngine = m_engine.data();
    JsEntryScope entryScope(jsEngine);

    if (JS_IsError(m_val) && (std::string_view(name) == "lineNumber" || std::string_view(name) == "line")) {
        QQuickJSValue lineResult;
        if (tryExtractErrorLineNumber(name, lineResult)) {
            return lineResult;
        }
    }

    JSContext* c = ctx();
    JSValue propVal = JS_GetPropertyStr(c, m_val, name);
    if (JS_IsException(propVal)) {
        return fromException(m_engine.data());
    }
    return QQuickJSValue(m_engine.data(), propVal, true);
}

QQuickJSValue QQuickJSValue::property(const QByteArray& name) const {
    return property(name.constData());
}

QQuickJSValue QQuickJSValue::property(QStringView name) const {
    return property(name.toString());
}

int QQuickJSValue::toInt() const {
    ensureEngineThread();
    JSContext* c = ctx();
    if (!c) return 0;
    if (JS_VALUE_GET_TAG(m_val) == JS_TAG_INT) return JS_VALUE_GET_INT(m_val);
    // ToNumber of an object runs user code (valueOf/toString).
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
        int32_t i = 0;
        if (JS_ToInt32(c, &i, m_val) < 0) {
            clearPendingException(c);
            return 0;
        }
        return i;
    }
    int32_t i = 0;
    if (JS_ToInt32(c, &i, m_val) < 0) {
        clearPendingException(c);
        return 0;
    }
    return i;
}

quint32 QQuickJSValue::toUInt() const {
    ensureEngineThread();
    JSContext* c = ctx();
    if (!c) return 0;
    // ToUint32 wraps negative numbers like QJSEngine::toUInt() does.
    if (JS_VALUE_GET_TAG(m_val) == JS_TAG_INT) return static_cast<quint32>(JS_VALUE_GET_INT(m_val));
    // ToNumber of an object runs user code (valueOf/toString).
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
        uint32_t u = 0;
        if (JS_ToUint32(c, &u, m_val) < 0) {
            clearPendingException(c);
            return 0;
        }
        return u;
    }
    uint32_t u = 0;
    if (JS_ToUint32(c, &u, m_val) < 0) {
        clearPendingException(c);
        return 0;
    }
    return u;
}

bool QQuickJSValue::toBool() const {
    ensureEngineThread();
    JSContext* c = ctx();
    if (!c) return false;
    if (JS_VALUE_GET_TAG(m_val) == JS_TAG_BOOL) return JS_VALUE_GET_BOOL(m_val) != 0;
    int ret = JS_ToBool(c, m_val);
    if (ret < 0) {
        clearPendingException(c);
        return false;
    }
    return ret > 0;
}

double QQuickJSValue::toDouble() const {
    ensureEngineThread();
    JSContext* c = ctx();
    if (!c) return std::numeric_limits<double>::quiet_NaN();
    // NORM_TAG (not GET_TAG): under NaN boxing a float64's raw tag never
    // equals JS_TAG_FLOAT64, and the fallback below would then route every
    // plain number through the slower JS_ToFloat64 path.
    const int tag = JS_VALUE_GET_NORM_TAG(m_val);
    if (tag == JS_TAG_INT) return static_cast<double>(JS_VALUE_GET_INT(m_val));
    if (tag == JS_TAG_FLOAT64) return JS_VALUE_GET_FLOAT64(m_val);
    // ToNumber of an object runs user code (valueOf/toString).
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
        double d = 0.0;
        if (JS_ToFloat64(c, &d, m_val) < 0) {
            clearPendingException(c);
            return std::numeric_limits<double>::quiet_NaN();
        }
        return d;
    }
    double d = 0.0;
    if (JS_ToFloat64(c, &d, m_val) < 0) {
        clearPendingException(c);
        return std::numeric_limits<double>::quiet_NaN();
    }
    return d;
}

QString QQuickJSValue::toString() const {
    ensureEngineThread();
    JSContext* c = ctx();
    if (!c) return QString();
    // ToString of an object runs user code (toString/valueOf/
    // Symbol.toPrimitive): entry bookkeeping applies; primitives (the hot
    // path of reading values back) take the direct conversion.
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
    }
    return QuickJSConvert::jsToQString(c, m_val);
}

QQuickJSValue QQuickJSValue::call(const QVariantList& args, const QQuickJSValue& thisObj) {
    if (!isValid() || !isSameContext(thisObj)) return QQuickJSValue();
    JSContext* c = ctx();
    if (!c) return QQuickJSValue();

    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();

    const int argc = static_cast<int>(args.size());
    QVarLengthArray<JSValue, InlineArgumentCount> jsArgs;
    jsArgs.resize(argc);
    // QVarLengthArray leaves trivial types uninitialized; the guard below
    // frees every element, so fill with JS_UNDEFINED first.
    std::ranges::fill(jsArgs, JS_UNDEFINED);

    ScopedJSValueArray guard(c, jsArgs);

    for (int i = 0; i < argc; ++i) {
        jsArgs[i] = jsEngine->toJSValue(args[i]);
        if (JS_IsException(jsArgs[i])) {
            return QQuickJSValue::fromException(jsEngine);
        }
    }

    return executeJsCall(jsEngine, c, m_val, jsArgs.data(), argc, thisObj, false);
}

QQuickJSValue QQuickJSValue::call(const QList<QQuickJSValue>& args, const QQuickJSValue& thisObj) {
    if (!isValid() || !isSameContext(thisObj) || !areArgsValid(args)) return QQuickJSValue();
    JSContext* c = ctx();
    if (!c) return QQuickJSValue();

    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();

    const int argc = static_cast<int>(args.size());
    QVarLengthArray<JSValue, InlineArgumentCount> jsArgs;
    jsArgs.resize(argc);
    // QVarLengthArray leaves trivial types uninitialized; the guard below
    // frees every element, so fill with JS_UNDEFINED first.
    std::ranges::fill(jsArgs, JS_UNDEFINED);

    ScopedJSValueArray guard(c, jsArgs);

    for (int i = 0; i < argc; ++i) {
        jsArgs[i] = JS_DupValue(c, args[i].jsVal());
    }

    return executeJsCall(jsEngine, c, m_val, jsArgs.data(), argc, thisObj, false);
}

QQuickJSValue QQuickJSValue::callWithInstance(const QQuickJSValue& instance, const QVariantList& args) {
    QQuickJSEngine* jsEngine = m_engine.data();
    if (!jsEngine) return QQuickJSValue();
    // QJSEngine semantics: a non-object instance binds the global object as
    // 'this' instead of the primitive value.
    return call(args, instance.isObject() ? instance : jsEngine->globalObject());
}

QQuickJSValue QQuickJSValue::callWithInstance(const QQuickJSValue& instance, const QList<QQuickJSValue>& args) {
    QQuickJSEngine* jsEngine = m_engine.data();
    if (!jsEngine) return QQuickJSValue();
    return call(args, instance.isObject() ? instance : jsEngine->globalObject());
}

QVariant QQuickJSValue::toVariant() const {
    const JSContext* c = ctx();
    if (!c) return QVariant();
    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();
    // Deep conversion reads properties: getters may run user JS.
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(jsEngine);
    }
    return jsEngine->toVariant(m_val);
}

QVariant QQuickJSValue::toVariant(ObjectConversionBehavior behavior) const {
    const JSContext* c = ctx();
    if (!c) return QVariant();
    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(jsEngine);
    }
    return jsEngine->toVariant(m_val, behavior);
}

QDateTime QQuickJSValue::toDateTime() const {
    ensureEngineThread();
    if (!isValid() || !JS_IsDate(m_val)) return QDateTime();
    // The shared object conversion path already maps Date objects to local
    // time QDateTime and invalid dates to an invalid QDateTime.
    return toVariant().toDateTime();
}

QObject* QQuickJSValue::toQObject() const {
    ensureEngineThread();
    JSContext* c = ctx();
    if (!c || !JS_IsObject(m_val)) return nullptr;
    const auto* data = static_cast<const JSQObjectData*>(JS_GetOpaque(m_val, m_engine->m_qobjectClassID));
    return data ? data->obj.data() : nullptr;
}

bool QQuickJSValue::hasProperty(const char* name) const {
    ensureEngineThread();
    if (!isValid()) return false;
    JSContext* c = ctx();
    // A Proxy target runs its `has` trap, which is user JS.
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
        ScopedJSAtom atom(c, name);
        if (!atom.isValid()) {
            clearPendingException(c);
            return false;
        }
        int res = JS_HasProperty(c, m_val, atom);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        return res > 0;
    }
    ScopedJSAtom atom(c, name);
    if (!atom.isValid()) {
        clearPendingException(c);
        return false;
    }
    int res = JS_HasProperty(c, m_val, atom);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res > 0;
}

bool QQuickJSValue::hasProperty(const QByteArray& name) const {
    return hasProperty(name.constData());
}

bool QQuickJSValue::hasProperty(QStringView name) const {
    return hasProperty(name.toString());
}

bool QQuickJSValue::hasProperty(const QString& name) const {
    ensureEngineThread();
    if (!isValid()) return false;
    JSContext* c = ctx();

    if (JS_IsObject(m_val)) {
        // A Proxy target runs its `has` trap, which is user JS.
        JsEntryScope entryScope(m_engine.data());
        JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
        if (atom == JS_ATOM_NULL) {
            clearPendingException(c);
            return false;
        }
        int res = JS_HasProperty(c, m_val, atom);
        JS_FreeAtom(c, atom);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        return res > 0;
    }

    JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
    if (atom == JS_ATOM_NULL) {
        clearPendingException(c);
        return false;
    }

    int res = JS_HasProperty(c, m_val, atom);
    JS_FreeAtom(c, atom);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res > 0;
}

void QQuickJSValue::setProperty(const char* name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (!isValid() || !name || !isSameContext(value)) return;

    // The write can run a setter on the prototype chain: entry bookkeeping
    // applies. On a starved stack the write runs under the emergency
    // budget; a setter too deep to fit throws, and the write becomes the
    // same silent no-op as any other failure of this void API.
    QQuickJSEngine* jsEngine = m_engine.data();
    JsEntryScope entryScope(jsEngine);

    JSContext* c = ctx();
    JSValue dup = JS_DupValue(c, value.m_val);
    if (JS_SetPropertyStr(c, m_val, name, dup) < 0) {
        clearPendingException(c);
    }
}

bool QQuickJSValue::deleteProperty(const char* name) {
    ensureEngineThread();
    if (!isValid()) return false;
    JSContext* c = ctx();
    // A Proxy target runs its deleteProperty trap, which is user JS.
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
        ScopedJSAtom atom(c, name);
        if (!atom.isValid()) {
            clearPendingException(c);
            return false;
        }
        int res = JS_DeleteProperty(c, m_val, atom, 0);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        return res == 1;
    }
    ScopedJSAtom atom(c, name);
    if (!atom.isValid()) {
        clearPendingException(c);
        return false;
    }
    int res = JS_DeleteProperty(c, m_val, atom, 0);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res == 1;
}

bool QQuickJSValue::deleteProperty(const QByteArray& name) {
    return deleteProperty(name.constData());
}

bool QQuickJSValue::deleteProperty(QStringView name) {
    return deleteProperty(name.toString());
}

bool QQuickJSValue::deleteProperty(const QString& name) {
    ensureEngineThread();
    if (!isValid()) return false;
    JSContext* c = ctx();

    if (JS_IsObject(m_val)) {
        // A Proxy target runs its deleteProperty trap, which is user JS.
        JsEntryScope entryScope(m_engine.data());
        JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
        if (atom == JS_ATOM_NULL) {
            clearPendingException(c);
            return false;
        }
        int res = JS_DeleteProperty(c, m_val, atom, 0);
        JS_FreeAtom(c, atom);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        return res == 1;
    }

    JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
    if (atom == JS_ATOM_NULL) {
        clearPendingException(c);
        return false;
    }

    int res = JS_DeleteProperty(c, m_val, atom, 0);
    JS_FreeAtom(c, atom);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res == 1;
}

bool QQuickJSValue::hasOwnProperty(const char* name) const {
    ensureEngineThread();
    if (!isValid() || !name) return false;
    JSContext* c = ctx();

    // A Proxy target runs its getOwnPropertyDescriptor trap, which is user JS.
    if (JS_IsObject(m_val)) {
        JsEntryScope entryScope(m_engine.data());
        ScopedJSAtom atom(c, name);
        if (!atom.isValid()) {
            clearPendingException(c);
            return false;
        }
        JSPropertyDescriptor desc;
        const int res = JS_GetOwnProperty(c, &desc, m_val, atom);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        if (res == 0) return false;
        JS_FreeValue(c, desc.value);
        JS_FreeValue(c, desc.getter);
        JS_FreeValue(c, desc.setter);
        return true;
    }

    ScopedJSAtom atom(c, name);
    if (!atom.isValid()) {
        clearPendingException(c);
        return false;
    }

    JSPropertyDescriptor desc;
    const int res = JS_GetOwnProperty(c, &desc, m_val, atom);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    if (res == 0) return false;

    // Accessor descriptors also carry owned getter/setter refs (they are
    // JS_UNDEFINED for data properties); mirror the engine's js_free_desc().
    JS_FreeValue(c, desc.value);
    JS_FreeValue(c, desc.getter);
    JS_FreeValue(c, desc.setter);
    return true;
}

bool QQuickJSValue::hasOwnProperty(const QByteArray& name) const {
    return hasOwnProperty(name.constData());
}

bool QQuickJSValue::hasOwnProperty(QStringView name) const {
    return hasOwnProperty(name.toString());
}

bool QQuickJSValue::hasOwnProperty(const QString& name) const {
    ensureEngineThread();
    if (!isValid()) return false;
    JSContext* c = ctx();

    if (JS_IsObject(m_val)) {
        // A Proxy target runs its getOwnPropertyDescriptor trap, which is
        // user JS.
        JsEntryScope entryScope(m_engine.data());
        JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
        if (atom == JS_ATOM_NULL) {
            clearPendingException(c);
            return false;
        }
        JSPropertyDescriptor desc;
        const int res = JS_GetOwnProperty(c, &desc, m_val, atom);
        JS_FreeAtom(c, atom);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        if (res == 0) return false;
        JS_FreeValue(c, desc.value);
        JS_FreeValue(c, desc.getter);
        JS_FreeValue(c, desc.setter);
        return true;
    }

    JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
    if (atom == JS_ATOM_NULL) {
        clearPendingException(c);
        return false;
    }

    JSPropertyDescriptor desc;
    const int res = JS_GetOwnProperty(c, &desc, m_val, atom);
    JS_FreeAtom(c, atom);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    if (res == 0) return false;

    // Accessor descriptors also carry owned getter/setter refs (they are
    // JS_UNDEFINED for data properties); mirror the engine's js_free_desc().
    JS_FreeValue(c, desc.value);
    JS_FreeValue(c, desc.getter);
    JS_FreeValue(c, desc.setter);
    return true;
}

QQuickJSValue QQuickJSValue::prototype() const {
    ensureEngineThread();
    if (!isValid() || !JS_IsObject(m_val)) return QQuickJSValue();
    QQuickJSEngine* jsEngine = m_engine.data();
    // A Proxy target runs its getPrototypeOf trap, which is user JS.
    JsEntryScope entryScope(jsEngine);
    JSContext* c = ctx();
    JSValue proto = JS_GetPrototype(c, m_val);
    if (JS_IsException(proto)) {
        return fromException(m_engine.data());
    }
    return QQuickJSValue(m_engine.data(), proto, true);
}

void QQuickJSValue::setPrototype(const QQuickJSValue& prototype) {
    ensureEngineThread();
    if (!isValid() || !JS_IsObject(m_val)) return;
    if (!isSameContext(prototype)) return;
    // Only an object or JS null is a valid prototype; anything else is
    // ignored, matching QJSEngine::setPrototype().
    if (!JS_IsObject(prototype.m_val) && !JS_IsNull(prototype.m_val)) return;
    QQuickJSEngine* jsEngine = m_engine.data();
    // A Proxy target runs its setPrototypeOf trap, which is user JS.
    JsEntryScope entryScope(jsEngine);
    JSContext* c = ctx();
    if (JS_SetPrototype(c, m_val, prototype.m_val) < 0) {
        clearPendingException(c);
    }
}

QQuickJSValue QQuickJSValue::property(quint32 arrayIndex) const {
    ensureEngineThread();
    if (!isValid()) return QQuickJSValue();
    QQuickJSEngine* jsEngine = m_engine.data();
    JsEntryScope entryScope(jsEngine);
    JSContext* c = ctx();
    JSValue propVal = JS_GetPropertyUint32(c, m_val, arrayIndex);

    if (JS_IsException(propVal)) {
        return fromException(m_engine.data());
    }
    return QQuickJSValue(m_engine.data(), propVal, true);
}

void QQuickJSValue::setProperty(const QByteArray& name, const QQuickJSValue& value) {
    setProperty(name.constData(), value);
}

void QQuickJSValue::setProperty(QStringView name, const QQuickJSValue& value) {
    setProperty(name.toString(), value);
}

void QQuickJSValue::setProperty(const QString& name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (!isValid() || !isSameContext(value)) return;

    QQuickJSEngine* jsEngine = m_engine.data();
    JsEntryScope entryScope(jsEngine);

    JSContext* c = ctx();

    JSAtom atom = QuickJSConvert::cachedStringAtom(m_engine.data(), c, name);
    if (atom == JS_ATOM_NULL) {
        clearPendingException(c);
        return;
    }

    JSValue dup = JS_DupValue(c, value.m_val);
    if (JS_SetProperty(c, m_val, atom, dup) < 0) {
        JS_FreeAtom(c, atom);
        clearPendingException(c);
        return;
    }
    JS_FreeAtom(c, atom);
}

void QQuickJSValue::setProperty(const QString& name, const QVariant& value) {
    JSContext* c = ctx();
    if (!c) return;
    QQuickJSEngine* jsEngine = m_engine.data();
    if (!jsEngine) return;
    ensureEngineThread();
    JSValue v = jsEngine->toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(c);
        return;
    }
    setProperty(name, QQuickJSValue(jsEngine, v, true));
}

void QQuickJSValue::setProperty(QStringView name, const QVariant& value) {
    setProperty(name.toString(), value);
}

void QQuickJSValue::setProperty(const char* name, const QVariant& value) {
    JSContext* c = ctx();
    if (!c || !name) return;
    QQuickJSEngine* jsEngine = m_engine.data();
    if (!jsEngine) return;
    ensureEngineThread();
    JSValue v = jsEngine->toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(c);
        return;
    }
    setProperty(name, QQuickJSValue(jsEngine, v, true));
}

void QQuickJSValue::setProperty(quint32 arrayIndex, const QQuickJSValue& value) {
    ensureEngineThread();
    if (!isValid() || !isSameContext(value)) return;

    QQuickJSEngine* jsEngine = m_engine.data();
    JsEntryScope entryScope(jsEngine);

    JSContext* c = ctx();
    if (JS_SetPropertyUint32(c, m_val, arrayIndex, JS_DupValue(c, value.m_val)) < 0) {
        clearPendingException(c);
    }
}

void QQuickJSValue::setProperty(quint32 arrayIndex, const QVariant& value) {
    JSContext* c = ctx();
    if (!c) return;
    QQuickJSEngine* jsEngine = m_engine.data();
    if (!jsEngine) return;
    ensureEngineThread();
    JSValue v = jsEngine->toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(c);
        return;
    }
    setProperty(arrayIndex, QQuickJSValue(jsEngine, v, true));
}

bool QQuickJSValue::equals(const QQuickJSValue& other) const {
    ensureEngineThread();
    if (!isValid() || !isSameContext(other)) return false;
    JSContext* c = ctx();
    // ECMA-262 abstract equality: the check can run user code (valueOf)
    // and report an exception instead of a verdict.
    if (JS_IsObject(m_val) || JS_IsObject(other.m_val)) {
        JsEntryScope entryScope(m_engine.data());
        const int res = JS_IsEqual(c, m_val, other.m_val);
        if (res < 0) {
            clearPendingException(c);
            return false;
        }
        return res != 0;
    }
    const int res = JS_IsEqual(c, m_val, other.m_val);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res != 0;
}

bool QQuickJSValue::strictlyEquals(const QQuickJSValue& other) const {
    ensureEngineThread();
    if (!isValid() || !isSameContext(other)) return false;
    JSContext* c = ctx();
    return JS_IsStrictEqual(c, m_val, other.m_val);
}

QQuickJSValue QQuickJSValue::construct(const QVariantList& args) {
    if (!isValid()) return QQuickJSValue();
    JSContext* c = ctx();
    if (!c) return QQuickJSValue();

    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();

    const int argc = static_cast<int>(args.size());
    QVarLengthArray<JSValue, InlineArgumentCount> jsArgs;
    jsArgs.resize(argc);
    // QVarLengthArray leaves trivial types uninitialized; the guard below
    // frees every element, so fill with JS_UNDEFINED first.
    std::ranges::fill(jsArgs, JS_UNDEFINED);

    ScopedJSValueArray guard(c, jsArgs);

    for (int i = 0; i < argc; ++i) {
        jsArgs[i] = jsEngine->toJSValue(args[i]);
        if (JS_IsException(jsArgs[i])) {
            return QQuickJSValue::fromException(jsEngine);
        }
    }

    return executeJsCall(jsEngine, c, m_val, jsArgs.data(), argc, *this, true);
}

QQuickJSValue QQuickJSValue::callAsConstructor(const QList<QQuickJSValue>& args) {
    if (!isValid() || !areArgsValid(args)) return QQuickJSValue();
    JSContext* c = ctx();
    if (!c) return QQuickJSValue();

    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();

    const int argc = static_cast<int>(args.size());
    QVarLengthArray<JSValue, InlineArgumentCount> jsArgs;
    jsArgs.resize(argc);
    // QVarLengthArray leaves trivial types uninitialized; the guard below
    // frees every element, so fill with JS_UNDEFINED first.
    std::ranges::fill(jsArgs, JS_UNDEFINED);

    ScopedJSValueArray guard(c, jsArgs);

    for (int i = 0; i < argc; ++i) {
        jsArgs[i] = JS_DupValue(c, args[i].jsVal());
    }

    return executeJsCall(jsEngine, c, m_val, jsArgs.data(), argc, *this, true);
}

bool QQuickJSValue::instanceOf(const QQuickJSValue& other) const {
    ensureEngineThread();
    if (!isValid() || !isSameContext(other) || !JS_IsObject(other.m_val)) return false;
    JSContext* c = ctx();
    if (!c) return false;

    // instanceof consults @@hasInstance, which may be user code - and the
    // SPEC runs it based on the RIGHT operand regardless of the left one's
    // type (OrdinaryHasInstance is reached through the method even for a
    // primitive left side), so the entry bookkeeping cannot be gated on
    // JS_IsObject(m_val). Nested (depth > 0) this is a pair of integer ops.
    JsEntryScope entryScope(m_engine.data());
    int res = JS_IsInstanceOf(c, m_val, other.m_val);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res != 0;
}
