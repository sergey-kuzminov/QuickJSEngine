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
    const bool topLevel = (engine->m_evalDepth == 0);
    if (topLevel) {
        engine->resetExecutionState();
    }

    EvalDepthGuard depthGuard(engine->m_evalDepth);
    // Declared after depthGuard so the flush (which may run user JS via reason
    // toString) happens while the depth guard is still held. Top-level calls
    // only: nested calls must not report the enclosing tick.
    auto rejectionFlush = qScopeGuard([engine, topLevel]() { if (topLevel) engine->flushUnhandledRejections(); });

    JSValue ret = isConstructor
                      ? JS_CallConstructor(c, funcVal, argc, jsArgs)
                      : JS_Call(c, funcVal, thisObj.jsVal(), argc, jsArgs);

    ScopedJSValue retScoped(c, ret);

    const bool hadException = JS_IsException(ret);
    QQuickJSValue errVal;
    if (hadException) {
        errVal = QQuickJSValue::fromException(engine);
    }

    if (engine->m_autoProcessPromises && JS_IsJobPending(engine->m_rt)) {
        engine->processPendingJobs();
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
    int32_t i = 0;
    if (JS_ToInt32(c, &i, m_val) < 0) {
        clearPendingException(c);
        return 0;
    }
    return i;
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
    const int tag = JS_VALUE_GET_TAG(m_val);
    if (tag == JS_TAG_INT) return static_cast<double>(JS_VALUE_GET_INT(m_val));
    if (tag == JS_TAG_FLOAT64) return JS_VALUE_GET_FLOAT64(m_val);
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

QVariant QQuickJSValue::toVariant() const {
    const JSContext* c = ctx();
    if (!c) return QVariant();
    QQuickJSEngine* jsEngine = m_engine.data();
    ensureEngineThread();
    return jsEngine->toVariant(m_val);
}

bool QQuickJSValue::hasProperty(const char* name) const {
    ensureEngineThread();
    if (!isValid()) return false;
    JSContext* c = ctx();
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
    if (!isValid() || !isSameContext(value)) return;

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

QQuickJSValue QQuickJSValue::property(quint32 arrayIndex) const {
    ensureEngineThread();
    if (!isValid()) return QQuickJSValue();
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
    JSValue v = jsEngine->toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(c);
        return;
    }
    setProperty(arrayIndex, QQuickJSValue(jsEngine, v, true));
}

bool QQuickJSValue::strictlyEquals(const QQuickJSValue& other) const {
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

    int res = JS_IsInstanceOf(c, m_val, other.m_val);
    if (res < 0) {
        clearPendingException(c);
        return false;
    }
    return res != 0;
}
