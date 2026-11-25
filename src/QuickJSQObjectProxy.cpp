//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSQObjectProxy.h"
#include "QuickJSConvert.h"

#include <cmath>
#include <utility>
#include <memory>

namespace {

// ONE read primitive for both metacall slots: a property read (op =
// ReadProperty, one-slot array) and a method invocation (op =
// InvokeMetaMethod, where args[1..] hold the converted arguments and args[0]
// is the return slot). Reads into a default-constructed storage of the EXACT
// C++ type matching the metatype and converts it to a JSValue; the buffer
// width is derived from T by construction - the invariant the whole
// convertMetaRead() ladder below builds on (see the adjustEnumType comment
// in QuickJSConvert.cpp).
template<typename T, typename Convert>
JSValue readMetaSlot(JSContext* ctx, QObject* obj, QMetaObject::Call op, int slotIdx,
                     QVarLengthArray<void*, MetaCallArgsLimit>* args, Convert convert)
{
    T val{};
    void* propertySlot[1] = { &val };
    void** a = args ? args->data() : propertySlot;
    a[0] = &val;
    const bool ok = safeMetacall(ctx, obj, op, slotIdx, a);
    a[0] = nullptr;
    if (!ok) return JS_EXCEPTION;
    return convert(std::move(val));
}

} // namespace

