# QuickJSEngine

**English** | [Русский](README_RU.md)

**QuickJSEngine** is a lightweight JavaScript engine for Qt, designed as an alternative to **QJSEngine** for console applications or widget-based (QtWidgets) applications that do not use the QtQml library. The project is a convenient C++ wrapper around the modern [quickjs-ng](https://github.com/quickjs-ng/quickjs) engine.

## Why JavaScript in C++ code?

Personally, I use JavaScript to add automation to my applications. The user can write a script on their own that adds a reaction in the program to some event (I write monitoring software).

I once used the **QtScript** module for this, which is based on JavaScriptCore (the WebKit engine) and has not been updated for a long time. It was replaced by **QJSEngine** (the V4 engine), but **QJSEngine** is an integral part of the QtQml module. Using **QJSEngine** drags in QML dependencies, which is excessive for purely console or widget applications. **QuickJSEngine** solves this problem by offering an independent, fast and modern JavaScript engine.

## The core: quickjs-ng

The core is **quickjs-ng** (ng — next generation) — a fork of the original QuickJS by Fabrice Bellard and Charlie Gordon, actively developed by the community.
* Supports modern JavaScript standards (ES2023+).
* A relatively compact and very fast interpreter.
* The **quickjs-ng** engine is distributed under the [MIT](https://opensource.org/license/mit) license.

## Philosophy and differences from QJSEngine

**QuickJSEngine** is a *lightweight* counterpart; it does **not** aim to implement absolutely all of **QJSEngine**'s functionality, since that is incompatible with the lightweight concept. Nor is all of QJSEngine's functionality required: the project targets console applications, not QML graphics. Still, the engine strives to stay as close as possible to the **QJSEngine** API for easy porting of existing code.

**What is not implemented (or implemented differently) compared to QJSEngine:**
* **Object ownership:** in **QJSEngine**, the GC (garbage collector) can delete a C++ object. In QuickJSEngine, C++ objects are never deleted by the JS garbage collector: lifetime is controlled exclusively by C++ code (via `parent` or `delete`), which is safer.
* **Q_GADGET:** passing Q_GADGET structures by value is not supported. Use `QObject*` or `QVariantMap`.
* **Lossy `toVariant()`:** in the default mode, JS-specific types (Map/Set/WeakMap/WeakSet/Promise/RegExp/DataView) and Symbols yield an *invalid* QVariant, where QJSEngine yields an empty `QVariantMap`, a `QRegularExpression` or a descriptive string. Use `toVariant(RetainJSObjects)` for a lossless `QVariant<QQuickJSValue>` wrap.
* **ES modules:** `importModule()` / `registerModule()` follow QJSEngine semantics; where QJSEngine refuses, the ECMAScript specification wins — circular imports, `import.meta`, top-level `await` and dynamic `import()` all work (each of them is a SyntaxError in QJSEngine).
* **Intentionally absent:** the V4 interop layer (`QJSManagedValue`, `QJSPrimitiveValue`, `coerceValue()`, `fromVariant()` — it mirrors the V4 memory model), engine-less `QJSValue(42)` constructors (every value is bound to its engine: thread-safety by design), `isVariant()` / `isUrl()` (deprecated in Qt 6.9 / V4-specific URL objects — Qt value types such as QUrl, QPointF, QSizeF or QRectF convert to `undefined` here, where V4 wraps them into value objects; strings still coerce back into such parameters where Qt's own QVariant conversion accepts them), `uiLanguage()` (JS-driven language switching is out of scope for console applications).
* **Minor divergences:** `newSymbol()` follows ECMA-262 (the `description` is set; V4 leaves it undefined), `console.count()` keys counters by the printed label instead of by script and line, the `qsTr()` translation context is fixed, and `Object.keys()` invokes C++ getters (a throwing getter fails the enumeration), where QJSEngine never invokes getters during key enumeration.

<details>
<summary>Fine print: exact divergences</summary>

* **ES modules, exact list:** `importModule()` / `registerModule()` are implemented with QJSEngine semantics: a singleton cache keyed by the canonical file path, registered modules matched by the bare specifier, the registered value itself as the default export, a snapshot of enumerable own properties at registration time, `":/..."` Qt resource paths, and a module that threw at top level is not re-executed on the next import. Beyond that, each of the following is a SyntaxError in QJSEngine but works here: circular imports (allowed by the ECMAScript specification), `import.meta` (a plain object per the specification), top-level `await` (ES2022), dynamic `import()` inside `evaluate()` scripts (which can request registered modules too), registered names without a `.mjs` suffix, and a bare specifier that is not a registered module resolving as a file relative to the current working directory. All module-loading failures are `SyntaxError` objects like in QJSEngine, with our own message texts (a missing file is named in the message instead of V4's cryptic `Invalid null URL`). `importModule()` pumps the microtask queue by default (`setAutoProcessPromises(false)` disables it), so modules with a top-level `await` complete synchronously.
* **Error delivery:** exceptions of JS signal handlers are reported (the `uncaughtException` signal, or a `qWarning` when nobody listens) and the signal emission continues, exactly like QJSEngine; `Object.keys()` on a QObject whose C++ getter throws (via `throwError()`) fails with that error, where QJSEngine never invokes getters during key enumeration at all.
* **Unhandled promise rejections:** reported once, at the end of the tick, through the same `uncaughtException` channel (Node.js-like; QJSEngine/V4 has no rejection tracker at all). A rejection handled by a late `.catch()` in the same tick is retracted and never reported. The pending-rejection buffer is capped at 1024 entries: with `setAutoProcessPromises(false)` and a never-drained job queue it would otherwise grow without bound (each entry pins the promise and its reason on the JS heap — a leak invisible to the script). Past the cap the OLDEST buffered rejection is reported immediately to make room, so every rejection is still reported exactly once — the report just arrives earlier than end-of-tick for overflow entries.
* **Lossy `toVariant()` refusals:** in the default (ConvertJSObjects) mode, Map/Set/WeakMap/WeakSet/Promise/RegExp/DataView yield an *invalid* QVariant, where QJSEngine yields an empty `QVariantMap` (Map/Set/Promise) or a `QRegularExpression` (RegExp). Symbols also yield an invalid QVariant, where QJSEngine yields the descriptive `"Symbol(name)"` string. This is deliberate: a string-shaped variant would silently coerce in metacall conversions (e.g. a nullptr written into a `QObject*` property) instead of throwing a TypeError. Use `toVariant(RetainJSObjects)` for a lossless `QVariant<QQuickJSValue>` wrap (function objects are retained in both modes, like in Qt 6.12).
* **Detached buffers:** `toVariant()` of a detached `ArrayBuffer` (or a TypedArray over one) yields an empty `QByteArray`, like QJSEngine. A view past the end of a live resizable `ArrayBuffer` instead fails the conversion — also V4 parity; quickjs-ng throws the same `TypeError` for both cases, so the buffer is probed through its getter to tell them apart.
* **Date/time conversions:** a valid `QDateTime`, `QDate` or `QTime` converts to a real `Date` (epoch milliseconds), an invalid one to an Invalid Date — V4 parity. `QTime` is anchored exactly like V4: the Date is the local `QDateTime(1971-04-01, time)` (a DST-transition-free window picked by qtdeclarative for time-without-a-date), so the epoch depends on the timezone, and `toVariant()` of such a Date returns a `QDateTime`, not a `QTime` — both are inherited V4 warts. A valid `QDate` anchors the UTC midnight of that date, exactly like V4 (`startOfDay(QTimeZone::utc())` in qv4dateobject.cpp), so the epoch is timezone-independent. The reverse direction matches V4 too: a `Date` narrows to a `QDate` through its local date with the QTBUG-92466 exception (an instant that is exactly the UTC midnight of a day whose local wall time falls on the previous day — a negative-offset zone — yields the UTC date, so date-only strings and `QDate`-anchored Dates round-trip), to a `QTime` through the local time-of-day; an invalid (NaN) `Date` yields the invalid target value silently, without a TypeError, in every date/time slot.
* **Dynamic properties (wrapper extension):** assigning a property the wrapped QObject does not declare creates a C++ dynamic property on that object (through `QObject::setProperty`). QJSEngine does NOT do this — it keeps unknown writes as plain JS properties on the wrapper object, invisible to C++ (probed against live Qt 6.12); the C++-visible mirror is a deliberate extension of this wrapper. `undefined` removes the dynamic property (the counterpart of a JS property deletion), `null` stores a null-marker QVariant that reads back as JS `null`, and a value that cannot be represented as a QVariant at all (a Symbol, a RegExp, a Promise) throws a TypeError instead of silently deleting the property.
* **`null`/`undefined` property writes (V4 parity):** meta-property writes coerce JS `null`/`undefined` through the same strict table QJSEngine uses (probed against live Qt 6.12), not through the JS valueOf/ToNumber machinery. `undefined` throws `"Cannot assign [undefined] to <type>"` for every property type except a QVariant-typed property, which accepts it as an invalid QVariant. `null` writes the type's null value where QJSEngine accepts one — a null `QString`/`QByteArray`, `false` for bool, `0` for double/float, an empty `QStringList`, a `nullptr` for `QObject*`, a `std::nullptr_t` QVariant for a QVariant-typed property (it reads back as JS `null`) — and throws `"Cannot assign std::nullptr_t to <type>"` everywhere else (integers, dates, QChar, containers). Method ARGUMENTS follow plain JS coercion instead, exactly like V4: `null` coerces to a null `QString`/`QByteArray` (not to the `"null"` string), `undefined` to the `"undefined"` string, and numbers through ToNumber.
* **Failed numeric coercions keep the user's exception (V4 parity):** a JS value that fails numeric conversion on its way into an `int`/`double`/`float` property or method argument — an object whose `valueOf()`/`toString()` throws, a `Symbol`, a `BigInt` — propagates the ORIGINAL JS exception (probed against live Qt 6.12: V4's double/float property paths and all argument paths do exactly this). A coercion that succeeds assigns normally: an object with a returning `valueOf()` writes the number, a `valueOf`-less plain object and a QObject wrapper both coerce to `NaN` in floating-point slots without a TypeError. Divergence kept: V4's `int` property set-path rejects ANY object outright with `"Cannot assign QJSValue to int"` (no valueOf call at all), while this wrapper runs the full ECMAScript coercion there too — the wrapper's uniform coercion contract.
* **Embedded NULs in property names:** JS property names may contain U+0000. The `QQuickJSValue` and engine `setProperty()` overloads taking a `QByteArray`, and the object→`QVariantMap` conversion, keep such names whole (V4 parity, probed: two keys differing only past the NUL stay two map entries); a `const char*` name stops at its terminating NUL by definition.
* **Proxied values in `toVariant()` (V4 parity):** a `Proxy` is never punched through during QVariant conversion — `new Proxy([1,2,3], {}).toVariant()` is an index-keyed `QVariantMap` (`{"0":1,...}`), not a list, exactly like QJSEngine (probed). This matches quickjs-ng 0.18's `JS_IsArray`, which also no longer punches through proxies.
* **Dead QObject wrappers:** after the wrapped C++ object is deleted, the JS wrapper survives and `isQObject()` stays `true` (QJSEngine parity: the classification does not branch on the pointer being alive), while `toQObject()` returns `nullptr`. For a QObject that lived in another thread the `destroyed` cleanup is delivered queued to the engine thread; a wrapper cached during that delivery window is re-validated by pointer identity on the next `newQObject()` of the same (possibly reused) address, so address reuse cannot serve a dead wrapper for a live object.
* **`console.count()` keying:** counters are keyed by the printed label (Node.js semantics), not by script and line number like V4 — quickjs-ng exposes no call-site information to C callbacks. The printed format (`label: N`) is identical. The label tables of `console.count()` / `console.time()` are capped at 1024 entries: a new label beyond the cap is counted/timed but not stored (the tables would otherwise grow without bound on dynamic labels, and that C++-side memory is invisible to `memoryUsage()`); the first overflow prints a one-shot warning.
* **`qsTr()` context:** the translation context is fixed (`"QQuickJSEngine"`); V4 derives it from the calling script's URL, which is not visible to C callbacks in quickjs-ng.
* **`exceptionStackTrace` frames:** parsed from the quickjs-ng `Error.stack` property into the V4 frame format `function:line:column:file`; native frames carry `-1/-1` and an empty file, like V4. A thrown non-Error value (e.g. `throw 'str'`) yields an empty list, since quickjs-ng attaches no stack to it. A nested `evaluate()` called from a C++ slot keeps the outer script frames in the inner error's stack, matching V4.
* **`isError()` for thrown primitives:** `isError()` is true for ANY thrown value — `throw 42` returns an error-flagged value holding 42 — while QJSEngine's `isError()` is true only for Error objects. This keeps C++ handling branch-free (everything reported by `evaluate()`/`call()` as an error IS one); `toString()` still returns the raw value. `errorType()` of such a value is `NoError`, exactly like QJSEngine: only real Error objects carry a class.
* **Engine-created errors:** `newErrorObject()` / `throwError()` build on the built-in prototypes, immune to scripts replacing `globalThis.TypeError` and friends (like V4). For `EvalError`/`URIError` the global constructor is still resolved: quickjs-ng 0.18 provides C helpers for the other five classes only. Messages containing an embedded NUL survive whole (the C `"%s"` constructors stop at the first NUL; the full payload is restored through a plain property write). The caller stacks printed by `console.trace()`/`console.assert()`/`console.exception()` are built the same way (`JS_NewError()`), so a replaced `globalThis.Error` cannot corrupt console output either.
* **`newSymbol()` description:** QuickJSEngine follows ECMA-262 (`Symbol('x').description === 'x'`); V4's `newSymbol()` leaves the description undefined even though `String()` still shows the name.
* **`errorType()` prototype walk:** the class is found by walking the prototype chain and comparing pointer identity with the built-in error prototypes captured in the engine constructor (before any script runs). Rewriting `e.name`, replacing the global constructors after the fact, or subclassing (`class MyRE extends RangeError`) cannot change the answer — a subclass reports its base class. Divergence: `Object.setPrototypeOf(err, RangeError.prototype)` reclassifies the value as `RangeError` here, while V4 keeps the class the object was constructed with. A Proxy trap met while walking the chain clears the pending exception and reports `GenericError`. The walk stops after 16 prototype hops (a guard against prototype cycles); V4 walks to the end, so a class wired deeper than 16 links reads as `GenericError` here.
* **`errorType()` of a Proxy:** a `Proxy` around an Error is not itself an Error object (the check is by class, like V4's internal class check), so `errorType()` reports `NoError` for it.
* **`call()` default `this`:** an omitted thisObject binds the engine global object, even for strict callees (QJSEngine parity). With an explicit primitive thisObject, the primitive follows regular JS call semantics (a sloppy callee boxes it, a strict one keeps it). `callWithInstance()` instead binds the global object for non-object instances, exactly like QJSEngine's method of the same name — the two APIs intentionally mirror their Qt counterparts.
* **Module re-registration:** `registerModule()` with the same name twice replaces the module for new imports; the previous internal module record stays alive until the engine is destroyed (quickjs-ng keeps module defs in a runtime-owned list with no public free API). Re-registering in a loop grows that unreachable list — avoid unbounded re-registration cycles.

</details>

**Additional API capabilities**

Since QuickJSEngine was created for console applications, it includes performance and safety management functions that QJSEngine does not have:

* **Limits management:** `setExecutionTimeoutMs()` (wall-clock, per run), `setStepBudget()` (deterministic per-run budget of executed engine steps — the testing-friendly complement to the timeout: no timing, a run under the budget always completes, a runaway loop or catastrophic-backtracking regex is interrupted after at most the budget plus one poll interval), `setMemoryLimit()`, `setMaxStackSize()`. Protection against malicious scripts (DoS) and hangs. Defaults: the JS heap is capped at 64 MiB and the JS recursion budget is 512 KiB plus a calibrated C machinery allowance, clamped to the real bounds of the engine thread's stack; the timeout and the step budget are off by default. The measured thread-stack floor is cached per thread and is not invalidated on stack switches: hosts using fibers/coroutines should create the engine while running on the deepest stack the engine will ever execute on.
* **Bytecode cache management:** `setEvalCacheEnabled()`, `setEvalCacheLimit()`, `clearEvalCache()`. Fine-tuning of memory consumption for frequent `evaluate()` calls with dynamically generated strings.
* **Monitoring:** `version()` (the quickjs-ng version), `memoryUsage()` (memory statistics), `setGCThreshold()` (manual tuning of the garbage collector threshold).
* **Extended evaluate:** support for passing a starting line number, `evaluate(script, fileName, lineNumber)`, for precise integration of JS errors into C++ logs.
* **BigInt:** full `BigInt` support (conversion to `qint64`/`quint64` and back without precision loss for numbers above `2^53 - 1`).
* **QJSValue::engine():** removed back in Qt 5 and absent in Qt 6, but I needed it in a multi-script object to determine which script a function was launched from.

## Project structure

* **01_example** — a simple example of using QuickJSEngine and QJSEngine.
* **02_speed** — a benchmark comparing the speed of QuickJSEngine and QJSEngine.
* **03_tests** — tests verifying that QuickJSEngine behavior matches QJSEngine.
* **04_leakgate** — a long-run memory leak detector (a memory-growth regression gate over a repeated workload in one engine).
* **quickjs-ng** — the quickjs-ng core sources.
* **src** — the QuickJSEngine wrapper sources.

## AI tools

The entire wrapper was developed with AI models, because this task fits AI extremely well for the following reasons:
1. It is merely a wrapper around a ready-made existing JavaScript engine, not the engine itself.
2. The behavior of the wrapper with an alternative JavaScript engine can be compared against a reference existing engine, so discrepancies in behavior are found quickly.

Do not think that this wrapper was created with a simple "make me an analog of **QJSEngine**" prompt. In reality it was a rather long (~10 full days) multi-stage process of gradually building up functionality, regression testing, performance testing and tuning, code review, and so on, round and round many times. The code was written by the **GLM-5.2/5.3** model; code reviews were performed by **ChatGPT**, **Claude Sonnet**, **Qwen**, **Kimi**, **DeepSeek**, **GLM-5.2**. There were places in the code for which I made separate test programs to find out the real performance of different approaches. I also used the **Callgrind** tool to identify hot spots.

Even if you look down on AI code, at least take a look at the performance tests — there are some interesting moments to be seen there.

## License

The **QuickJSEngine** wrapper itself is distributed under the terms of [The Unlicense](https://unlicense.org/) (public domain dedication). You can use it in any projects without restrictions. The **quickjs-ng** core keeps its original MIT license.

---

## Performance tests

The benchmark suite (02_speed) is built around the embedding scenario: a C++ application running monitoring scripts, passing QObjects and QVariants across the bridge, re-evaluating scripts, creating and destroying engines. The weighted score therefore measures "host bridge + mixed scripting", not "faster JavaScript engine": in pure compute V4's JIT keeps the crown on several workloads (Math loop 1.20x, Object creation 2.03x, and decisively RegExp 14.5x), while the interpreter takes others (Fibonacci 0.74x, function calls 0.77x, JSON.parse 0.57x, string munging 0.48x). Performance also depends on the processor architecture: V4's JIT advantage grows on fast cores with large caches, while the interpreter scales more uniformly.

On Orange Pi Zero (not Zero 3) (ARMv7, 512 MB RAM) QJSEngine could not finish the test suite (measured on the earlier 24-case set): the OOM-killer terminates the process on the Promise test (the engine holds ~420 MB of memory that is not released until the engine is recreated). QuickJSEngine finished the whole suite with a peak consumption of 88 MB and zero memory retention after garbage collection.

```text
======================================================================================
AMD Ryzen 7 7700 8-Core Processor, 64-bit, Calculate Linux Desktop KDE, Qt 6.11.3
======================================================================================
Test                                       |      Qt V4 |    QuickJS |   Ratio | Weight | Winner  
--------------------------------------------------------------------------------------------------
Math loop                                  |    17.0 ns |    24.2 ns |   1.43x |     15 | Qt V4   
Function call overhead                     |    43.7 ns |    39.4 ns |   0.90x |      5 | QuickJS 
Fibonacci (fib 30)                         |  109.89 ms |   83.90 ms |   0.76x |      5 | QuickJS 
Object creation                            |    65.0 ns |    97.4 ns |   1.50x |     10 | Qt V4   
Array access                               |    73.8 ns |    64.4 ns |   0.87x |      5 | QuickJS 
Regex (log-line match)                     |    1.07 us |    1.33 us |   1.25x |      5 | Qt V4   
JSON.parse (small)                         |    1.44 us |   866.8 ns |   0.60x |      5 | QuickJS 
JSON.stringify (small)                     |    1.78 us |    1.77 us |   0.99x |      5 | QuickJS 
String ops (split+join)                    |    1.92 us |    1.14 us |   0.60x |      5 | QuickJS 
C++ -> JS call                             |    26.9 ns |    53.6 ns |   1.99x |     15 | Qt V4   
JS -> C++ method (hoisted)                 |   102.8 ns |    67.9 ns |   0.66x |     15 | QuickJS 
JS -> C++ method (inline)                  |   332.8 ns |    88.5 ns |   0.27x |     10 | QuickJS 
Wrapper cache (newQObject)                 |    18.0 ns |    14.4 ns |   0.80x |      5 | QuickJS 
Identity (getChild)                        |    46.9 ns |    64.2 ns |   1.37x |      5 | Qt V4   
Signal emit, no listeners                  |     6.6 ns |     6.6 ns |   1.00x |      2 | QuickJS 
Signal emit, 1 JS callback                 |    63.9 ns |    45.6 ns |   0.71x |      5 | QuickJS 
Signal emit, 3 JS callbacks                |   178.6 ns |   114.5 ns |   0.64x |      5 | QuickJS 
Promise microtasks                         |    2.29 us |    1.17 us |   0.51x |      5 | QuickJS 
QVariant: String JS->C++                   |    92.5 ns |    99.3 ns |   1.07x |      5 | Qt V4   
QVariant: Map JS->C++                      |   132.2 ns |   413.6 ns |   3.13x |      5 | Qt V4   
QVariant: List JS->C++                     |   172.2 ns |   285.9 ns |   1.66x |      5 | Qt V4   
QVariant: String C++->JS                   |    97.6 ns |    92.5 ns |   0.95x |      5 | QuickJS 
QVariant: Map C++->JS                      |   612.9 ns |   321.1 ns |   0.52x |      5 | QuickJS 
QVariant: List C++->JS                     |   439.8 ns |   155.4 ns |   0.35x |      5 | QuickJS 
QVariant: List 32 C++ -> JS                |    1.51 us |   541.9 ns |   0.36x |      5 | QuickJS 
QVariant: StringList 16 C++ -> JS (wrap)   |   298.4 ns |   341.2 ns |   1.14x |      0 | Qt V4   
QVariant: StringList 16 C++ -> JS consume  |    3.10 us |   921.0 ns |   0.30x |      5 | QuickJS 
QVariant: StringList 16 JS->C++            |   308.8 ns |    1.05 us |   3.39x |      5 | Qt V4   
Property cache                             |    60.2 ns |    51.6 ns |   0.86x |     10 | QuickJS 
Property write (int)                       |    61.1 ns |    49.7 ns |   0.81x |      5 | QuickJS 
Property write (UTF string)                |    82.8 ns |    75.0 ns |   0.91x |      5 | QuickJS 
Negative cache                             |    41.7 ns |    35.8 ns |   0.86x |      5 | QuickJS 
Eval warm (cache hit)                      |    6.35 us |   198.1 ns |   0.03x |      5 | QuickJS 
Eval cold (compile)                        |    6.34 us |    2.29 us |   0.36x |      5 | QuickJS 
Eval thrash (limit vs unique)              |    5.99 us |    6.46 us |   1.08x |      0 | Qt V4   
Wrapper churn (create/delete)              |    2.37 us |    2.37 us |   1.00x |      5 | QuickJS 
UTF property                               |    99.7 ns |    61.2 ns |   0.61x |      5 | QuickJS 
Heavy object (property reads)              |    61.0 ns |    51.0 ns |   0.84x |     10 | QuickJS 
Heavy object (method calls)                |    72.1 ns |    55.0 ns |   0.76x |     10 | QuickJS 
Engine ctor+dtor                           |  440.84 us |   69.91 us |   0.16x |      0 | QuickJS 
--------------------------------------------------------------------------------------
Relative score (out of 237):
  QJSEngine (Qt V4) : 108.16
  QQuickJSEngine    : 128.84
  (relative score requires both engines; single-engine run)
  (3 weight-0 row(s): informational, excluded from both aggregates)
  Winner: QQuickJSEngine (19.11% faster)

Throughput index (geometric mean of ops/sec, higher = faster):
  QJSEngine (Qt V4) : 3456173.89
  QQuickJSEngine    : 4535991.99
```

```text
==================================================================================================
Intel(R) Xeon(R) Processor, 64-bit, Debian GNU/Linux 13 (trixie), Qt 6.12.0
==================================================================================================
Test                                       |      Qt V4 |    QuickJS |   Ratio | Weight | Winner  
--------------------------------------------------------------------------------------------------
Math loop                                  |    21.5 ns |    25.5 ns |   1.18x |     15 | Qt V4   
Function call overhead                     |    65.0 ns |    50.0 ns |   0.77x |      5 | QuickJS 
Fibonacci (fib 30)                         |  154.76 ms |  114.99 ms |   0.74x |      5 | QuickJS 
Object creation                            |    85.4 ns |   170.1 ns |   1.99x |     10 | Qt V4   
Array access                               |   104.7 ns |    84.0 ns |   0.80x |      5 | QuickJS 
Regex (log-line match)                     |    1.30 us |    1.84 us |   1.42x |      5 | Qt V4   
JSON.parse (small)                         |    2.38 us |    1.29 us |   0.54x |      5 | QuickJS 
JSON.stringify (small)                     |    2.13 us |    2.18 us |   1.02x |      5 | Qt V4   
String ops (split+join)                    |    3.29 us |    1.56 us |   0.47x |      5 | QuickJS 
C++ -> JS call                             |    31.6 ns |    62.3 ns |   1.97x |     15 | Qt V4   
JS -> C++ method (hoisted)                 |   110.2 ns |    66.0 ns |   0.60x |     15 | QuickJS 
JS -> C++ method (inline)                  |   303.0 ns |    88.6 ns |   0.29x |     10 | QuickJS 
Wrapper cache (newQObject)                 |    26.1 ns |    37.6 ns |   1.44x |      5 | Qt V4   
Identity (getChild)                        |    58.5 ns |    72.5 ns |   1.24x |      5 | Qt V4   
Signal emit, no listeners                  |     6.7 ns |     6.8 ns |   1.01x |      2 | Qt V4   
Signal emit, 1 JS callback                 |    84.8 ns |    66.2 ns |   0.78x |      5 | QuickJS 
Signal emit, 3 JS callbacks                |   225.1 ns |   171.3 ns |   0.76x |      5 | QuickJS 
Promise microtasks                         |    2.41 us |    1.65 us |   0.68x |      5 | QuickJS 
QVariant: String JS->C++                   |   112.8 ns |   111.0 ns |   0.98x |      5 | QuickJS 
QVariant: Map JS->C++                      |   619.3 ns |   458.0 ns |   0.74x |      5 | QuickJS 
QVariant: List JS->C++                     |   683.2 ns |   261.8 ns |   0.38x |      5 | QuickJS 
QVariant: String C++->JS                   |   100.9 ns |    93.9 ns |   0.93x |      5 | QuickJS 
QVariant: Map C++->JS                      |   712.5 ns |   502.4 ns |   0.71x |      5 | QuickJS 
QVariant: List C++->JS                     |   474.9 ns |   185.8 ns |   0.39x |      5 | QuickJS 
QVariant: List 32 C++ -> JS                |    1.55 us |   578.5 ns |   0.37x |      5 | QuickJS 
QVariant: StringList 16 C++ -> JS (wrap)   |   326.1 ns |   661.3 ns |   2.03x |      0 | Qt V4   
QVariant: StringList 16 C++ -> JS consume  |    3.32 us |    1.38 us |   0.42x |      5 | QuickJS 
QVariant: StringList 16 JS->C++            |   611.9 ns |    1.74 us |   2.85x |      5 | Qt V4   
Property cache                             |    76.9 ns |    51.4 ns |   0.67x |     10 | QuickJS 
Property write (int)                       |    79.6 ns |    46.9 ns |   0.59x |      5 | QuickJS 
Property write (UTF string)                |   105.1 ns |    84.7 ns |   0.81x |      5 | QuickJS 
Negative cache                             |    65.1 ns |    39.9 ns |   0.61x |      5 | QuickJS 
Eval warm (cache hit)                      |    6.98 us |   247.9 ns |   0.04x |      5 | QuickJS 
Eval cold (compile)                        |    6.98 us |    2.65 us |   0.38x |      5 | QuickJS 
Eval thrash (limit vs unique)              |    6.98 us |    5.52 us |   0.79x |      0 | QuickJS 
Wrapper churn (create/delete)              |    2.40 us |    2.38 us |   0.99x |      5 | QuickJS 
UTF property                               |   122.4 ns |    77.4 ns |   0.63x |      5 | QuickJS 
Heavy object (property reads)              |    75.6 ns |    51.3 ns |   0.68x |     10 | QuickJS 
Heavy object (method calls)                |    85.9 ns |    52.6 ns |   0.61x |     10 | QuickJS 
Engine ctor+dtor                           |  408.98 us |  120.14 us |   0.29x |      0 | QuickJS 
--------------------------------------------------------------------------------------
Relative score (out of 237):
  QJSEngine (Qt V4) : 103.29
  QQuickJSEngine    : 133.71
  (relative score requires both engines; single-engine run)
  (3 weight-0 row(s): informational, excluded from both aggregates)
  Winner: QQuickJSEngine (29.46% faster)

Throughput index (geometric mean of ops/sec, higher = faster):
  QJSEngine (Qt V4) : 2604119.27
  QQuickJSEngine    : 3703728.22
```
