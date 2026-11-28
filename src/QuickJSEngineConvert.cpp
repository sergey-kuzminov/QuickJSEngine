//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

// QVariant <-> JSValue conversion ladders of QQuickJSEngine (one class,
// several translation units - see QuickJSEngine_p.h): toJSValue()/
// toJSValueImpl() with the value-type dispatch, toVariant()/toVariantImpl()
// with cycle detection, and toJSValueFromVoid() for metacall read slots.
// The scalar fast paths live in QuickJSConvert (QuickJSConvert.h).

#include "QuickJSEngine_p.h"
#include "QuickJSConvert.h"
#include "QuickJSQObjectProxy.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

#include <cmath>
#include <limits>

namespace {

// Stack reserve for the QVariant conversion recursion (toVariantImpl <->
// variantFromJSObject): the C++ conversion chain runs outside the JS stack
// guard, so it needs its own floor check. One conversion level costs a frame
// pair plus the property-enumeration machinery - tens of KiB on fat
// Debug/ASan builds, a few KiB in release. 96 KiB covers the deepest
// legitimate single level (pair + JS_GetOwnPropertyNames + unwinding) on
// any known build while still allowing 60+ nesting levels on healthy
// stacks. The reserve deliberately sits 32 KiB closer to the floor than
// the JS guard's kSafetyMargin (128 KiB): the JS window must additionally
// fit the interpreter's per-frame overhead, the interrupt-handler path and
// a full JS throw + unwind below the deepest JS frame, while a conversion
// level is plain C++ recursion whose only below-the-line obligation is its
// own throw path (the RangeError construction is pure C).
constexpr size_t kVariantStackReserve = 96 * 1024;
} // namespace

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

    // C++ recursion guard for the C++-to-JS direction, symmetric with the
    // floor check in toVariantImpl(): toJSValueImpl <-> listToJSArray/
    // mapToJSObject/hashToJSObject recurse per nesting level with the same
    // depth cap but OUTSIDE the JS stack guard - on fat Debug/ASan builds
    // 64 levels of container conversion exhaust the real thread stack just
    // like the reverse direction. Bail out with the same catchable
    // RangeError the depth cap uses once the remaining stack distance to
    // the thread floor drops below the conversion reserve.
    //
    // Unknown floor (0): the floor check then has nothing to compare
    // against, so the depth-0 entry records its own SP
    // (m_conversionAnchorSp) and deeper levels bound the TOTAL conversion
    // descent by the assumed usable window below that anchor instead - a
    // weaker but sound bound that keeps the guard meaningful on platforms
    // without floor measurement.
    if (depth == 0)
        m_conversionAnchorSp = currentStackPointer();
    if (depth > 0) {
        const uintptr_t sp = currentStackPointer();
        const uintptr_t realFloor = threadStackFloor(sp);
        const uintptr_t floor = realFloor != 0
            ? realFloor
            : (m_conversionAnchorSp > kUnknownFloorAssumedUsable
                   ? m_conversionAnchorSp - kUnknownFloorAssumedUsable : 0);
        if (Q_UNLIKELY(floor != 0 && sp < floor + kVariantStackReserve)) {
            JS_ThrowRangeError(m_ctx, "Variant nesting too deep");
            return JS_EXCEPTION;
        }
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

    if (var.userType() == QMetaType::QVariant) return toJSValueImpl(var.value<QVariant>(), depth);

    if (QMetaType(var.userType()).flags() & QMetaType::IsEnumeration) {
        // Enumerations convert through the declared width: toInt() truncates
        // a quint64-backed enum (1 << 40 reads back as 0). IsUnsigned-
        // Enumeration selects the signedness of the underlying read, the
        // same policy the metacall ladders apply (adjustEnumType and the
        // sizeOf guards). Narrow enums come out as plain int32 numbers,
        // int-backed ones never even reach here (the fast paths).
        const QMetaType enumMt(var.userType());
        if (enumMt.flags() & QMetaType::IsUnsignedEnumeration)
            return QuickJSConvert::uint64ToJS(m_ctx, var.toULongLong());
        return QuickJSConvert::int64ToJS(m_ctx, var.toLongLong());
    }

    // constData() casts, not the QVariant accessors: the case guarantees
    // the exact stored type, and toString()/toByteArray()/... would take a
    // COW copy of the scalar on every conversion - the container
    // switch above already reads its payloads this way.
    switch (var.userType()) {
    case QMetaType::QString:
        return qStringToJS(m_ctx, *static_cast<const QString*>(var.constData()));
    case QMetaType::QChar:
        return qStringToJS(m_ctx, QString(*static_cast<const QChar*>(var.constData())));
    case QMetaType::QByteArray:
        return QuickJSConvert::byteArrayToJS(m_ctx, *static_cast<const QByteArray*>(var.constData()));
    case QMetaType::QDateTime:
        return QuickJSConvert::dateTimeToJS(m_ctx, *static_cast<const QDateTime*>(var.constData()));
    case QMetaType::QDate:
        return QuickJSConvert::dateToJS(m_ctx, *static_cast<const QDate*>(var.constData()));
    case QMetaType::QTime:
        return QuickJSConvert::timeToJS(m_ctx, *static_cast<const QTime*>(var.constData()));
    // The QVariant Qt builds for a JS null assigned to a QVariant-typed
    // property ("std::nullptr_t", V4 parity - see qobject_set_property):
    // it reads back as JS null.
    case QMetaType::Nullptr: return JS_NULL;
    default: break;
    }

    // Null-capable value types left after the specific cases above (a null
    // QString/QByteArray already converted to an EMPTY string, like V4 -
    // probed against live Qt 6.12; an invalid QDateTime/QDate/QTime became
    // an Invalid Date through its own case): V4 has no remaining null
    // semantic for them, but JS_NULL keeps the historical contract for any
    // exotic null-capable metatype instead of an undefined.
    if (var.isNull()) return JS_NULL;

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

    // Depth-0 conversion entry: record the anchor the unknown-floor descent
    // bound below is measured from (see toJSValueImpl's guard comment).
    if (depth == 0)
        m_conversionAnchorSp = currentStackPointer();

    // JS_VALUE_GET_NORM_TAG, not JS_VALUE_GET_TAG: under NaN boxing
    // (32-bit builds, enabled automatically by the vendored header) a
    // float64's raw tag is the rebased upper word and never equals
    // JS_TAG_FLOAT64 - the plain GET_TAG switch would drop every double
    // into the default case and silently return an invalid QVariant.
    // NORM_TAG normalizes exactly this case and is the identity on 64-bit.
    switch (JS_VALUE_GET_NORM_TAG(val)) {
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

        // C++ recursion guard: the toVariantImpl <-> variantFromJSObject
        // chain recurses per nesting level OUTSIDE the JS stack guard, and
        // on fat Debug/ASan builds (Windows MinGW Debug, ~22-36 KiB per
        // frame pair) a deep variant graph exhausts the real thread stack -
        // a hard crash (reproduced with ASan in JS_GetOwnPropertyNamesInternal
        // while converting a 64-level object). The depth cap above cannot
        // know the frame fatness; bail out with an invalid QVariant, exactly
        // like a cycle, once the remaining stack distance to the thread
        // floor drops below the conversion reserve.
        {
            const uintptr_t sp = currentStackPointer();
            const uintptr_t realFloor = threadStackFloor(sp);
            // Unknown floor: bound the descent from the conversion entry
            // anchor by the assumed usable window instead (no bound at
            // depth 0 - the anchor IS the current frame there).
            const uintptr_t floor = realFloor != 0
                ? realFloor
                : (m_conversionAnchorSp > kUnknownFloorAssumedUsable
                       ? m_conversionAnchorSp - kUnknownFloorAssumedUsable : 0);
            if (Q_UNLIKELY(floor != 0 && sp < floor + kVariantStackReserve))
                return QVariant();
        }

        visited.push(ptr);

        QVariant result;
        try {
            if (JS_IsDate(val)) {
                // Call the CAPTURED intrinsic Date.prototype.getTime, not the
                // getTime property the instance (or an edited prototype
                // chain) happens to carry: QJSEngine reads the internal date
                // slot, so a script that replaced d.getTime or
                // Date.prototype.getTime must not change the converted value.
                // The captured original builtin still costs one JS_Call - a
                // documented wrapper divergence (V4: slot read) - but its
                // result is fixed by construction: it reads the internal slot
                // of the receiver it is given.
                if (JS_IsFunction(m_ctx, m_intrinsics.dateGetTime)) {
                    ScopedJSValue ret(m_ctx, JS_Call(m_ctx, m_intrinsics.dateGetTime, val, 0, nullptr));
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
                    // Intrinsics not captured (exotic context): no honest
                    // internal read is available; an invalid QDateTime beats
                    // a script-controlled value.
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
    // Anchor for the unknown-floor descent bound of the container fast
    // paths below: toVariantImpl/toJSValueImpl record it at their own
    // depth-0 entries, but hashToJSObject/stringListToJSArray/mapToJSObject/
    // listToJSArray called directly from here recurse through toJSValueImpl
    // with depth >= 1, which measures against this anchor when the platform
    // exposes no thread-floor API.
    m_conversionAnchorSp = currentStackPointer();

    if (metaType == QMetaType::Int) return JS_NewInt32(m_ctx, *static_cast<int*>(data));
    if (metaType == QMetaType::Bool) return JS_NewBool(m_ctx, *static_cast<bool*>(data));
    if (metaType == QMetaType::Double) return JS_NewFloat64(m_ctx, *static_cast<double*>(data));
    if (metaType == QMetaType::Float) return JS_NewFloat64(m_ctx, *static_cast<float*>(data));
    if (metaType == QMetaType::QString) return qStringToJS(m_ctx, *static_cast<QString*>(data));
    if (metaType == QMetaType::QObjectStar) return wrapQObject(*static_cast<QObject**>(data));
    if (metaType == QMetaType::QVariant) return toJSValue(*static_cast<QVariant*>(data));
    if (metaType == QMetaType::UnknownType || metaType == QMetaType::Void) return JS_UNDEFINED;

    // Same scalar fast paths as toJSValueImpl(): metacall argument storage
    // points at a value of exactly the declared type. Every case reads the
    // exact C++ width: `long`/`unsigned long` are 4 bytes on LLP64 (Windows)
    // and 32-bit targets, and a full qint64/quint64 read there would go
    // past the end of the caller's storage.
    if (metaType == QMetaType::LongLong)
        return QuickJSConvert::int64ToJS(m_ctx, *static_cast<qint64*>(data));
    if (metaType == QMetaType::Long)
        return QuickJSConvert::int64ToJS(m_ctx, static_cast<qint64>(*static_cast<long*>(data)));
    if (metaType == QMetaType::ULongLong)
        return QuickJSConvert::uint64ToJS(m_ctx, *static_cast<quint64*>(data));
    if (metaType == QMetaType::ULong)
        return QuickJSConvert::uint64ToJS(m_ctx, static_cast<quint64>(*static_cast<unsigned long*>(data)));
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
    if (metaType == qMetaTypeId<QVariantMap>())
        return QuickJSConvert::mapToJSObject(this, m_ctx, static_cast<const QVariantMap*>(data), 0);
    if (metaType == qMetaTypeId<QVariantList>())
        return QuickJSConvert::listToJSArray(this, m_ctx, static_cast<const QVariantList*>(data), 0);
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
