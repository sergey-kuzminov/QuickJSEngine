//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

// Console and translation extensions of QQuickJSEngine (one class, several
// translation units - see QuickJSEngine_p.h): the console.* family, the
// qsTr/qsTranslate/qsTrId callbacks, String.prototype.arg, and
// installExtensions() that wires everything onto a global object.

#include "QuickJSEngine_p.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

#include <atomic>

// ---------------------------------------------------------------------------
// Console/translation extension internals (QJSEngine parity; semantics
// verified against qtdeclarative 6.10 sources: qjsengine.cpp /
// qqmlbuiltinfunctions.cpp, and against live Qt 6.12 behavior)
// ---------------------------------------------------------------------------

namespace {

// Ceiling for the console.count()/console.time() label tables. Both are
// keyed by arbitrary script-supplied labels and were the only engine-side
// containers without a natural bound: a script looping over dynamic labels
// grew them without limit, and the leak stays invisible to the JSMemoryUsage
// snapshot leakgate measures (it only sees the JS heap). 1024 labels matches
// the order of the other engine caches (AtomNameCacheLimit etc.); a script
// hitting it is almost certainly leaking labels on purpose.
constexpr int kConsoleLabelLimit = 1024;
std::atomic<bool> g_consoleLabelCapWarned{false};

QQuickJSEngine* engineFromContext(JSContext* ctx) {
    return static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

// One console argument rendered as a display string: ToString semantics for
// most values; symbols (which refuse implicit conversion) render as
// "Symbol(description)"; a failed conversion renders as a readable placeholder.
QString consoleArgToString(JSContext* ctx, JSValueConst val) {
    QString part = QuickJSConvert::jsToQString(ctx, val);
    if (part.isNull()) {
        // jsToQString failed and already cleared the pending exception;
        // substitute a readable fallback instead of printing nothing.
        if (JS_IsSymbol(val)) {
            ScopedJSValue desc(ctx, JS_GetPropertyStr(ctx, val, "description"));
            QString descStr;
            if (JS_IsString(desc.value())) {
                descStr = QuickJSConvert::jsToQString(ctx, desc.value());
            } else {
                clearPendingException(ctx);
            }
            part = descStr.isNull() ? QStringLiteral("Symbol()")
                                    : QStringLiteral("Symbol(%1)").arg(descStr);
        } else {
            part = QStringLiteral("[unprintable]");
        }
    }
    return part;
}

// console.assert()/console.exception() message: the arguments joined with a
// single space, like V4's writeToConsole().
QString joinConsoleArgs(JSContext* ctx, int argc, JSValueConst* argv, int start) {
    QStringList parts;
    for (int i = start; i < argc; ++i) {
        parts.append(consoleArgToString(ctx, argv[i]));
    }
    return parts.join(u' ');
}

// Generic console method body for log/info/debug/warn/error: the severity
// arrives in the magic argument. Installed through the static function-list
// table in installExtensions() instead of a per-method creation loop.
static JSValue js_console_generic(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    try {
        QStringList msgParts;
        for (int i = 0; i < argc; i++) {
            msgParts.append(consoleArgToString(ctx, argv[i]));
        }

        switch (static_cast<QtMsgType>(magic)) {
        case QtWarningMsg:
            qWarning().noquote() << "[JS console]" << msgParts.join(u' ');
            break;
        case QtCriticalMsg:
            qCritical().noquote() << "[JS console]" << msgParts.join(u' ');
            break;
        default:
            qDebug().noquote() << "[JS console]" << msgParts.join(u' ');
            break;
        }
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

QString consoleCallerStack(JSContext* ctx) {
    // Built through JS_NewError() on the internal JS_CLASS_ERROR prototype,
    // immune to scripts replacing globalThis.Error (the same hardening as
    // newErrorObject()). Its "stack" property lists this console.* frame
    // first, followed by the JavaScript callers; the console frame is
    // dropped below.
    ScopedJSValue err(ctx, JS_NewError(ctx));
    if (JS_IsException(err.value())) {
        clearPendingException(ctx);
        return QString();
    }
    ScopedJSValue stack(ctx, JS_GetPropertyStr(ctx, err.value(), "stack"));
    if (JS_IsException(stack.value())) {
        clearPendingException(ctx);
        return QString();
    }
    const QString s = QuickJSConvert::jsToQString(ctx, stack.value());
    if (s.isNull()) return QString();
    const qsizetype nl = s.indexOf(u'\n');
    return nl < 0 ? QString() : s.mid(nl + 1);
}
} // namespace

// qsTr keeps the fixed "QQuickJSEngine" translation context: the calling
// script's URL (V4's context source) is not visible to C callbacks in
// quickjs-ng.
static JSValue js_qsTr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) return JS_ThrowPlainError(ctx, "qsTr() requires at least one argument");
        if (!JS_IsString(argv[0])) return JS_ThrowPlainError(ctx, "qsTr(): first argument (sourceText) must be a string");
        if (argc > 1 && !JS_IsString(argv[1])) return JS_ThrowPlainError(ctx, "qsTr(): second argument (disambiguation) must be a string");
        if (argc > 2 && !JS_IsNumber(argv[2])) return JS_ThrowPlainError(ctx, "qsTr(): third argument (n) must be a number");

        const QString source = QuickJSConvert::jsToQString(ctx, argv[0]);
        QString comment;
        if (argc > 1) comment = QuickJSConvert::jsToQString(ctx, argv[1]);
        int n = -1;
        if (argc > 2) {
            // ToInt32 semantics: NaN/inf/out-of-range truncate
            // deterministically (a raw double-to-int cast is UB for those).
            int32_t i = 0;
            if (JS_ToInt32(ctx, &i, argv[2]) == 0) n = i;
            else clearPendingException(ctx);
        }
        return qStringToJS(ctx, QCoreApplication::translate("QQuickJSEngine",
                                                            source.toUtf8().constData(),
                                                            comment.toUtf8().constData(),
                                                            n));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "qsTr: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTr"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static JSValue js_qsTrNoOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_qsTranslate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    try {
        if (argc < 2) return JS_ThrowPlainError(ctx, "qsTranslate() requires at least two arguments");
        if (!JS_IsString(argv[0])) return JS_ThrowPlainError(ctx, "qsTranslate(): first argument (context) must be a string");
        if (!JS_IsString(argv[1])) return JS_ThrowPlainError(ctx, "qsTranslate(): second argument (sourceText) must be a string");
        if (argc > 2 && !JS_IsString(argv[2])) return JS_ThrowPlainError(ctx, "qsTranslate(): third argument (disambiguation) must be a string");

        const QString context = QuickJSConvert::jsToQString(ctx, argv[0]);
        const QString text = QuickJSConvert::jsToQString(ctx, argv[1]);
        QString comment;
        if (argc > 2) comment = QuickJSConvert::jsToQString(ctx, argv[2]);

        int i = 3;
        if (argc > i && JS_IsString(argv[i])) {
            qWarning("qsTranslate(): specifying the encoding as fourth argument is deprecated");
            ++i;
        }
        int n = -1;
        if (argc > i) {
            // ToInt32 semantics (see js_qsTr).
            int32_t v = 0;
            if (JS_ToInt32(ctx, &v, argv[i]) == 0) n = v;
            else clearPendingException(ctx);
        }

        return qStringToJS(ctx, QCoreApplication::translate(context.toUtf8().constData(),
                                                            text.toUtf8().constData(),
                                                            comment.toUtf8().constData(),
                                                            n));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "qsTranslate: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTranslate"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static JSValue js_qsTranslateNoOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[1]);
}

static JSValue js_qsTrId(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    try {
        if (argc < 1) return JS_ThrowPlainError(ctx, "qsTrId() requires at least one argument");
        if (!JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "qsTrId(): first argument (id) must be a string");
        if (argc > 1 && !JS_IsNumber(argv[1])) return JS_ThrowTypeError(ctx, "qsTrId(): second argument (n) must be a number");

        const QString id = QuickJSConvert::jsToQString(ctx, argv[0]);
        int n = -1;
        if (argc > 1) {
            // ToInt32 semantics (see js_qsTr).
            int32_t v = 0;
            if (JS_ToInt32(ctx, &v, argv[1]) == 0) n = v;
            else clearPendingException(ctx);
        }
        return qStringToJS(ctx, qtTrId(id.toUtf8().constData(), n));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "qsTrId: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in qsTrId"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

static JSValue js_qsTrIdNoOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

// String.prototype.arg(value): replaces the lowest numbered %N marker, like
// V4's GlobalExtensions::method_string_arg (int/double/bool/string values
// map to the corresponding QString::arg() overload).
static JSValue js_string_arg(JSContext* ctx, JSValueConst thisObj, int argc, JSValueConst* argv) {
    try {
        if (argc != 1) return JS_ThrowPlainError(ctx, "String.arg(): Invalid arguments");
        const QString value = QuickJSConvert::jsToQString(ctx, thisObj);
        if (value.isNull()) return JS_UNDEFINED;

        if (JS_IsNumber(argv[0])) {
            if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
                return qStringToJS(ctx, value.arg(JS_VALUE_GET_INT(argv[0])));
            }
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[0]) == 0) return qStringToJS(ctx, value.arg(d));
            clearPendingException(ctx);
            return JS_UNDEFINED;
        }
        if (JS_IsBool(argv[0])) {
            return qStringToJS(ctx, value.arg(JS_VALUE_GET_BOOL(argv[0]) != 0));
        }
        const QString argStr = QuickJSConvert::jsToQString(ctx, argv[0]);
        if (argStr.isNull()) return JS_UNDEFINED;
        return qStringToJS(ctx, value.arg(argStr));
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(ctx, "String.arg: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(ctx, "Unknown C++ exception in String.arg"); } catch (...) {}
        return JS_EXCEPTION;
    }
}

void QQuickJSEngine::installExtensions(QQuickJSEngine::Extensions extensions, const QQuickJSValue& object) {
    ensureEngineThread();

    // QJSEngine parity: reject values owned by another engine; non-object
    // targets (including the default argument) fall back to the global object.
    JSValue target = m_globalObj;
    if (object.isValid()) {
        if (object.m_engine.data() != this) {
            qWarning("QQuickJSEngine: Trying to install extensions from a different engine");
            return;
        }
        if (JS_IsObject(object.m_val)) target = object.m_val;
    }

    // Entry bookkeeping like the other JS entry points: the presence probe
    // below reads target properties (JS_GetPropertyStr runs user getters on
    // a custom target object) and the install writes them (JS_SetPropertyStr
    // runs user setters), so the guard must be anchored from THIS entry and
    // the starved refusal applies.
    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
        if (Q_UNLIKELY(m_stackStarved)) {
            // Starved entry (see evaluate()): installing executes user
            // accessors on the target under the emergency budget - the
            // exact execution the entry refusal exists to prevent. A void
            // setup API has no error channel: report and skip the install;
            // every subsequent evaluate() refuses on its own anchor
            // anyway, and re-calling installExtensions() after the host
            // climbs back to a healthy stack installs normally.
            qWarning("QQuickJSEngine: installExtensions() refused - the stack is "
                     "starved below the safety margin at this entry; console/gc/qsTr "
                     "were not installed");
            return;
        }
    }
    EvalDepthGuard depthGuard(m_evalDepth);
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

