//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSConvert.h"
#include "QuickJSEngine_p.h"

#include <QDateTime>
#include <limits>

#include <QRegularExpression>

QString QuickJSConvert::jsToQString(JSContext* ctx, JSValueConst val) {
    size_t len;
    const uint16_t* s = JS_ToCStringLenUTF16(ctx, &len, val);
    if (s) {
        QString str = QString(reinterpret_cast<const QChar*>(s), static_cast<qsizetype>(len));
        JS_FreeCStringUTF16(ctx, s);
        return str;
    }
    clearPendingException(ctx);
    return QString();
}

int QuickJSConvert::parseLineNumberFromStack(const QString& stack) {
    if (stack.isEmpty()) return -1;

    static const QRegularExpression reFull(QStringLiteral(":(\\d+):(\\d+)"));
    QRegularExpressionMatch match = reFull.match(stack);
    if (match.hasMatch()) {
        bool ok;
        int line = match.captured(1).toInt(&ok);
        if (ok) return line;
    }

    static const QRegularExpression reLine(QStringLiteral(":(\\d+)\\s*$"));
    match = reLine.match(stack);
    if (match.hasMatch()) {
        bool ok;
        int line = match.captured(1).toInt(&ok);
        if (ok) return line;
    }
    return -1;
}

int QuickJSConvert::extractExceptionLine(JSContext* ctx, JSValueConst exc) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
    int line = -1;

    ScopedJSValue lineVal(ctx, JS_GetProperty(ctx, exc, engine->m_atoms.lineNumber));
    if (consumeException(ctx, lineVal.value())) {
        lineVal = ScopedJSValue(ctx, JS_UNDEFINED);
    } else if (JS_IsUndefined(lineVal.value())) {
        lineVal = ScopedJSValue(ctx, JS_GetProperty(ctx, exc, engine->m_atoms.line));
        if (consumeException(ctx, lineVal.value())) lineVal = ScopedJSValue(ctx, JS_UNDEFINED);
    }

    if (JS_IsNumber(lineVal.value())) {
        if (JS_ToInt32(ctx, &line, lineVal.value()) < 0) {
            clearPendingException(ctx);
            line = -1;
        }
        return line;
    }

    ScopedJSValue stackVal(ctx, JS_GetProperty(ctx, exc, engine->m_atoms.stack));
    if (consumeException(ctx, stackVal.value())) return -1;

    size_t len;
    const char* s = JS_ToCStringLen(ctx, &len, stackVal.value());
    if (s) {
        QString stack = QString::fromUtf8(s, len);
        JS_FreeCString(ctx, s);
        line = parseLineNumberFromStack(stack);
    } else {
        clearPendingException(ctx);
    }
    return line;
}

// P15 (2026-11-08), quickjs-ng #1894: JS_GetTypedArrayBuffer returns a stale
// ta->length (the view's length at CREATION time) for a length-tracking view
// over a resizable ArrayBuffer: resize() down causes an out-of-bounds heap
// read (ASan: READ of size 64 from a realloc'd 7-byte block), resize() up
// silently truncates. JS_GetArrayBuffer is always honest, so the view span is
// clamped to the live buffer, element-aligned (JS ta.byteLength semantics:
// floor((byteLength-offset)/bpe)*bpe). After the upstream #1894 fix the
// condition always holds - the clamp becomes a no-op. Buffer growth is not
// cured by the binding: fixed/tracking views cannot be told apart via the
// public API (waiting for upstream; see 03_tests "RAB grow").
static QByteArray clampedByteArray(const uint8_t* buf, size_t bufSize,
                                   size_t byteOffset, size_t byteLength,
                                   size_t bytesPerElement)
{
    if (byteOffset >= bufSize) return QByteArray();   // view entirely past the end
    size_t span = bufSize - byteOffset;
    if (bytesPerElement > 1) span -= span % bytesPerElement;
    return QByteArray(reinterpret_cast<const char*>(buf + byteOffset),
                      static_cast<qsizetype>(qMin(byteLength, span)));
}