// The single QMetaType read ladder, shared by method returns and property
// reads (the two former parallel switches - keeping them in sync by hand is
// how the Long/Char/enum width bugs crept in). Behavior differences between
// the two callers are keyed off op alone: an unsupported method return type
// throws a TypeError, an unsupported property type reads as undefined
// (V4-like, so Object.keys()/for...in survive it).
JSValue QuickJSQObjectProxy::convertMetaRead(QQuickJSEngine* engine, JSContext* ctx, QObject* obj,
                                             QMetaObject::Call op, int slotIdx, int metaType,
                                             QVarLengthArray<void*, MetaCallArgsLimit>* args)
{
    switch (metaType) {
    case QMetaType::Int:
        return readMetaSlot<int>(ctx, obj, op, slotIdx, args, [ctx](int v) { return JS_NewInt32(ctx, v); });
    case QMetaType::Bool:
        return readMetaSlot<bool>(ctx, obj, op, slotIdx, args, [ctx](bool v) { return JS_NewBool(ctx, v); });
    case QMetaType::Double:
        return readMetaSlot<double>(ctx, obj, op, slotIdx, args, [ctx](double v) { return JS_NewFloat64(ctx, v); });
    case QMetaType::Float:
        return readMetaSlot<float>(ctx, obj, op, slotIdx, args, [ctx](float v) { return JS_NewFloat64(ctx, v); });
    case QMetaType::QString:
        return readMetaSlot<QString>(ctx, obj, op, slotIdx, args, [ctx](QString v) { return qStringToJS(ctx, v); });
    case QMetaType::QObjectStar:
        return readMetaSlot<QObject*>(ctx, obj, op, slotIdx, args, [engine, ctx](QObject* v) { return engine->wrapQObject(v); });
    case QMetaType::QVariant:
        return readMetaSlot<QVariant>(ctx, obj, op, slotIdx, args, [engine](QVariant v) { return engine->toJSValue(v); });
    case QMetaType::QStringList:
        return readMetaSlot<QStringList>(ctx, obj, op, slotIdx, args, [engine, ctx](QStringList v) { return QuickJSConvert::stringListToJSArray(engine, ctx, &v); });
    case QMetaType::QVariantList:
        return readMetaSlot<QVariantList>(ctx, obj, op, slotIdx, args, [engine, ctx](QVariantList v) { return QuickJSConvert::listToJSArray(engine, ctx, &v, 0); });
    // Narrow scalars: moc writes exactly sizeof(declared type) bytes into
    // the return/property slot, so each case must read its exact C++ type. A
    // char return read through a short buffer loses the sign (-1 becomes
    // 255); a 4-byte LLP64 long read through a qint64 buffer does the same
    // and a qint64 write would overflow it.
    case QMetaType::Short:
        return readMetaSlot<short>(ctx, obj, op, slotIdx, args, [ctx](short v) { return JS_NewInt32(ctx, v); });
    case QMetaType::Char:
        return readMetaSlot<char>(ctx, obj, op, slotIdx, args, [ctx](char v) { return JS_NewInt32(ctx, v); });
    case QMetaType::SChar:
        return readMetaSlot<signed char>(ctx, obj, op, slotIdx, args, [ctx](signed char v) { return JS_NewInt32(ctx, v); });
    case QMetaType::UShort: case QMetaType::UChar:
        // Unsigned narrow types zero-extend correctly in a ushort buffer.
        return readMetaSlot<ushort>(ctx, obj, op, slotIdx, args, [ctx](ushort v) { return JS_NewInt32(ctx, v); });
    case QMetaType::UInt:
        return readMetaSlot<uint>(ctx, obj, op, slotIdx, args, [ctx](uint v) { return QuickJSConvert::uint64ToJS(ctx, v); });
    case QMetaType::Long:
        return readMetaSlot<long>(ctx, obj, op, slotIdx, args, [ctx](long v) { return QuickJSConvert::int64ToJS(ctx, v); });
    case QMetaType::LongLong:
        return readMetaSlot<qint64>(ctx, obj, op, slotIdx, args, [ctx](qint64 v) { return QuickJSConvert::int64ToJS(ctx, v); });
    case QMetaType::ULong:
        return readMetaSlot<unsigned long>(ctx, obj, op, slotIdx, args, [ctx](unsigned long v) { return QuickJSConvert::uint64ToJS(ctx, v); });
    case QMetaType::ULongLong:
        return readMetaSlot<quint64>(ctx, obj, op, slotIdx, args, [ctx](quint64 v) { return QuickJSConvert::uint64ToJS(ctx, v); });
    case QMetaType::QDateTime:
        return readMetaSlot<QDateTime>(ctx, obj, op, slotIdx, args, [ctx](QDateTime v) { return QuickJSConvert::dateTimeToJS(ctx, v); });
    case QMetaType::QDate:
        return readMetaSlot<QDate>(ctx, obj, op, slotIdx, args, [ctx](QDate v) { return QuickJSConvert::dateToJS(ctx, v); });
    case QMetaType::QTime:
        return readMetaSlot<QTime>(ctx, obj, op, slotIdx, args, [ctx](QTime v) { return QuickJSConvert::timeToJS(ctx, v); });
    case QMetaType::QByteArray:
        return readMetaSlot<QByteArray>(ctx, obj, op, slotIdx, args, [ctx](QByteArray v) { return QuickJSConvert::byteArrayToJS(ctx, v); });
    default: break;
    }

    if (metaType == qMetaTypeId<QVariantMap>()) {
        return readMetaSlot<QVariantMap>(ctx, obj, op, slotIdx, args, [engine, ctx](QVariantMap v) { return QuickJSConvert::mapToJSObject(engine, ctx, &v, 0); });
    }
    if (metaType == qMetaTypeId<QVariantHash>()) {
        return readMetaSlot<QVariantHash>(ctx, obj, op, slotIdx, args, [engine, ctx](QVariantHash v) { return QuickJSConvert::hashToJSObject(engine, ctx, &v, 0); });
    }
    {
        // int-backed enums read as int32, like the generic QVariant path;
        // other enum widths keep the generic conversion below. This guard
        // and adjustEnumType() apply the same size predicate, so the cache
        // rewriting and the read ladder can never disagree on a width.
        const QMetaType mt(metaType);
        if ((mt.flags() & QMetaType::IsEnumeration) && mt.sizeOf() == sizeof(int))
            return readMetaSlot<int>(ctx, obj, op, slotIdx, args, [ctx](int v) { return JS_NewInt32(ctx, v); });
    }

    QVariant val = makeDefaultVariant(metaType);
    void* propertySlot[1] = { val.data() };
    void** a = args ? args->data() : propertySlot;
    a[0] = val.data();
    const bool ok = safeMetacall(ctx, obj, op, slotIdx, a);
    a[0] = nullptr;
    if (!ok) return JS_EXCEPTION;

    JSValue jsVal = engine->toJSValue(val);

    if (JS_IsUndefined(jsVal) && val.isValid() && !val.isNull()) {
        if (op == QMetaObject::InvokeMetaMethod) {
            return JS_ThrowTypeError(ctx, "Cannot invoke method: return type '%s' is not supported", QMetaType(metaType).name());
        }
        // V4-like: a property of an unsupported type reads as undefined
        // instead of throwing, so Object.keys()/for...in survive it. Only a
        // JS exception deliberately thrown by the C++ code (throwError()) or
        // a C++ exception escaping the metacall propagate as errors.
        return JS_UNDEFINED;
    }

    return jsVal;
}

