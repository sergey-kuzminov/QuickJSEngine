//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef QUICKJSQOBJECTPROXY_H
#define QUICKJSQOBJECTPROXY_H

#include "QuickJSEngine_p.h"

class QuickJSQObjectProxy {
public:
    // The single QMetaType read ladder shared by invokeAndConvertReturn()
    // and getMetaProperty() (a member so the lambdas keep friend access to
    // the engine's wrapQObject()/toJSValue()).
    static JSValue convertMetaRead(QQuickJSEngine* engine, JSContext* ctx, QObject* obj,
                                   QMetaObject::Call op, int slotIdx, int metaType,
                                   QVarLengthArray<void*, MetaCallArgsLimit>* args);
    static JSValue invokeAndConvertReturn(QQuickJSEngine* engine, JSContext* ctx, QObject* obj, int methodIdx, int retType, QVarLengthArray<void*, MetaCallArgsLimit>& args);
    static JSValue getMetaProperty(QQuickJSEngine* engine, JSContext* ctx, JSQObjectData* data, const MetaCacheEntry& entry);
    static JSValue getMethodProxy(QQuickJSEngine* engine, JSContext* ctx, JSQObjectData* data, JSAtom prop, const MetaCacheEntry& entry);
    static JSValue createMethodProxy(QQuickJSEngine* engine, JSContext* ctx, JSQObjectData* data, JSAtom prop, const MetaCacheEntry& entry, JSClassID classId);

    static int qobject_get_own_property(JSContext* ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop);
    static int qobject_get_own_property_names(JSContext* ctx, JSPropertyEnum** ptab, uint32_t* plen, JSValueConst obj);
    static int qobject_set_property(JSContext* ctx, JSValueConst this_obj, JSAtom prop, JSValueConst val, JSValueConst receiver, int flags);
    static JSValue js_method_invoke(JSContext* ctx, JSValueConst func_obj, JSValueConst this_val, int argc, JSValueConst* argv, int flags);
    static int qobject_delete_property(JSContext* ctx, JSValueConst obj, JSAtom prop);
};

#endif // QUICKJSQOBJECTPROXY_H