// Non-throwing buffer conversion for the ArrayBuffer/TypedArray/
// SharedArrayBuffer family: returns a QByteArray wrapped in a QVariant, or
// an invalid QVariant for anything else (including detached buffers).
// JS_GetArrayBuffer and JS_GetTypedArrayBuffer throw a TypeError on any
// non-buffer, so the probes clear pending exceptions instead of propagating.
static QVariant jsBufferToVariant(JSContext* ctx, JSValueConst val)
{
    if (JS_IsArrayBuffer(val)) {
        size_t bufSize = 0;
        uint8_t* buf = JS_GetArrayBuffer(ctx, &bufSize, val);
        if (buf) return QVariant(QByteArray(reinterpret_cast<const char*>(buf), static_cast<qsizetype>(bufSize)));
        clearPendingException(ctx);   // detached buffer: fall through
    } else if (JS_GetTypedArrayType(val) >= 0) {
        size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
        ScopedJSValue abuf(ctx, JS_GetTypedArrayBuffer(ctx, val, &byteOffset, &byteLength, &bytesPerElement));
        if (consumeException(ctx, abuf.value())) abuf = ScopedJSValue(ctx, JS_UNDEFINED);
        if (!JS_IsNull(abuf.value()) && !JS_IsUndefined(abuf.value())) {
            size_t bufSize = 0;
            uint8_t* buf = JS_GetArrayBuffer(ctx, &bufSize, abuf.value());
            if (buf) return QVariant(clampedByteArray(buf, bufSize, byteOffset, byteLength, bytesPerElement));
        }
    } else {
        // SharedArrayBuffer has no non-throwing test; the byteLength probe
        // (one prototype-chain lookup) keeps plain maps off the throw path.
        ScopedJSValue bl(ctx, JS_GetPropertyStr(ctx, val, "byteLength"));
        if (JS_IsException(bl.value())) {
            clearPendingException(ctx);
        } else if (!JS_IsUndefined(bl.value())) {
            size_t bufSize = 0;
            uint8_t* buf = JS_GetArrayBuffer(ctx, &bufSize, val);
            if (buf) return QVariant(QByteArray(reinterpret_cast<const char*>(buf), static_cast<qsizetype>(bufSize)));

            clearPendingException(ctx);

            size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
            ScopedJSValue abuf(ctx, JS_GetTypedArrayBuffer(ctx, val, &byteOffset, &byteLength, &bytesPerElement));
            if (consumeException(ctx, abuf.value())) abuf = ScopedJSValue(ctx, JS_UNDEFINED);
            if (!JS_IsNull(abuf.value()) && !JS_IsUndefined(abuf.value())) {
                buf = JS_GetArrayBuffer(ctx, &bufSize, abuf.value());
                if (buf) return QVariant(clampedByteArray(buf, bufSize, byteOffset, byteLength, bytesPerElement));
            }
        }
    }
    return QVariant();
}