    // Deduplicate by the actual property presence, not only by the installed
    // mask: scripts can delete console/gc/qsTr from the target object, and a
    // repeated request must reinstall what is missing.
    auto needsInstall = [this, target](Extension ext, const char* propName) -> bool {
        if (!m_installedExtensions.testFlag(ext)) return true;
        ScopedJSValue v(m_ctx, JS_GetPropertyStr(m_ctx, target, propName));
        if (JS_IsException(v.value())) {
            clearPendingException(m_ctx);
            return false;
        }
        return JS_IsUndefined(v.value());
    };

    Extensions toInstall = Extensions(0);
    if (extensions.testFlag(ConsoleExtension) && needsInstall(ConsoleExtension, "console")) toInstall |= ConsoleExtension;
    if (extensions.testFlag(GarbageCollectionExtension) && needsInstall(GarbageCollectionExtension, "gc")) toInstall |= GarbageCollectionExtension;
    if (extensions.testFlag(TranslationExtension) && needsInstall(TranslationExtension, "qsTr")) toInstall |= TranslationExtension;
    if (static_cast<int>(toInstall) == 0) return;

    // Per-extension success accounting (N-5): a bit is set only when that
    // extension actually installed, so a failed install is not masked as
    // done - the presence probe above reinstalls on the next call.
    Extensions installed = Extensions(0);

