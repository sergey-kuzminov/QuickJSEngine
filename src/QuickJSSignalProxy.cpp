//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSSignalProxy.h"
#include "QuickJSEngine_p.h"

#include <QMetaObject>

#include <algorithm>
#include <memory>

namespace {
struct ExecGuard {
    int* depth;
    explicit ExecGuard(int* d) : depth(d) { ++*depth; }
    ~ExecGuard() { if (depth) --*depth; }
    Q_DISABLE_COPY(ExecGuard)
};
}

int JSGenericReceiver::qt_metacall(QMetaObject::Call c, int id, void **a) {
    if (c != QMetaObject::InvokeMetaMethod) return QObject::qt_metacall(c, id, a);
    if (id == m_deleteLaterIdx) {
        if (callback) callback(a);
        return -1;
    }
    return QObject::qt_metacall(c, id, a);
}

JSValue QuickJSSignalProxy::js_signal_connect(JSContext* ctx, JSValueConst this_obj, int argc, JSValueConst* argv) {
    return invokeSafe(ctx, "signal_connect", [&]() -> JSValue {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
        if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected function as argument for connect()");

        auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(this_obj, engine->m_signalClassID));
        if (!data) {
            auto methodData = static_cast<const JSMethodProxyData*>(JS_GetOpaque(this_obj, engine->m_methodClassID));
            if (methodData) return JS_ThrowTypeError(ctx, "QObject method is not a signal");
            return JS_ThrowTypeError(ctx, "C++ object has been deleted");
        }
        if (data->obj.isNull()) return JS_ThrowTypeError(ctx, "C++ object has been deleted");

        QObject* sender = data->obj.data();
        if (sender->thread() != engine->thread()) {
            return JS_ThrowTypeError(ctx, "Cannot connect signal from a different thread");
        }

        // connect() targets the first declared signal of the name, like the
        // pre-overload-selection behavior.
        const int methodIdx = data->first().methodIdx;

        // Warn per connect: an unregistered parameter type converts to
        // undefined on direct emission, but a cross-thread queued emission
        // cannot copy the argument and the JS callback is never invoked.
        const QMetaMethod method = sender->metaObject()->method(methodIdx);
        for (int i = 0; i < data->first().paramCount; ++i) {
            if (data->first().paramTypes[i] == QMetaType::UnknownType) {
                qWarning("QQuickJSEngine: signal '%s' has parameter %d of unregistered type '%s'; "
                         "cross-thread queued emission will not deliver the JS callback",
                         method.name().constData(), i + 1, method.parameterTypeName(i).constData());
            }
        }

        auto receiver = std::make_unique<JSGenericReceiver>();
        receiver->paramCount = data->first().paramCount;
        receiver->paramTypes = data->first().paramTypes;

        receiver->m_ctx = ctx;
        receiver->m_jsCallback = JS_DupValue(ctx, argv[0]);
        receiver->m_sender = sender;
        receiver->m_methodIdx = methodIdx;

        QPointer<QQuickJSEngine> enginePtr = engine;
        JSGenericReceiver* rawReceiver = receiver.release();

        rawReceiver->callback = [receiver_ptr = rawReceiver, enginePtr](void** a) {
            if (enginePtr.isNull()) return;
            QQuickJSEngine* eng = enginePtr.data();
            JSContext* ctx = eng->m_ctx;

            try {
                const int argsSize = receiver_ptr->paramCount;
                QVarLengthArray<JSValue, 10> jsArgs;
                jsArgs.resize(argsSize);
                // QVarLengthArray leaves trivial types uninitialized; the guard
                // below frees every element, so fill with JS_UNDEFINED first.
                std::ranges::fill(jsArgs, JS_UNDEFINED);

                ScopedJSValueArray argsGuard(ctx, jsArgs);

                for (int i = 0; i < argsSize; ++i) {
                    JSValue v = eng->toJSValueFromVoid(a[i + 1], receiver_ptr->paramTypes[i]);
                    if (JS_IsException(v)) {
                        QQuickJSValue err = QQuickJSValue::fromException(eng);
                        if (err.isValid()) {
                            eng->processAndEmitException(ctx, err.jsVal());
                        } else {
                            clearPendingException(ctx);
                        }
                        return;
                    }
                    jsArgs[i] = v;
                }

                // Depth counting starts at the receiver creation (zero) and
                // only ever increments/decrements: a nested emission of the
                // same signal must add on TOP of the outer frame's count, so
                // the outer exit - not the nested one - brings it back to
                // zero. Resetting here would leave the outer frame
                // unprotected against a disconnect-time deletion (UAF).
                ExecGuard guard(&receiver_ptr->m_execDepth);

                // Entry bookkeeping (fresh quota + stack re-anchor at depth
                // 0, end-of-tick rejection flush) and the tick rule for the
                // microtask queue: a NESTED emission (a signal fired from a
                // running script) never drains the queue - run-to-completion,
                // the outer entry owns the drain.
                JsEntryScope entryScope(eng);
                if (Q_UNLIKELY(entryScope.starved())) {
                    // Refusal shape of the other JS-running entries: pure C
                    // throw, reported through the normal callback-exception
                    // channel (no script runs on the starved stack).
                    try { JS_ThrowRangeError(ctx, "Maximum call stack size exceeded"); } catch (...) {}
                    if (JS_HasException(ctx)) {
                        ScopedJSValue exc(ctx, JS_GetException(ctx));
                        eng->processAndEmitException(ctx, exc.value());
                    }
                    return;
                }

                ScopedJSValue ret(ctx, JS_Call(ctx, receiver_ptr->m_jsCallback, JS_UNDEFINED, argsSize, jsArgs.data()));

                if (JS_IsException(ret.value())) {
                    ScopedJSValue exc(ctx, JS_GetException(ctx));
                    eng->processAndEmitException(ctx, exc.value());
                }

                if (entryScope.isTopLevel() && eng->m_autoProcessPromises) {
                    eng->processPendingJobs();
                }
            } catch (const std::exception& e) {
                // Known std::exception -> catchable plain Error (uniform policy with safeMetacall)
                try { JS_ThrowPlainError(ctx, "C++ exception in signal callback: %s", e.what()); } catch (...) {}
                if (JS_HasException(ctx)) {
                    ScopedJSValue exc(ctx, JS_GetException(ctx));
                    eng->processAndEmitException(ctx, exc.value());
                }
            } catch (...) {
                try { JS_ThrowInternalError(ctx, "Unknown C++ exception in signal callback"); } catch (...) {}
                if (JS_HasException(ctx)) {
                    ScopedJSValue exc(ctx, JS_GetException(ctx));
                    eng->processAndEmitException(ctx, exc.value());
                }
            }
        };

        const bool connected = QMetaObject::connect(sender, methodIdx, rawReceiver, rawReceiver->m_deleteLaterIdx, Qt::AutoConnection);
        if (!connected) {
            delete rawReceiver;
            return JS_ThrowTypeError(ctx, "Failed to connect signal to JS function");
        }

        try {
            engine->m_signalReceivers.insert(sender, rawReceiver);
        } catch (...) {
            QMetaObject::disconnect(sender, methodIdx, rawReceiver, rawReceiver->m_deleteLaterIdx);
            delete rawReceiver;
            JS_ThrowOutOfMemory(ctx);
            return JS_EXCEPTION;
        }

        rawReceiver->m_destroyedConn = QObject::connect(sender, &QObject::destroyed, engine, [engine, rawReceiver](QObject* deadSender) {
            engine->m_signalReceivers.remove(deadSender, rawReceiver);
            if (rawReceiver->m_execDepth > 0) {
                engine->m_garbageReceivers.append(rawReceiver);
                QQuickJSEngine* eng = engine;
                QMetaObject::invokeMethod(eng, [eng, rawReceiver]() {
                    eng->m_garbageReceivers.removeAll(rawReceiver);
                    delete rawReceiver;
                }, Qt::QueuedConnection);
            } else {
                delete rawReceiver;
            }
        });
        Q_ASSERT(rawReceiver->m_destroyedConn);

        return JS_UNDEFINED;
    });
}