QVariant QuickJSConvert::variantFromJSObject(QQuickJSEngine* engine, JSContext* ctx, JSValueConst val,
                                            JSClassID qobjectClassID, JSClassID objectClassID,
                                            JSVisitedStack& visited, int depth,
                                            QQuickJSValue::ObjectConversionBehavior behavior)
{
    auto data = static_cast<JSQObjectData*>(JS_GetOpaque(val, qobjectClassID));
    if (data && !data->obj.isNull()) {
        return QVariant::fromValue(data->obj.data());
    }

    if (behavior == QQuickJSValue::RetainJSObjects) {
        // QJSEngine RetainJSObjects semantics (verified against Qt 6.12):
        // native-mapped families still convert (QObject wrappers above, Date
        // is handled upstream); buffers keep the QByteArray conversion; every
        // other object, arrays included, is retained as QVariant<QQuickJSValue>.
        if (JS_GetClassID(val) != objectClassID) {
            const QVariant buffer = jsBufferToVariant(ctx, val);
            if (buffer.isValid()) return buffer;
        }
        return QVariant::fromValue(QQuickJSValue(engine, JS_DupValue(ctx, val), true));
    }

    if (JS_IsArray(val)) {
        QVariantList list;
        ScopedJSValue lengthVal(ctx, JS_GetProperty(ctx, val, engine->m_atoms.length));
        uint32_t len = 0;
        if (JS_ToUint32(ctx, &len, lengthVal.value()) < 0) {
            clearPendingException(ctx);
            len = 0;
        }

        constexpr uint32_t MaxArrayConversionLength = 1000000;
        if (len > MaxArrayConversionLength) {
            qWarning("QQuickJSEngine: Array conversion length %u exceeds limit %u, returning empty QVariant", len, MaxArrayConversionLength);
            return QVariant();
        }

        list.reserve(qMin<uint32_t>(len, 16384));

        for (uint32_t i = 0; i < len; ++i) {
            ScopedJSValue item(ctx, JS_GetPropertyUint32(ctx, val, i));
            if (JS_IsException(item.value())) {
                qWarning("QQuickJSEngine: Exception thrown while reading array property, returning null QVariant");
                clearPendingException(ctx);
                list.append(QVariant());
            } else {
                list.append(engine->toVariantImpl(item.value(), visited, depth, behavior));
            }
        }
        return list;
    }

    if (JS_GetClassID(val) != objectClassID) {
        const QVariant buffer = jsBufferToVariant(ctx, val);
        if (buffer.isValid()) return buffer;

        // QJSEngine retains function objects even in the lossy conversion
        // (verified against Qt 6.12); the remaining exotic objects yield an
        // invalid QVariant instead of an empty QVariantMap to avoid hidden
        // data loss.
        if (JS_IsFunction(ctx, val)) {
            return QVariant::fromValue(QQuickJSValue(engine, JS_DupValue(ctx, val), true));
        }
        if (JS_IsMap(val) || JS_IsSet(val) || JS_IsWeakMap(val) || JS_IsWeakSet(val) ||
            JS_IsPromise(val) || JS_IsRegExp(val) || JS_IsDataView(val)) {
            return QVariant();
        }
    }

    QVariantMap map;
    JSPropertyEnum *props = nullptr;
    uint32_t n_props = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &n_props, val, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        qWarning("QQuickJSEngine: Failed to get property names during conversion to QVariantMap");
        clearPendingException(ctx);
        return QVariant();
    }

    for (uint32_t i = 0; i < n_props; ++i) {
        JSAtom atom = props[i].atom;

        // Fetch the value first: an enumerable getter may run user JS and
        // trigger other atom-name lookups, invalidating the key reference.
        ScopedJSValue item(ctx, JS_GetProperty(ctx, val, atom));
        const QString& keyStr = atomName(engine, ctx, atom);
        if (keyStr.isNull()) {
            qWarning("QQuickJSEngine: Failed to get property name during conversion to QVariantMap");
            clearPendingException(ctx);
            continue;
        }

        if (JS_IsException(item.value())) {
            qWarning("QQuickJSEngine: Exception thrown while reading object property, returning null QVariant");
            clearPendingException(ctx);
            map.insert(keyStr, QVariant());
        } else if (JS_IsObject(item.value())) {
            // Object values may recurse into atomName(); copy the key first.
            const QString key = keyStr;
            QVariant converted = engine->toVariantImpl(item.value(), visited, depth, behavior);
            map.insert(key, converted);
        } else {
            // Scalar values perform no atom lookups; the reference stays valid.
            map.insert(keyStr, engine->toVariantImpl(item.value(), visited, depth, behavior));
        }
    }

    JS_FreePropertyEnum(ctx, props, n_props);
    return map;
}

JSAtom QuickJSConvert::cachedStringAtom(QQuickJSEngine* engine, JSContext* ctx, const QString& key) {
    auto it = engine->m_stringAtomCache.constFind(key);
    if (it != engine->m_stringAtomCache.constEnd()) {
        return JS_DupAtom(ctx, it.value());
    }

    JSAtom atom = atomFromUtf8(ctx, key.toUtf8());
    if (atom == JS_ATOM_NULL) return JS_ATOM_NULL;

    if (engine->m_stringAtomCache.size() < AtomNameCacheLimit) {
        JSAtom dup = JS_DupAtom(ctx, atom);
        if (dup != JS_ATOM_NULL) {
            try {
                engine->m_stringAtomCache.insert(key, dup);
            } catch (...) {
                JS_FreeAtom(ctx, dup);
            }
        }
    }
    return atom;   // reference owned by the caller
}