    if (toInstall.testFlag(ConsoleExtension)) {
        ScopedJSValue console(m_ctx, JS_NewObject(m_ctx));
        if (JS_IsException(console.value())) {
            clearPendingException(m_ctx);
        } else {
            // log/info/debug/warn/error share one generic C function with a
            // severity magic; the static table installs all five methods in
            // one JS_SetPropertyFunctionList() pass.
            static constexpr JSCFunctionListEntry kConsoleMethods[] = {
                JS_CFUNC_MAGIC_DEF("log", 1, js_console_generic, QtDebugMsg),
                JS_CFUNC_MAGIC_DEF("info", 1, js_console_generic, QtDebugMsg),
                JS_CFUNC_MAGIC_DEF("debug", 1, js_console_generic, QtDebugMsg),
                JS_CFUNC_MAGIC_DEF("warn", 1, js_console_generic, QtWarningMsg),
                JS_CFUNC_MAGIC_DEF("error", 1, js_console_generic, QtCriticalMsg),
            };
            if (JS_SetPropertyFunctionList(m_ctx, console.value(), kConsoleMethods,
                                            static_cast<int>(sizeof(kConsoleMethods) / sizeof(kConsoleMethods[0]))) < 0) {
                clearPendingException(m_ctx);
            }

            // QJSEngine ConsoleExtension parity: assert, count, time, timeEnd,
            // trace, exception. Timers and counters live on the engine (V4
            // stores them on the engine, too), not on the target object.
            auto addConsoleNative = [this](JSValue obj, const char* name, JSCFunction* fn, int length) {
                JSValue func = JS_NewCFunction(m_ctx, fn, name, length);
                if (JS_SetPropertyStr(m_ctx, obj, name, func) < 0) {
                    clearPendingException(m_ctx);
                }
            };

            auto jsConsoleAssert = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc == 0) return JS_ThrowPlainError(ctx, "console.assert(): Missing argument");
                    const int truthy = JS_ToBool(ctx, argv[0]);
                    if (truthy < 0) clearPendingException(ctx);
                    if (truthy == 1) return JS_UNDEFINED;
                    const QString message = joinConsoleArgs(ctx, argc, argv, 1);
                    const QString stack = consoleCallerStack(ctx);
                    qCritical().noquote() << "[JS console]" << (stack.isEmpty() ? message : message + u'\n' + stack);
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleCount = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    QQuickJSEngine* engine = engineFromContext(ctx);
                    if (!engine) return JS_UNDEFINED;
                    // No argument -> the EMPTY label, not Chromium's
                    // "default": this matches Qt's own qml console
                    // (probed against live Qt 6.12: count() prints ": 1"),
                    // which is the parity target for this console.
                    const QString label = argc > 0 ? consoleArgToString(ctx, argv[0]) : QString();
                    // Known label: count on. New label while the table is at
                    // the ceiling: report 1 WITHOUT storing (the counter
                    // cannot accumulate; say so once instead of growing
                    // without bound - see kConsoleLabelLimit). The latch and
                    // the ceiling are SHARED with console.time() (Q5): both
                    // tables grow on dynamic labels, and one once-per-process
                    // warning covers both.
                    const auto it = engine->m_consoleCounters.find(label);
                    int value = 1;
                    if (it != engine->m_consoleCounters.end()) {
                        value = ++it.value();
                    } else if (engine->m_consoleCounters.size() < kConsoleLabelLimit) {
                        engine->m_consoleCounters.insert(label, 1);
                    } else if (!g_consoleLabelCapWarned.exchange(true)) {
                        qWarning("QQuickJSEngine: console.count()/console.time() label tables are capped"
                                 " (%d labels each) - new count labels are counted but not stored",
                                 kConsoleLabelLimit);
                    }
                    // Concatenation, not .arg(): a label containing a %N
                    // marker would otherwise be substituted by the chained
                    // arg() calls ("a%1b: 5" printed as "a5b: %2").
                    qDebug().noquote() << "[JS console]" << (label + QStringLiteral(": ") + QString::number(value));
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleTime = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc != 1) return JS_ThrowPlainError(ctx, "console.time(): Invalid arguments");
                    QQuickJSEngine* engine = engineFromContext(ctx);
                    if (engine) {
                        QElapsedTimer timer;
                        timer.start();
                        const QString label = consoleArgToString(ctx, argv[0]);
                        // A repeated label replaces its timer (no growth);
                        // a new label past the ceiling is dropped with a
                        // one-shot warning - the matching timeEnd() then
                        // stays silent, like any label without a timer
                        // (see kConsoleLabelLimit).
                        if (engine->m_consoleTimers.contains(label)
                                || engine->m_consoleTimers.size() < kConsoleLabelLimit) {
                            engine->m_consoleTimers.insert(label, timer);
                        } else if (!g_consoleLabelCapWarned.exchange(true)) {
                            // Shared latch with console.count() (Q5): the single
                            // warning names both tables.
                            qWarning("QQuickJSEngine: console.count()/console.time() label tables are capped"
                                     " (%d labels each) - new timers are dropped",
                                     kConsoleLabelLimit);
                        }
                    }
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleTimeEnd = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc != 1) return JS_ThrowPlainError(ctx, "console.timeEnd(): Invalid arguments");
                    QQuickJSEngine* engine = engineFromContext(ctx);
                    if (engine) {
                        const QString label = consoleArgToString(ctx, argv[0]);
                        const auto it = engine->m_consoleTimers.find(label);
                        if (it != engine->m_consoleTimers.end()) {
                            const qint64 elapsed = it.value().elapsed();
                            engine->m_consoleTimers.erase(it);
                            // V4 format: qDebug("%s: %llims", name, elapsed);
                            // concatenation for the same %N reason as count().
                            qDebug().noquote() << "[JS console]" << (label + QStringLiteral(": ") + QString::number(elapsed) + QStringLiteral("ms"));
                        }
                    }
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleTrace = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* /*argv*/) -> JSValue {
                try {
                    if (argc != 0) return JS_ThrowPlainError(ctx, "console.trace(): Invalid arguments");
                    const QString stack = consoleCallerStack(ctx);
                    if (!stack.isEmpty()) {
                        qDebug().noquote() << "[JS console]" << stack;
                    }
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            auto jsConsoleException = [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                try {
                    if (argc == 0) return JS_ThrowPlainError(ctx, "console.exception(): Missing argument");
                    const QString message = joinConsoleArgs(ctx, argc, argv, 0);
                    const QString stack = consoleCallerStack(ctx);
                    qCritical().noquote() << "[JS console]" << (stack.isEmpty() ? message : message + u'\n' + stack);
                    return JS_UNDEFINED;
                } catch (const std::exception& e) {
                    try { JS_ThrowPlainError(ctx, "console: %s", e.what()); } catch (...) {}
                    return JS_EXCEPTION;
                } catch (...) {
                    try { JS_ThrowInternalError(ctx, "console: unknown C++ exception"); } catch (...) {}
                    return JS_EXCEPTION;
                }
            };

            addConsoleNative(console.value(), "assert", jsConsoleAssert, 1);
            addConsoleNative(console.value(), "count", jsConsoleCount, 0);
            addConsoleNative(console.value(), "time", jsConsoleTime, 1);
            addConsoleNative(console.value(), "timeEnd", jsConsoleTimeEnd, 1);
            addConsoleNative(console.value(), "trace", jsConsoleTrace, 0);
            addConsoleNative(console.value(), "exception", jsConsoleException, 1);

            // print(): a console.log() alias installed on the target object
            // itself, like QJSEngine installs it next to console.
            ScopedJSValue logFn(m_ctx, JS_GetPropertyStr(m_ctx, console.value(), "log"));
            if (JS_SetPropertyStr(m_ctx, target, "console", console.release()) < 0) {
                clearPendingException(m_ctx);
            } else {
                installed |= ConsoleExtension;
                if (JS_IsFunction(m_ctx, logFn.value())) {
                    if (JS_SetPropertyStr(m_ctx, target, "print", JS_DupValue(m_ctx, logFn.value())) < 0) {
                        clearPendingException(m_ctx);
                    }
                } else {
                    clearPendingException(m_ctx);
                }
            }
        }
    }

    if (toInstall.testFlag(GarbageCollectionExtension)) {
        JSValue func = JS_NewCFunction(m_ctx, [](JSContext* ctx, JSValueConst, int, JSValueConst*) -> JSValue {
            try {
                JSRuntime* rt = JS_GetRuntime(ctx);
                QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
                if (engine) engine->collectGarbage();
                return JS_UNDEFINED;
            } catch (const std::exception& e) {
                try { JS_ThrowPlainError(ctx, "gc: %s", e.what()); } catch (...) {}
                return JS_EXCEPTION;
            } catch (...) {
                try { JS_ThrowInternalError(ctx, "Unknown C++ exception in gc"); } catch (...) {}
                return JS_EXCEPTION;
            }
        }, "gc", 0);

        if (JS_SetPropertyStr(m_ctx, target, "gc", func) < 0) {
            clearPendingException(m_ctx);
        } else {
            installed |= GarbageCollectionExtension;
        }
    }

    if (toInstall.testFlag(TranslationExtension)) {
        bool translationOk = true;
        auto addTargetFn = [this, target, &translationOk](const char* name, JSCFunction* fn, int length) {
            JSValue func = JS_NewCFunction(m_ctx, fn, name, length);
            if (JS_SetPropertyStr(m_ctx, target, name, func) < 0) {
                clearPendingException(m_ctx);
                translationOk = false;
            }
        };
        addTargetFn("qsTr", js_qsTr, 1);
        addTargetFn("QT_TR_NOOP", js_qsTrNoOp, 1);
        addTargetFn("qsTranslate", js_qsTranslate, 2);
        addTargetFn("QT_TRANSLATE_NOOP", js_qsTranslateNoOp, 2);
        addTargetFn("qsTrId", js_qsTrId, 1);
        addTargetFn("QT_TRID_NOOP", js_qsTrIdNoOp, 1);

        // String.prototype.arg() is installed on the engine-wide string
        // prototype (like V4), regardless of the target object.
        ScopedJSValue stringCtor(m_ctx, JS_GetPropertyStr(m_ctx, m_globalObj, "String"));
        if (JS_IsException(stringCtor.value())) {
            clearPendingException(m_ctx);
        } else {
            ScopedJSValue proto(m_ctx, JS_GetPropertyStr(m_ctx, stringCtor.value(), "prototype"));
            if (JS_IsException(proto.value())) {
                clearPendingException(m_ctx);
            } else if (JS_IsObject(proto.value())) {
                JSValue func = JS_NewCFunction(m_ctx, js_string_arg, "arg", 1);
                if (JS_SetPropertyStr(m_ctx, proto.value(), "arg", func) < 0) {
                    clearPendingException(m_ctx);
                    translationOk = false;
                }
            }
        }
        if (translationOk) installed |= TranslationExtension;
    }

    m_installedExtensions |= installed;
}
