//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

// ES module support of QQuickJSEngine (one class, several translation
// units - see QuickJSEngine_p.h): the canonical-path singleton cache, the
// moduleNormalize/moduleLoader callbacks, registered modules and
// importModule()/registerModule().

#include "QuickJSEngine_p.h"
#include "QuickJSConvert.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// ---------------------------------------------------------------------------
// ES modules: importModule() / registerModule() (QJSEngine parity; semantics
// verified against live Qt 6.12: canonical-path singleton cache, registered
// modules matched by the bare specifier, default export = the value itself,
// snapshot of enumerable own properties at registration time)
// ---------------------------------------------------------------------------

// Canonical cache key for a module file: QFileInfo::canonicalFilePath() with
// an absolute-path fallback for files that do not exist yet. Qt resource
// paths (":/...") are already canonical.
static QString canonicalModulePath(const QString& fileName) {
    if (fileName.startsWith(QLatin1Char(':'))) return fileName;
    const QFileInfo fi(fileName);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
}

char* QQuickJSEngine::moduleNormalize(JSContext* ctx, const char* base_name,
                                      const char* module_name, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    // Runs on pure C frames inside module linking; an escaping C++
    // exception would unwind through them. Best effort on failure: pass
    // the specifier through unresolved - the loader then reports the
    // standard could-not-load error.
    try {
        const QString spec = QString::fromUtf8(module_name);

        if (!spec.startsWith(QLatin1Char('.')) && !spec.startsWith(QLatin1Char('/'))) {
            // Bare specifier (or a ":/..." resource path): registered modules are
            // matched by the exact name, like QJSEngine; everything else is passed
            // through unchanged for the module loader.
            const auto it = engine->m_registeredModules.find(spec);
            if (it != engine->m_registeredModules.end()) {
                return js_strdup(ctx, it.value().constData());
            }
            return js_strdup(ctx, module_name);
        }

        // Relative or root-absolute specifier: resolve against the importing
        // module's directory and canonicalize, so that different spellings of the
        // same file resolve to one module instance.
        QString combined = spec;
        if (spec.startsWith(QLatin1Char('.')) && base_name && *base_name) {
            const QString baseDir = QFileInfo(QString::fromUtf8(base_name)).absolutePath();
            if (!baseDir.isEmpty()) combined = baseDir + QLatin1Char('/') + spec;
        }
        const QString canonical = combined.startsWith(QLatin1Char(':'))
                                      ? QDir::cleanPath(combined)
                                      : canonicalModulePath(combined);
        const QByteArray utf8 = canonical.toUtf8();
        return js_strdup(ctx, utf8.constData());
    } catch (...) {
        return js_strdup(ctx, module_name);
    }
}

JSModuleDef* QQuickJSEngine::moduleLoader(JSContext* ctx, const char* module_name, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    if (!engine) return nullptr;
    // Runs on pure C frames; an escaping C++ exception would unwind through
    // them. Fail like the file-missing branch: the pending SyntaxError
    // surfaces through the importer's normal error channel.
    try {
        // The same file may already be loaded under a different spelling (for
        // example a bare name from one importer and a relative path from another);
        // unify on the canonical-keyed singleton instance. The entry may still be
        // uninstantiated (compiled only); the importer's linking instantiates it.
        const QString cacheKey = canonicalModulePath(QString::fromUtf8(module_name));
        if (JSModuleDef* existing = engine->m_fileModules.value(cacheKey).def) {
            return existing;
        }

        // QFile understands Qt resource paths (":/...").
        QFile file(cacheKey);
        if (!file.open(QIODevice::ReadOnly)) {
            // QJSEngine reports all module-loading failures as SyntaxError
            // objects; match the error class (the message text is our own).
            JS_ThrowSyntaxError(ctx, "could not load module filename '%s'", cacheKey.toUtf8().constData());
            return nullptr;
        }
        const QByteArray source = file.readAll();

        const QByteArray nameUtf8 = cacheKey.toUtf8();
        JSEvalOptions options = {
            .version = JS_EVAL_OPTIONS_VERSION,
            .eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY,
            .filename = nameUtf8.constData(),
            .line_num = 1,
        };
        JSValue mod = JS_Eval2(ctx, source.constData(), static_cast<size_t>(source.size()), &options);
        if (JS_IsException(mod)) return nullptr;
        JSModuleDef* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod));
        JS_FreeValue(ctx, mod);
        engine->m_fileModules.insert(cacheKey, FileModuleEntry{def, false});
        return def;
    } catch (...) {
        JS_ThrowSyntaxError(ctx, "could not load module filename '%s'", module_name);
        return nullptr;
    }
}