const QString& QuickJSConvert::atomName(QQuickJSEngine* engine, JSContext* ctx, JSAtom atom) {
    const auto it = engine->m_atomNameCache.constFind(atom);
    if (it != engine->m_atomNameCache.constEnd()) return it.value();

    // Miss: render the name and cache it while under the capacity limit.
    // Above the limit the name lands in the engine-owned fallback slot; the
    // reference contract is documented in the header.
    QString name;
    ScopedCString key(ctx, JS_AtomToCString(ctx, atom));
    if (key.isValid()) {
        name = QString::fromUtf8(key.value());
        if (engine->m_atomNameCache.size() < AtomNameCacheLimit) {
            JSAtom dupAtom = JS_DupAtom(ctx, atom);
            if (dupAtom != JS_ATOM_NULL) {
                try {
                    return engine->m_atomNameCache.insert(dupAtom, name).value();
                } catch (...) {
                    JS_FreeAtom(ctx, dupAtom);
                }
            }
        }
    } else {
        clearPendingException(ctx);
    }
    engine->m_atomNameFallback = std::move(name);
    return engine->m_atomNameFallback;
}

JSValue QuickJSConvert::mapToJSObject(QQuickJSEngine* engine, JSContext* ctx, const QVariantMap* mapPtr, int depth) {
    // Do not rewrite this loop over JS_NewObjectFrom: its fail path in
    // quickjs-ng v0.18.0 double-frees values[i] when JS_SetProperty fails
    // (e.g. a throwing setter on Object.prototype). The atom cache below
    // already provides the main win for maps.
    if (depth > MaxVariantConversionDepth) {
        JS_ThrowRangeError(ctx, "Variant nesting too deep");
        return JS_EXCEPTION;
    }

    ScopedJSValue obj(ctx, JS_NewObject(ctx));
    if (JS_IsException(obj.value())) return JS_EXCEPTION;

    for (auto it = mapPtr->constBegin(); it != mapPtr->constEnd(); ++it) {
        ScopedJSValue val(ctx, engine->toJSValueImpl(it.value(), depth + 1));
        if (JS_IsException(val.value())) return JS_EXCEPTION;

        JSAtom atom = cachedStringAtom(engine, ctx, it.key());
        if (atom == JS_ATOM_NULL) return JS_EXCEPTION;

        JSValue v = val.release();
        if (JS_DefinePropertyValue(ctx, obj.value(), atom, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, atom);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, atom);
    }
    return obj.release();
}

JSValue QuickJSConvert::hashToJSObject(QQuickJSEngine* engine, JSContext* ctx, const QVariantHash* hashPtr, int depth) {
    // See the note in mapToJSObject: JS_NewObjectFrom is unsafe on its fail
    // path in v0.18.0, stay with per-key DefineProperty + atom cache.
    if (depth > MaxVariantConversionDepth) {
        JS_ThrowRangeError(ctx, "Variant nesting too deep");
        return JS_EXCEPTION;
    }

    ScopedJSValue obj(ctx, JS_NewObject(ctx));
    if (JS_IsException(obj.value())) return JS_EXCEPTION;

    for (auto it = hashPtr->constBegin(); it != hashPtr->constEnd(); ++it) {
        ScopedJSValue val(ctx, engine->toJSValueImpl(it.value(), depth + 1));
        if (JS_IsException(val.value())) return JS_EXCEPTION;

        JSAtom atom = cachedStringAtom(engine, ctx, it.key());
        if (atom == JS_ATOM_NULL) return JS_EXCEPTION;

        JSValue v = val.release();
        if (JS_DefinePropertyValue(ctx, obj.value(), atom, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, atom);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, atom);
    }
    return obj.release();
}

