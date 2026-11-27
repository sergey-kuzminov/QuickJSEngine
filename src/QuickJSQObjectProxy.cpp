//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSQObjectProxy.h"
#include "QuickJSConvert.h"

#include <cmath>
#include <limits>
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

// Narrows a Date-origin variant (a local-spec QDateTime, invalid for a NaN
// Date) to a date/time target exactly like V4's hinted toVariant
// (qv4engine.cpp) combined with DateObject::dateTimeToDate
// (qv4dateobject.cpp): QDate = the local date with the QTBUG-92466
// exception (an instant that is exactly the UTC midnight of a day whose
// local wall time falls on the previous day - a negative-offset zone -
// yields the UTC date, so date-only strings and QDate-anchored Dates
// round-trip); QTime = the local time-of-day; QDateTime = the local
// datetime. An invalid input yields the invalid target value, silently.
static QVariant dateTargetFromVariant(const QVariant& dtVar, int targetType)
{
    const QDateTime dt = dtVar.isValid() ? dtVar.toDateTime() : QDateTime();
    if (targetType == QMetaType::QDate)
        return QVariant::fromValue(QuickJSConvert::dateTimeToDate(dt));
    if (targetType == QMetaType::QTime)
        return QVariant::fromValue(dt.time());
    return QVariant::fromValue(dt);
}

} // namespace