JSValue QuickJSQObjectProxy::invokeAndConvertReturn(QQuickJSEngine* engine, JSContext* ctx, QObject* obj, int methodIdx, int retType, QVarLengthArray<void*, MetaCallArgsLimit>& args) {
    if (retType == QMetaType::Void) {
        // No return slot to convert: invoke and report undefined.
        if (!safeMetacall(ctx, obj, QMetaObject::InvokeMetaMethod, methodIdx, args.data())) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    if (retType == QMetaType::UnknownType) {
        return JS_ThrowTypeError(ctx, "Cannot invoke method: return type is not registered");
    }
    return convertMetaRead(engine, ctx, obj, QMetaObject::InvokeMetaMethod, methodIdx, retType, &args);
}

JSValue QuickJSQObjectProxy::getMetaProperty(QQuickJSEngine* engine, JSContext* ctx, JSQObjectData* data, const MetaCacheEntry& entry) {
    if (!entry.isReadable) {
        return JS_UNDEFINED;
    }

    const int metaType = entry.type;
    if (metaType == QMetaType::UnknownType) {
        qWarning("QQuickJSEngine: Property has unregistered type, returning undefined");
        return JS_UNDEFINED;
    }

    QObject* obj = data->obj.data();

    return convertMetaRead(engine, ctx, obj, QMetaObject::ReadProperty, entry.index, metaType, nullptr);
}

JSValue QuickJSQObjectProxy::createMethodProxy(QQuickJSEngine* engine, JSContext* ctx, JSQObjectData* data, JSAtom prop, const MetaCacheEntry& entry, JSClassID classId) {
    Q_UNUSED(engine);

    ScopedJSValue proxyVal(ctx, JS_NewObjectClass(ctx, classId));
    if (JS_IsException(proxyVal.value())) return proxyVal.value();

    auto proxyData = std::make_unique<JSMethodProxyData>();
    proxyData->obj = data->obj;
    proxyData->methodIdx = entry.index;
    proxyData->retType = entry.type;
    proxyData->paramCount = entry.paramCount;
    proxyData->paramTypes = entry.paramTypes;

    QMetaMethod m = data->obj->metaObject()->method(entry.index);
    proxyData->methodName = m.name();

    // Set the opaque pointer before publishing the proxy in the cache: on the
    // (practically unreachable) failure path the cache must not hold a proxy
    // object without opaque data.
    JSMethodProxyData* rawProxyData = proxyData.release();
    if (JS_SetOpaque(proxyVal.value(), rawProxyData) != 0) {
        delete rawProxyData;
        JS_ThrowInternalError(ctx, "Failed to set method proxy opaque data");
        return JS_EXCEPTION;
    }

    JSAtom dupAtom = JS_DupAtom(ctx, prop);
    if (dupAtom == JS_ATOM_NULL) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    JSValue dupVal = JS_DupValue(ctx, proxyVal.value());
    try {
        data->methodProxyCache.append(MethodProxyEntry{.atom = dupAtom, .value = dupVal});
    } catch (...) {
        JS_FreeAtom(ctx, dupAtom);
        JS_FreeValue(ctx, dupVal);
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }

    return proxyVal.release();
}

int QuickJSQObjectProxy::qobject_get_own_property(JSContext* ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop) {
    return invokeSafe(ctx, "get_own_property", [&]() -> int {
        // Cost note: enumeration (Object.keys/for-in) probes every property
        // through here with a full descriptor - the engine's flags-only probe
        // still reads desc->value - so each listed meta-property costs one
        // metacall with its side effects. Inherent to the current engine API.
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
        auto data = static_cast<JSQObjectData*>(JS_GetOpaque(obj, engine->m_qobjectClassID));
        if (!data || data->obj.isNull()) {
            JS_ThrowTypeError(ctx, "QObject has been deleted");
            return -1;
        }

        const MetaCacheEntry* it = data->metaCache->find(prop);
        // A JS exception thrown by the C++ accessor (throwError(), or a C++
        // exception caught by safeMetacall) propagates like a method call
        // does. A property of an unsupported type reads as undefined
        // (V4-like), so Object.keys()/for...in survive it.
        if (it) {
            if (it->kind == MetaCacheEntry::Kind::Property) {
                if (desc) {
                    desc->flags = JS_PROP_ENUMERABLE;
                    if (it->isWritable) desc->flags |= JS_PROP_WRITABLE;
                    desc->getter = JS_UNDEFINED;
                    desc->setter = JS_UNDEFINED;
                    if (it->type == QMetaType::UnknownType) {
                        desc->value = JS_UNDEFINED;
                    } else {
                        desc->value = getMetaProperty(engine, ctx, data, *it);
                        if (JS_IsException(desc->value)) {
                            // Keep the pending exception: the read fails with
                            // the thrown error instead of silently producing
                            // undefined.
                            return -1;
                        }
                    }
                }
                return 1;
            } else if (it->kind == MetaCacheEntry::Kind::Method) {
                if (desc) {
                    desc->flags = 0;
                    desc->getter = JS_UNDEFINED;
                    desc->setter = JS_UNDEFINED;
                    desc->value = getMethodProxy(engine, ctx, data, prop, *it);
                    if (JS_IsException(desc->value)) return -1;
                }
                return 1;
            }
        }

        for (auto& e : data->dynamicPropertyCache) {
            if (e.atom == prop) {
                if (e.state == PropertyCacheState::Invalid) {
                    if (data->obj->dynamicPropertyNames().isEmpty()) return 0;
                    QVariant dynProp = data->obj->property(e.name.constData());
                    if (dynProp.isValid()) {
                        e.state = PropertyCacheState::Dynamic;
                        if (desc) {
                            desc->flags = JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
                            desc->getter = JS_UNDEFINED;
                            desc->setter = JS_UNDEFINED;
                            desc->value = engine->toJSValue(dynProp);
                            if (JS_IsException(desc->value)) return -1;
                        }
                        return 1;
                    }
                    return 0;
                }
                if (e.state == PropertyCacheState::Dynamic) {
                    QVariant dynProp = data->obj->property(e.name.constData());
                    if (dynProp.isValid()) {
                        if (desc) {
                            desc->flags = JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
                            desc->getter = JS_UNDEFINED;
                            desc->setter = JS_UNDEFINED;
                            desc->value = engine->toJSValue(dynProp);
                            if (JS_IsException(desc->value)) return -1;
                        }
                        return 1;
                    }
                    e.state = PropertyCacheState::Invalid;
                    return 0;
                }
                break;
            }
        }

        const QString& nameQ = QuickJSConvert::atomName(engine, ctx, prop);
        if (!nameQ.isNull()) {
            const QByteArray nameBa(nameQ.toUtf8());
            QVariant dynProp = data->obj->property(nameBa.constData());
            if (dynProp.isValid()) {
                if (desc) {
                    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
                    desc->getter = JS_UNDEFINED;
                    desc->setter = JS_UNDEFINED;
                    desc->value = engine->toJSValue(dynProp);
                    if (JS_IsException(desc->value)) return -1;
                }
                return 1;
            }

            if (data->dynamicPropertyCache.size() < DynamicPropCacheLimit) {
                JSAtom dupAtom = JS_DupAtom(ctx, prop);
                if (dupAtom == JS_ATOM_NULL) {
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                try {
                    data->dynamicPropertyCache.append(DynamicPropEntry{.atom = dupAtom, .state = PropertyCacheState::Invalid, .name = nameBa});
                } catch (...) {
                    JS_FreeAtom(ctx, dupAtom);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
            }
        }

        return 0;
    });
}

int QuickJSQObjectProxy::qobject_get_own_property_names(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen, JSValueConst obj) {
    return invokeSafe(ctx, "get_own_property_names", [&]() -> int {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
        auto data = static_cast<JSQObjectData*>(JS_GetOpaque(obj, engine->m_qobjectClassID));
        if (!data || data->obj.isNull()) {
            JS_ThrowTypeError(ctx, "QObject has been deleted");
            return -1;
        }

        const JSMetaObjectCache* metaCache = data->metaCache;
        const QList<QByteArray> dynNames = data->obj->dynamicPropertyNames();
        const int dynCount = dynNames.size();

        const int totalCount = metaCache->methodAtoms.size() + metaCache->propertyAtoms.size() + dynCount;
        if (totalCount == 0) {
            *ptab = nullptr;
            *plen = 0;
            return 0;
        }

        QSet<JSAtom> addedAtoms;
        addedAtoms.reserve(totalCount);

        *ptab = static_cast<JSPropertyEnum*>(js_malloc(ctx, sizeof(JSPropertyEnum) * totalCount));
        if (!*ptab) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        *plen = 0;

        auto addAtom = [&](JSAtom atom, bool isEnumerable) -> bool {
            if (atom == JS_ATOM_NULL) {
                for (uint32_t j = 0; j < *plen; ++j) JS_FreeAtom(ctx, (*ptab)[j].atom);
                js_free(ctx, *ptab);
                *ptab = nullptr; *plen = 0;
                JS_ThrowOutOfMemory(ctx);
                return false;
            }
            if (addedAtoms.contains(atom)) {
                JS_FreeAtom(ctx, atom);
                return true;
            }
            (*ptab)[*plen].atom = atom;
            (*ptab)[*plen].is_enumerable = isEnumerable;
            addedAtoms.insert(atom);
            (*plen)++;
            return true;
        };

        for (JSAtom atom : metaCache->methodAtoms) {
            if (!addAtom(JS_DupAtom(ctx, atom), false)) return -1;
        }

        for (JSAtom atom : metaCache->propertyAtoms) {
            if (!addAtom(JS_DupAtom(ctx, atom), true)) return -1;
        }

        for (const QByteArray& name : dynNames) {
            if (!addAtom(JS_NewAtom(ctx, name.constData()), true)) return -1;
        }

        return 0;
    });
}

int QuickJSQObjectProxy::qobject_set_property(JSContext* ctx, JSValueConst this_obj, JSAtom prop, JSValueConst val, JSValueConst receiver, int flags) {
    Q_UNUSED(receiver);
    Q_UNUSED(flags);
    return invokeSafe(ctx, "set_property", [&]() -> int {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
        auto data = static_cast<JSQObjectData*>(JS_GetOpaque(this_obj, engine->m_qobjectClassID));
        if (!data || data->obj.isNull()) {
            JS_ThrowTypeError(ctx, "QObject has been deleted");
            return -1;
        }

        QObject* obj = data->obj.data();
        const MetaCacheEntry* it = data->metaCache->find(prop);
        if (it && it->kind == MetaCacheEntry::Kind::Property) {
            const auto& entry = *it;

            if (!entry.isWritable) {
                const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                JS_ThrowTypeError(ctx, "Cannot assign to read only property '%s'", name.constData());
                return -1;
            }

            const int metaType = entry.type;
            if (metaType == QMetaType::UnknownType) {
                const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                JS_ThrowTypeError(ctx, "Cannot assign to property '%s' of unknown type", name.constData());
                return -1;
            }

            if (metaType == QMetaType::Int) {
                int v = 0;
                if (JS_ToInt32(ctx, &v, val) == 0) {
                    void* a[1] = { &v };
                    if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                    return 1;
                }
                clearPendingException(ctx);
            } else if (metaType == QMetaType::Bool) {
                int b = JS_ToBool(ctx, val);
                if (b >= 0) {
                    bool v = (b != 0); void* a[1] = { &v };
                    if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                    return 1;
                }
                clearPendingException(ctx);
            } else if (metaType == QMetaType::Double) {
                double v = 0;
                if (JS_ToFloat64(ctx, &v, val) == 0) {
                    void* a[1] = { &v };
                    if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                    return 1;
                }
                clearPendingException(ctx);
            } else if (metaType == QMetaType::QString) {
                ScopedJSValue strVal(ctx, JS_ToString(ctx, val));
                if (JS_IsException(strVal.value())) return -1;

                QString v = QuickJSConvert::jsToQString(ctx, strVal.value());
                void* a[1] = { &v };
                if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                return 1;
            } else if (metaType == QMetaType::QObjectStar) {
                QVariant var = engine->toVariant(val);
                if (!var.isValid() && !JS_IsUndefined(val) && !JS_IsNull(val)) {
                    // Unrepresentable value (Symbol, Map, RegExp, ...): refuse
                    // instead of silently writing nullptr, symmetric to the
                    // generic path below.
                    const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                    JS_ThrowTypeError(ctx, "Cannot assign value to property '%s' (incompatible type)", name.constData());
                    return -1;
                }
                QObject* v = var.value<QObject*>();
                void* a[1] = { &v };
                if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                return 1;
            } else if (metaType == QMetaType::Float) {
                // Exact numeric tags only: other shapes keep the generic
                // toVariant()+convert() route below (its coercion for
                // strings/objects is not a plain cast).
                const int tag = JS_VALUE_GET_TAG(val);
                if (tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag)) {
                    const double num = (tag == JS_TAG_INT)
                                           ? static_cast<double>(JS_VALUE_GET_INT(val))
                                           : JS_VALUE_GET_FLOAT64(val);
                    float v = static_cast<float>(num);
                    void* a[1] = { &v };
                    if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                    return 1;
                }
            } else if (metaType == QMetaType::LongLong) {
                // Integral int/float64 tags only: QVariant::convert rounds
                // fractional doubles (3.5 -> 4), so those keep the generic path.
                const int tag = JS_VALUE_GET_TAG(val);
                qint64 v = 0;
                bool exact = false;
                if (tag == JS_TAG_INT) {
                    v = JS_VALUE_GET_INT(val);
                    exact = true;
                } else if (JS_TAG_IS_FLOAT64(tag)) {
                    const double d = JS_VALUE_GET_FLOAT64(val);
                    if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0 && d == std::trunc(d)) {
                        v = static_cast<qint64>(d);
                        exact = true;
                    }
                }
                if (exact) {
                    void* a[1] = { &v };
                    if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                    return 1;
                }
            } else if (metaType == QMetaType::UInt) {
                const int tag = JS_VALUE_GET_TAG(val);
                quint32 v = 0;
                bool exact = false;
                if (tag == JS_TAG_INT && JS_VALUE_GET_INT(val) >= 0) {
                    v = static_cast<quint32>(JS_VALUE_GET_INT(val));
                    exact = true;
                } else if (JS_TAG_IS_FLOAT64(tag)) {
                    const double d = JS_VALUE_GET_FLOAT64(val);
                    if (std::isfinite(d) && d >= 0.0 && d <= 4294967295.0 && d == std::trunc(d)) {
                        v = static_cast<quint32>(d);
                        exact = true;
                    }
                }
                if (exact) {
                    void* a[1] = { &v };
                    if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
                    return 1;
                }
            }

            QVariant convertedVar = engine->toVariant(val);
            if (!convertedVar.isValid()) {
                if (JS_IsUndefined(val) || JS_IsNull(val)) {
                    // Legacy semantics: undefined/null reset or write the default.
                    if (entry.isResettable) {
                        void* a[1] = { nullptr };
                        if (!safeMetacall(ctx, obj, QMetaObject::ResetProperty, entry.index, a)) return -1;
                        return 1;
                    }
                    convertedVar = makeDefaultVariant(metaType);
                } else {
                    // Any other unrepresentable value (Symbol, RegExp, Promise,
                    // ...) must not silently write the default.
                    const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                    JS_ThrowTypeError(ctx, "Cannot assign value to property '%s' (incompatible type)", name.constData());
                    return -1;
                }
            } else if (metaType != QMetaType::QVariant && convertedVar.userType() != metaType) {
                const QMetaType mt(metaType);
                if (!mt.isValid() || !convertedVar.canConvert(mt) || !convertedVar.convert(mt)) {
                    const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                    JS_ThrowTypeError(ctx, "Cannot assign value to property '%s' (incompatible type)", name.constData());
                    return -1;
                }
            }

            void* a[1] = { (metaType == QMetaType::QVariant) ? static_cast<void*>(&convertedVar) : convertedVar.data() };
            if (!safeMetacall(ctx, obj, QMetaObject::WriteProperty, entry.index, a)) return -1;
            return 1;
        } else if (it && it->kind == MetaCacheEntry::Kind::Method) {
            const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
            JS_ThrowTypeError(ctx, "Cannot assign to read only property '%s'", name.constData());
            return -1;
        }

        QVariant var = engine->toVariant(val);

        QByteArray dynName;
        bool foundInCache = false;

        for (auto& e : data->dynamicPropertyCache) {
            if (e.atom == prop) {
                if (e.state == PropertyCacheState::Invalid) e.state = PropertyCacheState::Dynamic;
                dynName = e.name;
                foundInCache = true;
                break;
            }
        }

        if (!foundInCache) {
            const QString& nameQ = QuickJSConvert::atomName(engine, ctx, prop);
            if (nameQ.isNull()) {
                JS_ThrowInternalError(ctx, "Failed to allocate memory for property name");
                return -1;
            }
            dynName = QByteArray(nameQ.toUtf8());

            if (data->dynamicPropertyCache.size() < DynamicPropCacheLimit) {
                JSAtom dupAtom = JS_DupAtom(ctx, prop);
                if (dupAtom == JS_ATOM_NULL) {
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                try {
                    data->dynamicPropertyCache.append(DynamicPropEntry{.atom = dupAtom, .state = PropertyCacheState::Dynamic, .name = dynName});
                } catch (...) {
                    JS_FreeAtom(ctx, dupAtom);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
            }
        }

        obj->setProperty(dynName.constData(), var);
        return 1;
    });
}

JSValue QuickJSQObjectProxy::js_method_invoke(JSContext* ctx, JSValueConst func_obj, JSValueConst this_val, int argc, JSValueConst* argv, int flags) {
    Q_UNUSED(this_val);
    Q_UNUSED(flags);
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));

    return invokeSafe(ctx, "method invoke", [&]() -> JSValue {
        // One probe covers both proxy flavors: method and signal proxies
        // share the JSMethodProxyData layout; the class-id check rejects any
        // other class (e.g. QObject wrappers).
        JSClassID proxyClassId = 0;
        auto data = static_cast<JSMethodProxyData*>(JS_GetAnyOpaque(func_obj, &proxyClassId));
        if (!data || (proxyClassId != engine->m_methodClassID && proxyClassId != engine->m_signalClassID)
                || data->obj.isNull()) {
            return JS_ThrowTypeError(ctx, "C++ object has been deleted");
        }

        QObject* obj = data->obj.data();
        const int paramCount = data->paramCount;

        QVarLengthArray<void*, MetaCallArgsLimit> args(paramCount + 1);
        args[0] = nullptr;

        struct NativeArgHolder {
            int i; bool b; double d; float f; qint64 i64; quint32 u32; QString str; QObject* obj;
        };
        QVarLengthArray<NativeArgHolder, 10> nativeArgs(paramCount);
        QVarLengthArray<QVariant, 10> varArgs(paramCount);

        for (int i = 0; i < paramCount; ++i) {
            const int expectedType = data->paramTypes[i];
            bool converted = false;

            if (i < argc && expectedType != QMetaType::UnknownType && expectedType != QMetaType::QVariant) {
                if (expectedType == QMetaType::Int) {
                    if (JS_ToInt32(ctx, &nativeArgs[i].i, argv[i]) == 0) { args[i+1] = &nativeArgs[i].i; converted = true; }
                    else clearPendingException(ctx);
                } else if (expectedType == QMetaType::Bool) {
                    int b = JS_ToBool(ctx, argv[i]);
                    if (b >= 0) { nativeArgs[i].b = (b != 0); args[i+1] = &nativeArgs[i].b; converted = true; }
                    else clearPendingException(ctx);
                } else if (expectedType == QMetaType::Double) {
                    if (JS_ToFloat64(ctx, &nativeArgs[i].d, argv[i]) == 0) { args[i+1] = &nativeArgs[i].d; converted = true; }
                    else clearPendingException(ctx);
                } else if (expectedType == QMetaType::Float) {
                    // Exact numeric tags only: other shapes (strings, objects,
                    // BigInt) keep the generic toVariant()+convert() route
                    // below, whose coercion is not a plain cast.
                    const int tag = JS_VALUE_GET_TAG(argv[i]);
                    if (tag == JS_TAG_INT) {
                        nativeArgs[i].f = static_cast<float>(JS_VALUE_GET_INT(argv[i]));
                        args[i+1] = &nativeArgs[i].f; converted = true;
                    } else if (JS_TAG_IS_FLOAT64(tag)) {
                        nativeArgs[i].f = static_cast<float>(JS_VALUE_GET_FLOAT64(argv[i]));
                        args[i+1] = &nativeArgs[i].f; converted = true;
                    }
                } else if (expectedType == QMetaType::LongLong) {
                    // Integral int/float64 tags only: QVariant::convert
                    // rounds fractional doubles, so those keep the generic path.
                    const int tag = JS_VALUE_GET_TAG(argv[i]);
                    if (tag == JS_TAG_INT) {
                        nativeArgs[i].i64 = JS_VALUE_GET_INT(argv[i]);
                        args[i+1] = &nativeArgs[i].i64; converted = true;
                    } else if (JS_TAG_IS_FLOAT64(tag)) {
                        const double d = JS_VALUE_GET_FLOAT64(argv[i]);
                        if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0 && d == std::trunc(d)) {
                            nativeArgs[i].i64 = static_cast<qint64>(d);
                            args[i+1] = &nativeArgs[i].i64; converted = true;
                        }
                    }
                } else if (expectedType == QMetaType::UInt) {
                    const int tag = JS_VALUE_GET_TAG(argv[i]);
                    if (tag == JS_TAG_INT && JS_VALUE_GET_INT(argv[i]) >= 0) {
                        nativeArgs[i].u32 = static_cast<quint32>(JS_VALUE_GET_INT(argv[i]));
                        args[i+1] = &nativeArgs[i].u32; converted = true;
                    } else if (JS_TAG_IS_FLOAT64(tag)) {
                        const double d = JS_VALUE_GET_FLOAT64(argv[i]);
                        if (std::isfinite(d) && d >= 0.0 && d <= 4294967295.0 && d == std::trunc(d)) {
                            nativeArgs[i].u32 = static_cast<quint32>(d);
                            args[i+1] = &nativeArgs[i].u32; converted = true;
                        }
                    }
                } else if (expectedType == QMetaType::QString) {
                    ScopedJSValue strVal(ctx, JS_ToString(ctx, argv[i]));
                    if (JS_IsException(strVal.value())) {
                        return JS_EXCEPTION;
                    }
                    nativeArgs[i].str = QuickJSConvert::jsToQString(ctx, strVal.value());
                    args[i+1] = &nativeArgs[i].str;
                    converted = true;
                } else if (expectedType == QMetaType::QObjectStar) {
                    QVariant v = engine->toVariant(argv[i]);
                    if (v.canConvert<QObject*>()) { nativeArgs[i].obj = v.value<QObject*>(); args[i+1] = &nativeArgs[i].obj; converted = true; }
                }
                else if (expectedType == qMetaTypeId<QQuickJSValue>()) {
                    varArgs[i] = QVariant::fromValue(QQuickJSValue(engine, JS_DupValue(ctx, argv[i]), true));
                    args[i+1] = varArgs[i].data();
                    converted = true;
                }
            }

            if (!converted) {
                if (expectedType == QMetaType::UnknownType) {
                    return JS_ThrowTypeError(ctx, "Argument %d for method '%s' has unregistered type", i + 1, data->methodName.constData());
                }

                if (expectedType == QMetaType::QVariant) {
                    varArgs[i] = (i < argc) ? engine->toVariant(argv[i]) : QVariant();
                } else {
                    varArgs[i] = makeDefaultVariant(expectedType);
                    if (i < argc) {
                        QVariant convertedVar = engine->toVariant(argv[i]);
                        if (convertedVar.userType() != expectedType) {
                            const QMetaType mt(expectedType);
                            if (mt.isValid() && convertedVar.canConvert(mt) && convertedVar.convert(mt)) {
                                varArgs[i] = convertedVar;
                            } else {
                                return JS_ThrowTypeError(ctx, "Argument %d for method '%s' has incompatible type", i + 1, data->methodName.constData());
                            }
                        } else {
                            varArgs[i] = convertedVar;
                        }
                    }
                }
                args[i + 1] = (expectedType == QMetaType::QVariant)
                                  ? static_cast<void*>(&varArgs[i])
                                  : varArgs[i].data();
            }
        }

        return invokeAndConvertReturn(engine, ctx, obj, data->methodIdx, data->retType, args);
    });
}