JSValue QuickJSSignalProxy::js_signal_disconnect(JSContext* ctx, JSValueConst this_obj, int argc, JSValueConst* argv) {
    return invokeSafe(ctx, "signal_disconnect", [&]() -> JSValue {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
        if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected function as argument for disconnect()");

        auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(this_obj, engine->m_signalClassID));
        if (!data) {
            auto methodData = static_cast<const JSMethodProxyData*>(JS_GetOpaque(this_obj, engine->m_methodClassID));
            if (methodData) return JS_ThrowTypeError(ctx, "QObject method is not a signal");
            return JS_ThrowTypeError(ctx, "C++ object has been deleted");
        }
        if (data->obj.isNull()) return JS_ThrowTypeError(ctx, "C++ object has been deleted");

        QObject* sender = data->obj.data();
        const int methodIdx = data->first().methodIdx;

        for (auto it = engine->m_signalReceivers.find(sender); it != engine->m_signalReceivers.end() && it.key() == sender; ) {
            JSGenericReceiver* receiver = it.value();
            if (receiver->m_methodIdx == methodIdx && JS_IsStrictEqual(ctx, receiver->m_jsCallback, argv[0])) {
                QMetaObject::disconnect(sender, methodIdx, receiver, receiver->m_deleteLaterIdx);
                QObject::disconnect(receiver->m_destroyedConn);
                it = engine->m_signalReceivers.erase(it);

                if (receiver->m_execDepth > 0) {
                    // A frame of this receiver is still executing (a nested
                    // emission): defer the deletion to the event loop.
                    engine->m_garbageReceivers.append(receiver);
                    QQuickJSEngine* eng = engine;
                    QMetaObject::invokeMethod(eng, [eng, receiver]() {
                        eng->m_garbageReceivers.removeAll(receiver);
                        delete receiver;
                    }, Qt::QueuedConnection);
                } else {
                    delete receiver;
                }
                continue;
            }
            ++it;
        }
        return JS_UNDEFINED;
    });
}