int QQuickJSEngine::registeredModuleInit(JSContext* ctx, JSModuleDef* m) {
    // Runs on pure C frames during module instantiation; an escaping C++
    // exception would unwind through them. -1 is the module-init failure
    // the importer already handles.
    try {
        // The snapshot (an internal object holding "default" plus the captured
        // named exports) was stored as the module's private value by
        // registerModule(); instantiation hands the values to the engine.
        ScopedJSValue snap(ctx, JS_GetModulePrivateValue(ctx, m));
        if (!JS_IsObject(snap.value())) return 0;

        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, snap.value(), JS_GPN_STRING_MASK) < 0) {
            return -1;
        }
        // RAII ownership: an escaping C++ exception (the safety net below)
        // used to skip the manual JS_FreePropertyEnum on its way out.
        const ScopedPropertyEnum propsGuard(ctx, props, count);

        int ret = 0;
        for (uint32_t i = 0; i < count && ret == 0; ++i) {
            ScopedCString name(ctx, JS_AtomToCString(ctx, props[i].atom));
            JSValue val = JS_GetProperty(ctx, snap.value(), props[i].atom);
            if (!name.isValid() || JS_IsException(val)) {
                // JS_SetModuleExport() consumes the value in all cases; free the
                // failed read result manually before bailing out.
                JS_FreeValue(ctx, val);
                ret = -1;
                break;
            }
            // Ownership of val passes to the module on success and is freed by
            // the engine on failure.
            if (JS_SetModuleExport(ctx, m, name.value(), val) < 0) ret = -1;
        }
        return ret;
    } catch (...) {
        return -1;
    }
}

