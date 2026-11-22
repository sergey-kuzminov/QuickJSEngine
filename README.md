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
* **Intentionally absent:** the V4 interop layer (`QJSManagedValue`, `QJSPrimitiveValue`, `coerceValue()`, `fromVariant()` — it mirrors the V4 memory model), engine-less `QJSValue(42)` constructors (every value is bound to its engine: thread-safety by design), `isVariant()` / `isUrl()` (deprecated in Qt 6.9 / V4-specific URL objects — QUrl converts to a string here), `uiLanguage()` (JS-driven language switching is out of scope for console applications).
* **Minor divergences:** `newSymbol()` follows ECMA-262 (the `description` is set; V4 leaves it undefined), `console.count()` keys counters by the printed label instead of by script and line, the `qsTr()` translation context is fixed, and `Object.keys()` invokes C++ getters (a throwing getter fails the enumeration), where QJSEngine never invokes getters during key enumeration.

<details>
<summary>Fine print: exact divergences</summary>

* **ES modules, exact list:** `importModule()` / `registerModule()` are implemented with QJSEngine semantics: a singleton cache keyed by the canonical file path, registered modules matched by the bare specifier, the registered value itself as the default export, a snapshot of enumerable own properties at registration time, `":/..."` Qt resource paths, and a module that threw at top level is not re-executed on the next import. Beyond that, each of the following is a SyntaxError in QJSEngine but works here: circular imports (allowed by the ECMAScript specification), `import.meta` (a plain object per the specification), top-level `await` (ES2022), dynamic `import()` inside `evaluate()` scripts (which can request registered modules too), registered names without a `.mjs` suffix, and a bare specifier that is not a registered module resolving as a file relative to the current working directory. All module-loading failures are `SyntaxError` objects like in QJSEngine, with our own message texts (a missing file is named in the message instead of V4's cryptic `Invalid null URL`). `importModule()` pumps the microtask queue by default (`setAutoProcessPromises(false)` disables it), so modules with a top-level `await` complete synchronously.
* **Error delivery:** exceptions of JS signal handlers are reported (the `uncaughtException` signal, or a `qWarning` when nobody listens) and the signal emission continues, exactly like QJSEngine; `Object.keys()` on a QObject whose C++ getter throws (via `throwError()`) fails with that error, where QJSEngine never invokes getters during key enumeration at all.
* **Lossy `toVariant()` refusals:** in the default (ConvertJSObjects) mode, Map/Set/WeakMap/WeakSet/Promise/RegExp/DataView yield an *invalid* QVariant, where QJSEngine yields an empty `QVariantMap` (Map/Set/Promise) or a `QRegularExpression` (RegExp). Symbols also yield an invalid QVariant, where QJSEngine yields the descriptive `"Symbol(name)"` string. This is deliberate: a string-shaped variant would silently coerce in metacall conversions (e.g. a nullptr written into a `QObject*` property) instead of throwing a TypeError. Use `toVariant(RetainJSObjects)` for a lossless `QVariant<QQuickJSValue>` wrap (function objects are retained in both modes, like in Qt 6.12).
* **`newSymbol()` description:** QuickJSEngine follows ECMA-262 (`Symbol('x').description === 'x'`); V4's `newSymbol()` leaves the description undefined even though `String()` still shows the name.
* **`console.count()` keying:** counters are keyed by the printed label (Node.js semantics), not by script and line number like V4 — quickjs-ng exposes no call-site information to C callbacks. The printed format (`label: N`) is identical.
* **`qsTr()` context:** the translation context is fixed (`"QQuickJSEngine"`); V4 derives it from the calling script's URL, which is not visible to C callbacks in quickjs-ng.
* **`exceptionStackTrace` frames:** parsed from the quickjs-ng `Error.stack` property into the V4 frame format `function:line:column:file`; native frames carry `-1/-1` and an empty file, like V4. A thrown non-Error value (e.g. `throw 'str'`) yields an empty list, since quickjs-ng attaches no stack to it.

</details>

**Additional API capabilities**

Since QuickJSEngine was created for console applications, it includes performance and safety management functions that QJSEngine does not have:

* **Limits management:** `setExecutionTimeoutMs()`, `setMemoryLimit()`, `setMaxStackSize()`. Protection against malicious scripts (DoS) and hangs.
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

Performance strongly depends on the processor architecture. V4's advantage comes from JIT compilation, which requires fast cores and large caches, whereas the QuickJSEngine interpreter scales equally with clock frequency on all architectures.

On Orange Pi Zero (not Zero 3) (ARMv7, 512 MB RAM) QJSEngine could not finish the test suite: the OOM-killer terminates the process on the Promise test (the engine holds ~420 MB of memory that is not released until the engine is recreated). QuickJSEngine finished all 24 tests with a peak consumption of 88 MB and zero memory retention after garbage collection.

```text
=====================================================================================
AMD Ryzen 7 7700 8-Core Processor, 64-bit, Calculate Linux Desktop KDE, Qt 6.11.2
=====================================================================================
Test                           |      Qt V4 |    QuickJS |   Ratio | Weight | Winner
-------------------------------------------------------------------------------------
Math loop                      |    17.8 ns |    26.0 ns |   1.46x |     15 | Qt V4
Function call overhead         |    43.4 ns |    43.3 ns |   1.00x |      5 | QuickJS
Fibonacci (fib 30)             |  106.90 ms |   89.48 ms |   0.84x |      5 | QuickJS
Object creation                |    65.7 ns |   104.5 ns |   1.59x |     10 | Qt V4
Array access                   |    75.3 ns |    68.4 ns |   0.91x |      5 | QuickJS
C++ -> JS call                 |    27.6 ns |    46.3 ns |   1.68x |     15 | Qt V4
JS -> C++ method               |    97.7 ns |    67.9 ns |   0.69x |     15 | QuickJS
Wrapper cache (newQObject)     |    18.5 ns |    14.5 ns |   0.78x |      5 | QuickJS
Identity (getChild)            |    47.1 ns |    62.1 ns |   1.32x |      5 | Qt V4
Signal emit, no listeners      |     6.3 ns |     6.3 ns |   1.00x |      2 | QuickJS
Signal emit, 1 JS callback     |    67.6 ns |    40.2 ns |   0.60x |      5 | QuickJS
Signal emit, 3 JS callbacks    |   190.3 ns |   100.0 ns |   0.53x |      5 | QuickJS
Promise microtasks             |    2.42 us |    1.18 us |   0.49x |      5 | QuickJS
QVariant: Map                  |   213.4 ns |   605.7 ns |   2.84x |      5 | Qt V4
QVariant: Map long keys        |   213.5 ns |   580.5 ns |   2.72x |      5 | Qt V4
QVariant: String               |   141.0 ns |   147.0 ns |   1.04x |     10 | Qt V4
QVariant: List                 |   371.4 ns |   412.9 ns |   1.11x |      5 | Qt V4
QVariant: List 32 C++ -> JS    |    1.49 us |   506.7 ns |   0.34x |      5 | QuickJS
QVariant: StringList 16 C++ -> |   303.8 ns |   307.0 ns |   1.01x |      5 | Qt V4
Method cache                   |    80.4 ns |    63.7 ns |   0.79x |     10 | QuickJS
Property cache                 |    61.7 ns |    52.5 ns |   0.85x |     10 | QuickJS
Negative cache                 |    45.4 ns |    40.2 ns |   0.89x |      5 | QuickJS
Eval cache (same scripts)      |    6.45 us |   178.0 ns |   0.03x |      5 | QuickJS
Wrapper churn (create/delete)  |    1.68 us |    1.72 us |   1.02x |      5 | Qt V4
UTF property                   |    97.8 ns |    66.9 ns |   0.68x |      5 | QuickJS
Heavy object (property reads)  |    60.5 ns |    52.5 ns |   0.87x |     10 | QuickJS
Heavy object (method calls)    |    69.9 ns |    54.8 ns |   0.78x |     10 | QuickJS
-------------------------------------------------------------------------------------
Relative score (out of 192):
  QJSEngine (Qt V4) : 92.99
  QQuickJSEngine    : 99.01
  (relative score requires both engines; single-engine run)
  Winner: QQuickJSEngine (6.48% faster)

Throughput index (geometric mean of ops/sec, higher = faster):
  QJSEngine (Qt V4) : 4920447.75
  QQuickJSEngine    : 5889233.38
```

```text
=====================================================================================
Intel(R) Core(TM) i3-7100 CPU @ 3.90GHz, 64-bit, Windows 10 Version 22H2, Qt 6.11.2
=====================================================================================
Test                           |      Qt V4 |    QuickJS |   Ratio | Weight | Winner
-------------------------------------------------------------------------------------
Math loop                      |    44.2 ns |    52.1 ns |   1.18x |     15 | Qt V4
Function call overhead         |   117.4 ns |    80.5 ns |   0.69x |      5 | QuickJS
Fibonacci (fib 30)             |  213.94 ms |  177.80 ms |   0.83x |      5 | QuickJS
Object creation                |    75.4 ns |   183.4 ns |   2.43x |     10 | Qt V4
Array access                   |   128.1 ns |   136.9 ns |   1.07x |      5 | Qt V4
C++ -> JS call                 |    69.1 ns |    80.8 ns |   1.17x |     15 | Qt V4
JS -> C++ method               |   227.1 ns |   126.6 ns |   0.56x |     15 | QuickJS
Wrapper cache (newQObject)     |    36.3 ns |    28.9 ns |   0.80x |      5 | QuickJS
Identity (getChild)            |    98.2 ns |    91.0 ns |   0.93x |      5 | QuickJS
Signal emit, no listeners      |    15.7 ns |    14.2 ns |   0.90x |      2 | QuickJS
Signal emit, 1 JS callback     |   121.4 ns |    80.4 ns |   0.66x |      5 | QuickJS
Signal emit, 3 JS callbacks    |   318.6 ns |   194.7 ns |   0.61x |      5 | QuickJS
Promise microtasks             |    2.12 us |    1.55 us |   0.73x |      5 | QuickJS
QVariant: Map                  |   417.6 ns |    1.16 us |   2.78x |      5 | Qt V4
QVariant: Map long keys        |   407.2 ns |    1.17 us |   2.88x |      5 | Qt V4
QVariant: String               |   286.2 ns |   345.0 ns |   1.21x |     10 | Qt V4
QVariant: List                 |   588.7 ns |   806.2 ns |   1.37x |      5 | Qt V4
QVariant: List 32 C++ -> JS    |    1.73 us |    1.20 us |   0.70x |      5 | QuickJS
QVariant: StringList 16 C++ -> |   390.0 ns |   672.3 ns |   1.72x |      5 | Qt V4
Method cache                   |   219.7 ns |   119.9 ns |   0.55x |     10 | QuickJS
Property cache                 |   157.4 ns |   125.8 ns |   0.80x |     10 | QuickJS
Negative cache                 |   117.9 ns |    81.5 ns |   0.69x |      5 | QuickJS
Eval cache (same scripts)      |   14.15 us |   337.0 ns |   0.02x |      5 | QuickJS
Wrapper churn (create/delete)  |    3.53 us |    4.01 us |   1.14x |      5 | Qt V4
UTF property                   |   215.2 ns |   148.6 ns |   0.69x |      5 | QuickJS
Heavy object (property reads)  |   161.6 ns |   123.3 ns |   0.76x |     10 | QuickJS
Heavy object (method calls)    |   195.0 ns |   100.6 ns |   0.52x |     10 | QuickJS
-------------------------------------------------------------------------------------
Relative score (out of 192):
  QJSEngine (Qt V4) : 90.64
  QQuickJSEngine    : 101.36
  (relative score requires both engines; single-engine run)
  Winner: QQuickJSEngine (11.83% faster)

Throughput index (geometric mean of ops/sec, higher = faster):
  QJSEngine (Qt V4) : 2487938.68
  QQuickJSEngine    : 2976681.45
```

```text
=====================================================================================
arm64, 64-bit, Buildroot 2026.11-git, Qt 6.11.3 (Orange Pi Zero 3)
=====================================================================================
Test                           |      Qt V4 |    QuickJS |   Ratio | Weight | Winner
-------------------------------------------------------------------------------------
Math loop                      |   223.3 ns |   284.8 ns |   1.28x |     15 | Qt V4
Function call overhead         |   739.8 ns |   500.5 ns |   0.68x |      5 | QuickJS
Fibonacci (fib 30)             |     1.61 s |     1.07 s |   0.66x |      5 | QuickJS
Object creation                |   762.6 ns |    1.54 us |   2.01x |     10 | Qt V4
Array access                   |    1.01 us |   902.4 ns |   0.90x |      5 | QuickJS
C++ -> JS call                 |   513.1 ns |   573.8 ns |   1.12x |     15 | Qt V4
JS -> C++ method               |    1.50 us |   885.0 ns |   0.59x |     15 | QuickJS
Wrapper cache (newQObject)     |   320.0 ns |   231.6 ns |   0.72x |      5 | QuickJS
Identity (getChild)            |   770.1 ns |   725.4 ns |   0.94x |      5 | QuickJS
Signal emit, no listeners      |   121.5 ns |   121.7 ns |   1.00x |      2 | Qt V4
Signal emit, 1 JS callback     |    1.25 us |   673.2 ns |   0.54x |      5 | QuickJS
Signal emit, 3 JS callbacks    |    3.25 us |    1.57 us |   0.48x |      5 | QuickJS
Promise microtasks             |   17.38 us |   14.81 us |   0.85x |      5 | QuickJS
QVariant: Map                  |    3.53 us |    8.92 us |   2.53x |      5 | Qt V4
QVariant: Map long keys        |    3.52 us |    9.25 us |   2.63x |      5 | Qt V4
QVariant: String               |    2.41 us |    2.34 us |   0.97x |     10 | QuickJS
QVariant: List                 |    5.51 us |    5.67 us |   1.03x |      5 | Qt V4
QVariant: List 32 C++ -> JS    |   19.20 us |    7.14 us |   0.37x |      5 | QuickJS
QVariant: StringList 16 C++ -> |    4.03 us |    5.22 us |   1.29x |      5 | Qt V4
Method cache                   |    1.44 us |   832.4 ns |   0.58x |     10 | QuickJS
Property cache                 |    1.25 us |   711.3 ns |   0.57x |     10 | QuickJS
Negative cache                 |    1.06 us |   607.6 ns |   0.57x |      5 | QuickJS
Eval cache (same scripts)      |  110.82 us |    3.00 us |   0.03x |      5 | QuickJS
Wrapper churn (create/delete)  |   21.48 us |   21.48 us |   1.00x |      5 | QuickJS
UTF property                   |    1.80 us |    1.03 us |   0.57x |      5 | QuickJS
Heavy object (property reads)  |    1.26 us |   707.5 ns |   0.56x |     10 | QuickJS
Heavy object (method calls)    |    1.23 us |   754.9 ns |   0.61x |     10 | QuickJS
-------------------------------------------------------------------------------------
Relative score (out of 192):
  QJSEngine (Qt V4) : 85.94
  QQuickJSEngine    : 106.06
  (relative score requires both engines; single-engine run)
  Winner: QQuickJSEngine (23.41% faster)

Throughput index (geometric mean of ops/sec, higher = faster):
  QJSEngine (Qt V4) : 309962.53
  QQuickJSEngine    : 417176.62
```