// Frees accumulated JSValues on an early exit, before ownership is passed
// to JS_NewArrayFrom (which consumes the values exactly once, on all paths).
static void freePendingValues(JSContext* ctx, std::span<const JSValue> values) {
    for (JSValue v : values) JS_FreeValue(ctx, v);
}

JSValue QuickJSConvert::listToJSArray(QQuickJSEngine* engine, JSContext* ctx, const QVariantList* listPtr, int depth) {
    if (depth > MaxVariantConversionDepth) {
        JS_ThrowRangeError(ctx, "Variant nesting too deep");
        return JS_EXCEPTION;
    }

    // Bulk assembly: values are collected in a buffer and handed to
    // JS_NewArrayFrom (one allocation + memcpy) instead of per-element
    // appends with geometric reallocations.
    QVarLengthArray<JSValue, 32> values;
    values.reserve(listPtr->size());

    for (const QVariant& item : *listPtr) {
        ScopedJSValue val(ctx, engine->toJSValueImpl(item, depth + 1));
        if (JS_IsException(val.value())) {
            freePendingValues(ctx, values);
            return JS_EXCEPTION;
        }
        values.append(val.release());
    }

    return JS_NewArrayFrom(ctx, static_cast<int>(values.size()), values.constData());
}

JSValue QuickJSConvert::stringListToJSArray(QQuickJSEngine* engine, JSContext* ctx, const QStringList* listPtr) {
    Q_UNUSED(engine);
    // Bulk assembly, as in listToJSArray.
    QVarLengthArray<JSValue, 32> values;
    values.reserve(listPtr->size());

    for (const QString& item : *listPtr) {
        ScopedJSValue jsItem(ctx, qStringToJS(ctx, item));
        if (JS_IsException(jsItem.value())) {
            freePendingValues(ctx, values);
            return JS_EXCEPTION;
        }
        values.append(jsItem.release());
    }

    return JS_NewArrayFrom(ctx, static_cast<int>(values.size()), values.constData());
}

int QuickJSConvert::adjustEnumType(int type) {
    if (QMetaType(type).flags() & QMetaType::IsEnumeration) return QMetaType::Int;
    return type;
}

// ---------------------------------------------------------------------------
// Scalar fast paths (shared by toJSValueImpl and the metacall sites)
// ---------------------------------------------------------------------------

// qint64 ladder: Int32 when exact, Float64 within the safe integer range,
// BigInt beyond it (deliberate precision-preserving extension).
JSValue QuickJSConvert::int64ToJS(JSContext* ctx, qint64 v) {
    if (v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max()) {
        return JS_NewInt32(ctx, static_cast<int32_t>(v));
    }
    if (v >= -9007199254740991LL && v <= 9007199254740991LL) {
        return JS_NewFloat64(ctx, static_cast<double>(v));
    }
    return JS_NewBigInt64(ctx, v);
}

JSValue QuickJSConvert::uint64ToJS(JSContext* ctx, quint64 v) {
    if (v <= static_cast<quint64>(std::numeric_limits<int32_t>::max())) {
        return JS_NewInt32(ctx, static_cast<int32_t>(v));
    }
    if (v <= 9007199254740991ULL) {
        return JS_NewFloat64(ctx, static_cast<double>(v));
    }
    return JS_NewBigUint64(ctx, v);
}

JSValue QuickJSConvert::dateTimeToJS(JSContext* ctx, const QDateTime& dt) {
    return JS_NewDate(ctx, static_cast<double>(dt.toMSecsSinceEpoch()));
}

JSValue QuickJSConvert::dateToJS(JSContext* ctx, const QDate& d) {
    return dateTimeToJS(ctx, QDateTime(d, QTime(0, 0)));
}

JSValue QuickJSConvert::timeToJS(JSContext* ctx, const QTime& t) {
    return qStringToJS(ctx, t.toString(Qt::ISODate));
}

JSValue QuickJSConvert::byteArrayToJS(JSContext* ctx, const QByteArray& ba) {
    return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(ba.constData()),
                                  static_cast<size_t>(ba.size()));
}
