//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#ifndef QUICKJSSIGNALPROXY_H
#define QUICKJSSIGNALPROXY_H

#include <quickjs.h>

class QuickJSSignalProxy {
public:
    static JSValue js_signal_connect(JSContext* ctx, JSValueConst this_obj, int argc, JSValueConst* argv);
    static JSValue js_signal_disconnect(JSContext* ctx, JSValueConst this_obj, int argc, JSValueConst* argv);
};

#endif // QUICKJSSIGNALPROXY_H