QQuickJSValue QQuickJSEngine::importModule(const QString& fileName) {
    ensureEngineThread();

    // QJSEngine parity: a pending exception preempts the import and is
    // returned as the result instead of executing anything (the leaked
    // exception of a failed registration surfaces here in live Qt 6.12).
    if (JS_HasException(m_ctx)) {
        return QQuickJSValue::fromException(this);
    }

    // Entry bookkeeping (fresh quota, guard re-anchor, tick-end drain and
    // rejection flush - all in the scope; see evaluate()). A starved anchor
    // refuses before the module machinery runs: the window below it is
    // inside the guard's safety margin; module compilation/evaluation
    // would run on a stack that cannot even fit the throw path. Refuse
    // with the same catchable RangeError before entering JS.
    JsEntryScope entryScope(this);
    const bool topLevel = entryScope.isTopLevel();
    if (Q_UNLIKELY(entryScope.starved())) {
        return newErrorObject(QQuickJSValue::RangeError,
                               QStringLiteral("Maximum call stack size exceeded"));
    }

    // Singleton cache: the first import evaluates the module; every later
    // import - from C++ or from JavaScript - returns the same instance. A
    // cached entry that has never been driven through JS_EvalFunction (a
    // dependency whose import chain failed before linking) is evaluated now.
    const QString cacheKey = canonicalModulePath(fileName);
    const auto cached = m_fileModules.constFind(cacheKey);
    if (cached != m_fileModules.cend() && cached->instantiated) {
        JSValue ns = JS_GetModuleNamespace(m_ctx, cached->def);
        if (JS_IsException(ns)) {
            return QQuickJSValue::fromException(this);
        }
        return QQuickJSValue(this, ns, true);
    }

    JSModuleDef* def = nullptr;
    if (cached != m_fileModules.cend()) {
        def = cached->def;
    } else {
        QFile file(cacheKey);
        if (!file.open(QIODevice::ReadOnly)) {
            // QJSEngine reports all module-loading failures as SyntaxError
            // objects; match the error class (the message text is our own).
            JS_ThrowSyntaxError(m_ctx, "could not load module filename '%s'", cacheKey.toUtf8().constData());
            return QQuickJSValue::fromException(this);
        }
        const QByteArray source = file.readAll();

        const QByteArray nameUtf8 = cacheKey.toUtf8();
        JSEvalOptions options = {
            .version = JS_EVAL_OPTIONS_VERSION,
            .eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY,
            .filename = nameUtf8.constData(),
            .line_num = 1,
        };
        JSValue mod = JS_Eval2(m_ctx, source.constData(), static_cast<size_t>(source.size()), &options);
        if (JS_IsException(mod)) {
            // Syntax error and the like; Qt reports this as a SyntaxError, too.
            return QQuickJSValue::fromException(this);
        }
        def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod));
        JS_FreeValue(m_ctx, mod);
        // Register before evaluation: a module that throws stays cached, and a
        // re-import returns its namespace without re-running the code.
        m_fileModules.insert(cacheKey, FileModuleEntry{def, false});
    }

    // JS_EvalFunction consumes the module value it is given; hand it a fresh
    // reference to the def. Evaluation failures come in two shapes: linking
    // errors raise a context exception, while a module that throws in its
    // top-level code rejects the evaluation promise.
    JSValue ret = JS_EvalFunction(m_ctx, JS_DupValue(m_ctx, JS_MKPTR(JS_TAG_MODULE, def)));
    ScopedJSValue retScoped(m_ctx, ret);

    // Run-to-completion: only a top-level import drains the microtask queue.
    if (topLevel && m_autoProcessPromises) processPendingJobs();

    QQuickJSValue errVal;
    bool linked = true;
    if (JS_IsException(ret)) {
        // A linking failure leaves the def uninstantiated: a later import
        // retries the whole evaluation.
        errVal = QQuickJSValue::fromException(this);
        linked = false;
    } else if (JS_PromiseState(m_ctx, ret) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(m_ctx, ret);
        // The rejection was reported to the unhandled-rejection tracker when
        // it happened; the error is handed to the C++ caller instead, so
        // retract the buffered report to avoid a duplicate uncaughtException.
        retractHandledRejection(m_ctx, ret);
        errVal = QQuickJSValue(this, reason, true);
    }

    if (linked) {
        const auto entry = m_fileModules.find(cacheKey);
        if (entry != m_fileModules.end()) entry->instantiated = true;
    }

    // QJSEngine parity: a sticky interrupt flag yields an "Interrupted" error
    // object even when the module code failed for another reason (the same
    // precedence evaluate() applies).
    if (isInterrupted()) {
        return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
    }
    if (errVal.isValid()) {
        return errVal;
    }

    JSValue ns = JS_GetModuleNamespace(m_ctx, def);
    if (JS_IsException(ns)) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, ns, true);
}