// A wrapper's cached metaobject can go stale when the QObject's dynamic
// metaobject changes at runtime (QML component refinement, dynamic
// property systems). The exotic handlers re-check the identity (one
// pointer compare) and rebind the wrapper when it changed: method proxies
// are dropped (their atoms and cache entries belong to the old meta
// cache's name set), the dynamic-property cache (name-keyed) and the
// destroyed hook survive. Returns false with a pending exception on a
// failed rebuild. A class member (not a free helper) for the engine's
// private access, like the other proxy helpers.
bool QuickJSQObjectProxy::refreshMetaCacheIfChanged(QQuickJSEngine* engine, JSQObjectData* data) {
    const QMetaObject* current = data->obj->metaObject();
    if (current == data->metaObj) return true;
    JSMetaObjectCache* cache = engine->metaObjectCacheFor(current);
    if (!cache) return false;
    for (auto& e : data->methodProxyCache) {
        JS_FreeAtom(engine->m_ctx, e.atom);
        JS_FreeValue(engine->m_ctx, e.value);
    }
    data->methodProxyCache.clear();
    data->metaObj = current;
    data->metaCache = cache;
    return true;
}

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
    if (Q_UNLIKELY(!val.isValid())) {
        // A metatype whose QVariant payload cannot be default-constructed
        // (legacy registrations without a public default ctor): the data
        // slot would be a nullptr passed straight into the metacall.
        if (op == QMetaObject::InvokeMetaMethod) {
            return JS_ThrowTypeError(ctx, "Cannot invoke method: return type '%s' cannot be default-constructed", QMetaType(metaType).name());
        }
        qWarning("QQuickJSEngine: Property type '%s' cannot be default-constructed, returning undefined", QMetaType(metaType).name());
        return JS_UNDEFINED;
    }
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

    // Collect every same-name overload in metaobject order (the cache entry
    // remembers only the first; the invoke path selects among all of them
    // like V4). moc lists the full signature before the default-argument
    // truncated synthetic entries it generates.
    const QMetaObject* meta = data->obj->metaObject();
    QMetaMethod m = meta->method(entry.index);
    proxyData->methodName = m.name();
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod mm = meta->method(i);
        if (mm.access() == QMetaMethod::Private) continue;
        if (mm.name() != proxyData->methodName) continue;
        const int pc = mm.parameterCount();
        if (pc > 255) {
            // Same ceiling as the meta-cache build: paramCount is a uint8_t
            // there; the candidate list keeps the same policy.
            qWarning("QQuickJSEngine: Method '%s' has more than 255 parameters (%d), skipping the overload",
                     proxyData->methodName.constData(), pc);
            continue;
        }
        JSMethodCandidate c;
        c.methodIdx = i;
        c.retType = mm.returnType();
        if (!QMetaType(c.retType).isRegistered()) c.retType = QMetaType::UnknownType;
        c.paramCount = pc;
        c.paramTypes.resize(pc);
        for (int j = 0; j < pc; ++j) {
            int paramType = mm.parameterType(j);
            if (!QMetaType(paramType).isRegistered()) paramType = QMetaType::UnknownType;
            c.paramTypes[j] = QuickJSConvert::adjustEnumType(paramType);
        }
        proxyData->candidates.append(std::move(c));
    }

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

        // A stale cached metaobject (dynamic metaobject swap at runtime)
        // rebinds before the lookup; the comment below the lookup documents
        // the error semantics of what a found entry returns.
        if (!refreshMetaCacheIfChanged(engine, data)) return -1;
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

        const QString nameQ = QuickJSConvert::atomName(engine, ctx, prop);
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

        if (!refreshMetaCacheIfChanged(engine, data)) return -1;
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

        // Liveness for every write below: the value conversions can run user
        // JS (valueOf, toString, getters) and that JS may delete the target
        // QObject; a local raw pointer would dangle (H-3). data->obj is a
        // QPointer and reflects the deletion immediately.
        auto writeProp = [&](int idx, void** a) -> bool {
            if (Q_UNLIKELY(data->obj.isNull())) {
                JS_ThrowTypeError(ctx, "QObject has been deleted");
                return false;
            }
            return safeMetacall(ctx, data->obj.data(), QMetaObject::WriteProperty, idx, a);
        };

        if (!refreshMetaCacheIfChanged(engine, data)) return -1;
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

            // V4 parity for JS null/undefined (probed against live Qt
            // 6.12): a meta-property write coerces them through a strict
            // table, NOT through the valueOf/ToNumber machinery below.
            // undefined: only a QVariant-typed property accepts it (V4
            // stores an invalid QVariant); everything else throws
            // "Cannot assign [undefined] to <type>". null: the nullable and
            // JS-coercible types take their null value (a null QString, an
            // empty list, false/0 - the setter IS invoked, V4 does not
            // reset to the declaration default); the remaining types throw
            // "Cannot assign std::nullptr_t to <type>".
            if (JS_IsUndefined(val)) {
                if (metaType != QMetaType::QVariant) {
                    JS_ThrowTypeError(ctx, "Cannot assign [undefined] to %s", QMetaType(metaType).name());
                    return -1;
                }
                QVariant v;
                void* a[1] = { &v };
                if (!writeProp(entry.index, a)) return -1;
                return 1;
            }
            if (JS_IsNull(val)) {
                // Typed slots so the metacall reads the exact C++ width.
                switch (metaType) {
                case QMetaType::QString: {
                    QString v;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::QByteArray: {
                    QByteArray v;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::Bool: {
                    bool v = false;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::Double: {
                    double v = 0.0;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::Float: {
                    float v = 0.0f;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::QStringList: {
                    QStringList v;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::QVariant: {
                    QVariant v = QVariant::fromValue(nullptr);
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                case QMetaType::QObjectStar: {
                    QObject* v = nullptr;
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                default:
                    JS_ThrowTypeError(ctx, "Cannot assign std::nullptr_t to %s", QMetaType(metaType).name());
                    return -1;
                }
            }

            if (metaType == QMetaType::Int) {
                int v = 0;
                if (JS_ToInt32(ctx, &v, val) == 0) {
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                // V4 parity (probed against live Qt 6.12): a failed numeric
                // coercion is always a JS exception from user code (valueOf/
                // toString throwing, Symbol, BigInt) - the double/float set
                // path propagates it, so does the argument path. Propagate
                // the pending exception instead of falling through to the
                // generic QVariant route, which would replace the user's
                // error with a generic "incompatible type" TypeError. A
                // failure without a pending exception is not produced by the
                // vendor; keep the old fallback for it defensively.
                if (JS_HasException(ctx)) return -1;
                clearPendingException(ctx);
            } else if (metaType == QMetaType::Bool) {
                int b = JS_ToBool(ctx, val);
                if (b >= 0) {
                    bool v = (b != 0); void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                clearPendingException(ctx);
            } else if (metaType == QMetaType::Double) {
                double v = 0;
                if (JS_ToFloat64(ctx, &v, val) == 0) {
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
                // Same as the Int branch above: V4's double/float set
                // path propagates a valueOf exception; do not swallow it.
                if (JS_HasException(ctx)) return -1;
                clearPendingException(ctx);
            } else if (metaType == QMetaType::QString) {
                ScopedJSValue strVal(ctx, JS_ToString(ctx, val));
                if (JS_IsException(strVal.value())) return -1;

                QString v = QuickJSConvert::jsToQString(ctx, strVal.value());
                void* a[1] = { &v };
                if (!writeProp(entry.index, a)) return -1;
                return 1;
            } else if (metaType == QMetaType::QObjectStar) {
                QVariant var = engine->toVariant(val);
                // V4 parity (probed against live Qt 6.12): only a QObject
                // wrapper (or null/undefined, handled above) assigns to a
                // QObject* property; any other value - a plain object, a
                // number, a string - throws instead of silently writing
                // nullptr.
                if (!var.isValid() || !var.canConvert<QObject*>()) {
                    const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                    JS_ThrowTypeError(ctx, "Cannot assign value to property '%s' (incompatible type)", name.constData());
                    return -1;
                }
                QObject* v = var.value<QObject*>();
                void* a[1] = { &v };
                if (!writeProp(entry.index, a)) return -1;
                return 1;
            } else if (metaType == QMetaType::Float) {
                // Exact numeric tags: other shapes keep the generic
                // toVariant()+convert() route below (its coercion for
                // strings/BigInt is not a plain cast). Objects are the
                // exception (probed against live Qt 6.12): V4 coerces
                // them through valueOf - a returning valueOf assigns, a
                // throwing one propagates, a valueOf-less object (and a
                // QObject wrapper) yields NaN - while the generic QVariant
                // route would report "incompatible type" and lose the
                // user's exception.
                const int tag = JS_VALUE_GET_TAG(val);
                if (tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag)) {
                    const double num = (tag == JS_TAG_INT)
                                           ? static_cast<double>(JS_VALUE_GET_INT(val))
                                           : JS_VALUE_GET_FLOAT64(val);
                    float v = static_cast<float>(num);
                    void* a[1] = { &v };
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                } else if (tag == JS_TAG_OBJECT) {
                    double d = 0;
                    if (JS_ToFloat64(ctx, &d, val) == 0) {
                        float v = static_cast<float>(d);
                        void* a[1] = { &v };
                        if (!writeProp(entry.index, a)) return -1;
                        return 1;
                    }
                    if (JS_HasException(ctx)) return -1;
                    clearPendingException(ctx);
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
                    if (!writeProp(entry.index, a)) return -1;
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
                    if (!writeProp(entry.index, a)) return -1;
                    return 1;
                }
            }

            QVariant convertedVar;
            if ((metaType == QMetaType::QDate || metaType == QMetaType::QTime
                 || metaType == QMetaType::QDateTime)
                && JS_IsDate(val)) {
                // V4 parity (probed in America/New_York and Europe/Moscow;
                // qv4engine.cpp toVariant passes the property type as the
                // hint): a Date narrows to a date/time property through
                // dateTargetFromVariant() - the local date/time with the
                // QTBUG-92466 exception for QDate. An invalid (NaN) Date
                // writes the invalid target value silently, like V4,
                // instead of throwing.
                convertedVar = dateTargetFromVariant(engine->toVariant(val), metaType);
            } else {
                convertedVar = engine->toVariant(val);
            }
            if (!convertedVar.isValid()) {
                // Any unrepresentable value (Symbol, RegExp, Promise,
                // ...) must not silently write the default; null and
                // undefined never reach here (the strict V4 table above
                // handled them).
                const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                JS_ThrowTypeError(ctx, "Cannot assign value to property '%s' (incompatible type)", name.constData());
                return -1;
            } else if (metaType != QMetaType::QVariant && convertedVar.userType() != metaType) {
                const QMetaType mt(metaType);
                if (!mt.isValid() || !convertedVar.canConvert(mt) || !convertedVar.convert(mt)) {
                    const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
                    JS_ThrowTypeError(ctx, "Cannot assign value to property '%s' (incompatible type)", name.constData());
                    return -1;
                }
            }

            void* a[1] = { (metaType == QMetaType::QVariant) ? static_cast<void*>(&convertedVar) : convertedVar.data() };
            if (!writeProp(entry.index, a)) return -1;
            return 1;
        } else if (it && it->kind == MetaCacheEntry::Kind::Method) {
            const QByteArray name = QuickJSConvert::atomName(engine, ctx, prop).toUtf8();
            JS_ThrowTypeError(ctx, "Cannot assign to read only property '%s'", name.constData());
            return -1;
        }

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
            const QString nameQ = QuickJSConvert::atomName(engine, ctx, prop);
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

        // Dynamic-property semantics (a wrapper extension: QJSEngine keeps
        // unknown writes on the JS wrapper itself, this wrapper mirrors them
        // into C++ dynamic properties instead - see README). undefined
        // removes the property (an invalid QVariant is QObject's remove
        // signal, the counterpart of a JS property deletion); null stores
        // the null marker QVariant (std::nullptr_t, which reads back as JS
        // null); any other value that cannot be represented as a QVariant
        // (Symbol, RegExp, Promise, ...) throws instead of silently
        // DELETING the property through the invalid-QVariant remove path.
        QVariant var = engine->toVariant(val);
        if (JS_IsNull(val)) {
            var = QVariant::fromValue(nullptr);
        } else if (JS_IsUndefined(val)) {
            var = QVariant();
        } else if (!var.isValid()) {
            JS_ThrowTypeError(ctx, "Cannot assign value to dynamic property '%s' (incompatible type)", dynName.constData());
            return -1;
        }

        // Liveness re-check (H-3): the toVariant() conversion above may have
        // run user JS that deleted the target QObject.
        if (Q_UNLIKELY(data->obj.isNull())) {
            JS_ThrowTypeError(ctx, "QObject has been deleted");
            return -1;
        }
        data->obj.data()->setProperty(dynName.constData(), var);
        return 1;
    });
}

namespace {
// Conversion-cost heuristic for overload selection among same-argc
// candidates (V4 picks by argument conversion cost; pinned against live
// Qt 6.12: a JS number or bool prefers a numeric parameter over QString,
// a JS string prefers QString). Lower is better; ties keep the first
// declared candidate. The cost never decides convertibility - the actual
// conversion below still throws for incompatible values.
int argConversionCost(int paramType, JSValueConst v)
{
    const int tag = JS_VALUE_GET_TAG(v);
    const bool isNumber = (tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag));
    const bool isString = (tag == JS_TAG_STRING);
    const bool isBool = (tag == JS_TAG_BOOL);

    switch (paramType) {
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::Double:
    case QMetaType::Float: case QMetaType::Long: case QMetaType::ULong:
    case QMetaType::LongLong: case QMetaType::ULongLong:
    case QMetaType::Short: case QMetaType::UShort: case QMetaType::Char:
    case QMetaType::SChar: case QMetaType::UChar:
        if (isNumber) return 0;
        if (isBool) return 1;
        return 2;
    case QMetaType::QString:
        if (isString) return 0;
        if (isNumber || isBool) return 1;
        return 2;
    case QMetaType::QByteArray:
        return isString ? 1 : 2;
    case QMetaType::Bool:
        if (isBool) return 0;
        if (isNumber) return 1;
        return 2;
    case QMetaType::QObjectStar:
        return (tag == JS_TAG_OBJECT || JS_IsNull(v) || JS_IsUndefined(v)) ? 0 : 2;
    case QMetaType::QVariant:
        return 1;
    default:
        break;
    }
    if (paramType == qMetaTypeId<QQuickJSValue>()) return 0;
    return 2;  // anything else: the generic conversion decides later
}
} // namespace

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

        // V4 parity (probed against live Qt 6.12): select among ALL
        // same-name overloads. An exact parameter-count match wins (ties
        // broken by argument conversion cost, then by declaration order);
        // with no exact match the candidate with the largest parameter
        // count below argc runs and the extra arguments are ignored; when
        // every candidate needs more arguments than provided, the call
        // fails with "Insufficient arguments" instead of silently filling
        // value-initialized defaults. moc lists the full signature before
        // the default-argument-truncated entries it generates, so an exact
        // match exists whenever the C++ declaration has defaults.
        const JSMethodCandidate* chosen = nullptr;
        {
            int bestCost = std::numeric_limits<int>::max();
            for (const JSMethodCandidate& c : data->candidates) {
                if (c.paramCount != argc) continue;
                int cost = 0;
                for (int i = 0; i < c.paramCount; ++i)
                    cost += argConversionCost(c.paramTypes[i], argv[i]);
                if (cost < bestCost) { bestCost = cost; chosen = &c; }
            }
            if (!chosen) {
                int bestPc = -1;
                for (const JSMethodCandidate& c : data->candidates) {
                    if (c.paramCount < argc && c.paramCount > bestPc) { bestPc = c.paramCount; chosen = &c; }
                }
            }
        }
        if (!chosen) {
            return JS_ThrowPlainError(ctx, "Insufficient arguments");
        }

        const int paramCount = chosen->paramCount;

        QVarLengthArray<void*, MetaCallArgsLimit> args(paramCount + 1);
        args[0] = nullptr;

        struct NativeArgHolder {
            int i; bool b; double d; float f; qint64 i64; quint32 u32; QString str; QByteArray ba; QObject* obj;
        };
        QVarLengthArray<NativeArgHolder, 10> nativeArgs(paramCount);
        QVarLengthArray<QVariant, 10> varArgs(paramCount);

        for (int i = 0; i < paramCount; ++i) {
            const int expectedType = chosen->paramTypes[i];
            bool converted = false;

            if (expectedType != QMetaType::UnknownType && expectedType != QMetaType::QVariant) {
                if (expectedType == QMetaType::Int) {
                    if (JS_ToInt32(ctx, &nativeArgs[i].i, argv[i]) == 0) { args[i+1] = &nativeArgs[i].i; converted = true; }
                    // V4 parity (probed against live Qt 6.12): the argument
                    // path propagates the ORIGINAL exception thrown by
                    // valueOf/toString instead of replacing it with a generic
                    // "incompatible type" TypeError.
                    else if (JS_HasException(ctx)) return JS_EXCEPTION;
                } else if (expectedType == QMetaType::Bool) {
                    int b = JS_ToBool(ctx, argv[i]);
                    if (b >= 0) { nativeArgs[i].b = (b != 0); args[i+1] = &nativeArgs[i].b; converted = true; }
                    else if (JS_HasException(ctx)) return JS_EXCEPTION;
                } else if (expectedType == QMetaType::Double) {
                    if (JS_ToFloat64(ctx, &nativeArgs[i].d, argv[i]) == 0) { args[i+1] = &nativeArgs[i].d; converted = true; }
                    else if (JS_HasException(ctx)) return JS_EXCEPTION;
                } else if (expectedType == QMetaType::Float) {
                    // Exact numeric tags: strings/BigInt keep the generic
                    // toVariant()+convert() route below, whose coercion is
                    // not a plain cast. Objects coerce through valueOf
                    // (probed against live Qt 6.12: a returning valueOf
                    // assigns, a throwing one propagates, a valueOf-less
                    // object or a QObject wrapper yields NaN).
                    const int tag = JS_VALUE_GET_TAG(argv[i]);
                    if (tag == JS_TAG_INT) {
                        nativeArgs[i].f = static_cast<float>(JS_VALUE_GET_INT(argv[i]));
                        args[i+1] = &nativeArgs[i].f; converted = true;
                    } else if (JS_TAG_IS_FLOAT64(tag)) {
                        nativeArgs[i].f = static_cast<float>(JS_VALUE_GET_FLOAT64(argv[i]));
                        args[i+1] = &nativeArgs[i].f; converted = true;
                    } else if (tag == JS_TAG_OBJECT) {
                        double d = 0;
                        if (JS_ToFloat64(ctx, &d, argv[i]) == 0) {
                            nativeArgs[i].f = static_cast<float>(d);
                            args[i+1] = &nativeArgs[i].f; converted = true;
                        } else if (JS_HasException(ctx)) return JS_EXCEPTION;
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
                    if (JS_IsNull(argv[i])) {
                        // V4 parity (probed against live Qt 6.12): a JS null
                        // argument coerces to a NULL QString, not to the
                        // "null" string JS_ToString would produce.
                        nativeArgs[i].str = QString();
                        args[i+1] = &nativeArgs[i].str;
                        converted = true;
                    } else {
                        ScopedJSValue strVal(ctx, JS_ToString(ctx, argv[i]));
                        if (JS_IsException(strVal.value())) {
                            return JS_EXCEPTION;
                        }
                        nativeArgs[i].str = QuickJSConvert::jsToQString(ctx, strVal.value());
                        args[i+1] = &nativeArgs[i].str;
                        converted = true;
                    }
                } else if (expectedType == QMetaType::QByteArray && JS_IsNull(argv[i])) {
                    // V4 parity (probed): a JS null argument coerces to a
                    // NULL QByteArray (the generic void*->QByteArray
                    // conversion below would throw instead).
                    nativeArgs[i].ba = QByteArray();
                    args[i+1] = &nativeArgs[i].ba;
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
                    varArgs[i] = engine->toVariant(argv[i]);
                } else if ((expectedType == QMetaType::QDate || expectedType == QMetaType::QTime
                            || expectedType == QMetaType::QDateTime)
                           && JS_IsDate(argv[i])) {
                    // V4 parity (probed in America/New_York and Europe/Moscow;
                    // qv4engine.cpp toVariant passes the argument type as the
                    // hint): a Date narrows to a date/time argument through
                    // dateTargetFromVariant() - the local date/time with the
                    // QTBUG-92466 exception for QDate. An invalid (NaN) Date
                    // passes the invalid target value silently, like V4,
                    // instead of throwing.
                    varArgs[i] = dateTargetFromVariant(engine->toVariant(argv[i]), expectedType);
                } else {
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
                args[i + 1] = (expectedType == QMetaType::QVariant)
                                  ? static_cast<void*>(&varArgs[i])
                                  : varArgs[i].data();
            }
        }

        // Liveness re-check (H-3): the argument conversions above may have
        // run user JS (valueOf, toString, getters) that deleted the target
        // QObject; the metacall must not go through the dangling pointer.
        if (Q_UNLIKELY(data->obj.isNull())) {
            return JS_ThrowTypeError(ctx, "C++ object has been deleted");
        }

        return invokeAndConvertReturn(engine, ctx, obj, chosen->methodIdx, chosen->retType, args);
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
        if (!refreshMetaCacheIfChanged(engine, data)) return -1;

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
                // Invalid state: the property may have been removed and
                // re-added from the C++ side while the entry was
                // invalidated - re-probe like the read path (get_own_property),
                // so delete sees what get sees.
                QVariant dynProp = data->obj->property(e.name.constData());
                if (dynProp.isValid()) {
                    data->obj->setProperty(e.name.constData(), QVariant());
                    return 1;
                }
                return 0;
            }
        }

        const QString nameQ = QuickJSConvert::atomName(engine, ctx, prop);
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