JSValue QuickJSQObjectProxy::getMethodProxy(QQuickJSEngine* engine, JSContext* ctx, JSQObjectData* data, JSAtom prop, const MetaCacheEntry& entry) {
    for (const auto& e : data->methodProxyCache) {
        if (e.atom == prop) return JS_DupValue(ctx, e.value);
    }

    const JSClassID classId = (entry.methodType == QMetaMethod::Signal)
                                  ? engine->m_signalClassID
                                  : engine->m_methodClassID;
    return createMethodProxy(engine, ctx, data, prop, entry, classId);
}

int QuickJSQObjectProxy::qobject_delete_property(JSContext* ctx, JSValueConst obj, JSAtom prop) {
    return invokeSafe(ctx, "delete_property", [&]() -> int {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
        auto data = static_cast<JSQObjectData*>(JS_GetOpaque(obj, engine->m_qobjectClassID));
        if (!data || data->obj.isNull()) {
            JS_ThrowTypeError(ctx, "QObject has been deleted");
            return -1;
        }

        if (data->metaCache->find(prop)) {
            return 0;
        }

        for (auto& e : data->dynamicPropertyCache) {
            if (e.atom == prop) {
                if (e.state == PropertyCacheState::Dynamic) {
                    data->obj->setProperty(e.name.constData(), QVariant());
                    e.state = PropertyCacheState::Invalid;
                    return 1;
                }
                return 0;
            }
        }

        const QString& nameQ = QuickJSConvert::atomName(engine, ctx, prop);
        if (!nameQ.isNull()) {
            const QByteArray nameBa(nameQ.toUtf8());
            QVariant dynProp = data->obj->property(nameBa.constData());
            if (dynProp.isValid()) {
                data->obj->setProperty(nameBa.constData(), QVariant());
                return 1;
            }
        }

        return 0;
    });
}