bool QQuickJSEngine::registerModule(const QString& moduleName, const QQuickJSValue& value) {
    ensureEngineThread();

    // A pending exception preempts the registration, like evaluate().
    if (JS_HasException(m_ctx)) {
        return false;
    }

    if (!value.isValid() || value.engine() != this) {
        // Passing a foreign value is undefined behavior in QJSEngine; refuse
        // it with a warning instead of corrupting this engine.
        qWarning("QQuickJSEngine::registerModule: the value is invalid or belongs to another engine");
        return false;
    }

    // The snapshot reads property values, which may run user JavaScript
    // (getters): the scope keeps the tick bookkeeping consistent with the
    // other JS entry points (fresh quota, guard re-anchor, tick-end drain
    // and rejection flush - see evaluate()).
    JsEntryScope entryScope(this);
    // Starved entry: the snapshot below reads values through JS_GetProperty
    // - it runs user getters ("Values - including getters - are read
    // exactly once, here"), i.e. script execution on a stack the guard
    // already considers starved, where even the overflow throw path may
    // not fit below the anchor. Refuse like the other entry points:
    // 'false' keeps parity with every other registration failure, and the
    // pending RangeError (built through the pure C path - refusing must
    // not itself run script) surfaces at the next importModule()/
    // evaluate(), exactly like a throwing getter's failed registration.
    if (Q_UNLIKELY(entryScope.starved())) {
        JS_ThrowRangeError(m_ctx, "Maximum call stack size exceeded");
        return false;
    }

    // Snapshot: "default" is the value itself; every own enumerable string
    // property becomes a named export. Values - including getters - are read
    // exactly once, here; later modifications of the value are not visible to
    // importers, matching the documented QJSEngine semantics.
    JSValueConst val = value.jsVal();
    JSValue snap = JS_NewObject(m_ctx);
    if (JS_IsException(snap)) {
        clearPendingException(m_ctx);
        return false;
    }

    bool ok = true;
    QVarLengthArray<QByteArray, 16> exportNames;
    if (JS_IsObject(val)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(m_ctx, &props, &count, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            clearPendingException(m_ctx);
            JS_FreeValue(m_ctx, snap);
            return false;
        }
        // RAII ownership: the exportNames append below can throw bad_alloc
        // past the old manual free, leaking the enum array and its atoms.
        const ScopedPropertyEnum propsGuard(m_ctx, props, count);
        for (uint32_t i = 0; i < count && ok; ++i) {
            ScopedCString name(m_ctx, JS_AtomToCString(m_ctx, props[i].atom));
            if (!name.isValid()) {
                ok = false;
                break;
            }
            JSValue pv = JS_GetProperty(m_ctx, val, props[i].atom);
            if (JS_IsException(pv)) {
                // A throwing getter leaves the exception pending and fails
                // the registration, like live Qt 6.12 (the next import or
                // evaluate call surfaces the error).
                ok = false;
                break;
            }
            // Ownership of pv passes to the snapshot object in both cases.
            // Define, not Set: a Set on "__proto__" would install the value
            // as the snapshot's PROTOTYPE instead of an own property,
            // leaving the export declared but never filled.
            if (JS_DefinePropertyValue(m_ctx, snap, props[i].atom, pv, JS_PROP_C_W_E) < 0) {
                ok = false;
                continue;
            }
            // "default" is declared separately (it is the value itself,
            // like QJSEngine): a same-named property of the value is ignored.
            if (qstrcmp(name.value(), "default") != 0) {
                exportNames.append(QByteArray(name.value()));
            }
        }
        if (!ok) {
            JS_FreeValue(m_ctx, snap);
            return false;
        }
    }

    if (JS_SetPropertyStr(m_ctx, snap, "default", JS_DupValue(m_ctx, val)) < 0) {
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, snap);
        return false;
    }

    // A unique internal name lets re-registration replace the module for new
    // imports (the previous def stays in the engine's list, unreachable). The
    // public name is mapped by the normalizer.
    const QByteArray internal = "quickjsengine::module::" + QByteArray::number(++m_moduleSerial)
                                + "::" + moduleName.toUtf8();
    JSModuleDef* mod = JS_NewCModule(m_ctx, internal.constData(),
                                     &QQuickJSEngine::registeredModuleInit);
    if (!mod) {
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, snap);
        return false;
    }

    if (JS_AddModuleExport(m_ctx, mod, "default") < 0) {
        ok = false;
    }
    for (const QByteArray& exportName : std::as_const(exportNames)) {
        if (!ok) break;
        if (JS_AddModuleExport(m_ctx, mod, exportName.constData()) < 0) {
            ok = false;
        }
    }

    if (!ok) {
        // The half-registered module stays in the list under its unique name;
        // it is never referenced by the normalizer and cannot be imported.
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, snap);
        return false;
    }

    // JS_SetModulePrivateValue() takes ownership of the snapshot value.
    JS_SetModulePrivateValue(m_ctx, mod, snap);
    m_registeredModules.insert(moduleName, internal);
    return true;
}
