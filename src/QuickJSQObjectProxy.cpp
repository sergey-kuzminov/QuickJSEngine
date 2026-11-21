//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSQObjectProxy.h"
#include "QuickJSConvert.h"

#include <utility>
#include <memory>

namespace {

// Calls a meta-method, stores the return value in a default-constructed
// storage of type T and converts it to a JSValue. args[0] is the return slot.
template<typename T, typename Convert>
JSValue metacallReturn(JSContext* ctx, QObject* obj, int methodIdx,
                       QVarLengthArray<void*, MetaCallArgsLimit>& args, Convert convert)
{
    T val{};
    args[0] = &val;
    const bool ok = safeMetacall(ctx, obj, QMetaObject::InvokeMetaMethod, methodIdx, args.data());
    args[0] = nullptr;
    if (!ok) return JS_EXCEPTION;
    return convert(std::move(val));
}

// Reads a meta-property into a default-constructed storage of type T and
// converts it to a JSValue.
template<typename T, typename Convert>
JSValue readMetaProperty(JSContext* ctx, QObject* obj, int propIdx, Convert convert)
{
    T val{};
    void* a[1] = { &val };
    if (!safeMetacall(ctx, obj, QMetaObject::ReadProperty, propIdx, a)) return JS_EXCEPTION;
    return convert(std::move(val));
}

} // namespace

JSValue QuickJSQObjectProxy::invokeAndConvertReturn(QQuickJSEngine* engine, JSContext* ctx, QObject* obj, int methodIdx, int retType, QVarLengthArray<void*, MetaCallArgsLimit>& args) {
    switch (retType) {
    case QMetaType::Void: {
        if (!safeMetacall(ctx, obj, QMetaObject::InvokeMetaMethod, methodIdx, args.data())) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    case QMetaType::UnknownType:
        return JS_ThrowTypeError(ctx, "Cannot invoke method: return type is not registered");
    case QMetaType::Int:
        return metacallReturn<int>(ctx, obj, methodIdx, args, [ctx](int v) { return JS_NewInt32(ctx, v); });
    case QMetaType::Bool:
        return metacallReturn<bool>(ctx, obj, methodIdx, args, [ctx](bool v) { return JS_NewBool(ctx, v); });
    case QMetaType::Double:
        return metacallReturn<double>(ctx, obj, methodIdx, args, [ctx](double v) { return JS_NewFloat64(ctx, v); });
    case QMetaType::QString:
        return metacallReturn<QString>(ctx, obj, methodIdx, args, [ctx](QString v) { return qStringToJS(ctx, v); });
    case QMetaType::QObjectStar:
        return metacallReturn<QObject*>(ctx, obj, methodIdx, args, [engine, ctx](QObject* v) { return engine->wrapQObject(v); });
    case QMetaType::QVariant:
        return metacallReturn<QVariant>(ctx, obj, methodIdx, args, [engine](QVariant v) { return engine->toJSValue(v); });
    case QMetaType::QStringList:
        return metacallReturn<QStringList>(ctx, obj, methodIdx, args, [engine, ctx](QStringList v) { return QuickJSConvert::stringListToJSArray(engine, ctx, &v); });
    case QMetaType::QVariantList:
        return metacallReturn<QVariantList>(ctx, obj, methodIdx, args, [engine, ctx](QVariantList v) { return QuickJSConvert::listToJSArray(engine, ctx, &v, 0); });
    default: break;
    }

    if (retType == qMetaTypeId<QVariantMap>()) {
        return metacallReturn<QVariantMap>(ctx, obj, methodIdx, args, [engine, ctx](QVariantMap v) { return QuickJSConvert::mapToJSObject(engine, ctx, &v, 0); });
    }

    QVariant val = makeDefaultVariant(retType);
    args[0] = val.data();
    const bool ok = safeMetacall(ctx, obj, QMetaObject::InvokeMetaMethod, methodIdx, args.data());
    args[0] = nullptr;
    if (!ok) return JS_EXCEPTION;

    JSValue jsVal = engine->toJSValue(val);

    if (JS_IsUndefined(jsVal) && val.isValid() && !val.isNull()) {
        return JS_ThrowTypeError(ctx, "Cannot invoke method: return type '%s' is not supported", QMetaType(retType).name());
    }

    return jsVal;
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

    switch (metaType) {
    case QMetaType::Int:
        return readMetaProperty<int>(ctx, obj, entry.index, [ctx](int v) { return JS_NewInt32(ctx, v); });
    case QMetaType::Bool:
        return readMetaProperty<bool>(ctx, obj, entry.index, [ctx](bool v) { return JS_NewBool(ctx, v); });
    case QMetaType::Double:
        return readMetaProperty<double>(ctx, obj, entry.index, [ctx](double v) { return JS_NewFloat64(ctx, v); });
    case QMetaType::QString:
        return readMetaProperty<QString>(ctx, obj, entry.index, [ctx](QString v) { return qStringToJS(ctx, v); });
    case QMetaType::QObjectStar:
        return readMetaProperty<QObject*>(ctx, obj, entry.index, [engine, ctx](QObject* v) { return engine->wrapQObject(v); });
    case QMetaType::QVariant:
        return readMetaProperty<QVariant>(ctx, obj, entry.index, [engine](QVariant v) { return engine->toJSValue(v); });
    case QMetaType::QStringList:
        return readMetaProperty<QStringList>(ctx, obj, entry.index, [engine, ctx](QStringList v) { return QuickJSConvert::stringListToJSArray(engine, ctx, &v); });
    case QMetaType::QVariantList:
        return readMetaProperty<QVariantList>(ctx, obj, entry.index, [engine, ctx](QVariantList v) { return QuickJSConvert::listToJSArray(engine, ctx, &v, 0); });
    default: break;
    }

    if (metaType == qMetaTypeId<QVariantMap>()) {
        return readMetaProperty<QVariantMap>(ctx, obj, entry.index, [engine, ctx](QVariantMap v) { return QuickJSConvert::mapToJSObject(engine, ctx, &v, 0); });
    }

    QVariant val = makeDefaultVariant(metaType);
    void* a[1] = { val.data() };
    if (!safeMetacall(ctx, obj, QMetaObject::ReadProperty, entry.index, a)) return JS_EXCEPTION;

    JSValue jsVal = engine->toJSValue(val);

    if (JS_IsUndefined(jsVal) && val.isValid() && !val.isNull()) {
        return JS_ThrowTypeError(ctx, "Cannot read property: type '%s' is not supported", QMetaType(metaType).name());
    }

    return jsVal;
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
        // Design note: asymmetry in error handling. If reading a meta-property
        // throws (e.g. unsupported type), clear it and return JS_UNDEFINED so
        // Object.keys()/for...in survive a bad property. Dynamic properties and
        // direct method calls do re-throw.
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
                            clearPendingException(ctx);
                            desc->value = JS_UNDEFINED;
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
        auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(func_obj, engine->m_methodClassID));
        if (!data) data = static_cast<JSMethodProxyData*>(JS_GetOpaque(func_obj, engine->m_signalClassID));

        if (!data || data->obj.isNull()) return JS_ThrowTypeError(ctx, "C++ object has been deleted");

        QObject* obj = data->obj.data();
        const int paramCount = data->paramCount;

        QVarLengthArray<void*, MetaCallArgsLimit> args(paramCount + 1);
        args[0] = nullptr;

        struct NativeArgHolder {
            int i; bool b; double d; QString str; QObject* obj;
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
