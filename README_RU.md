# QuickJSEngine

[English](README.md) | **Русский**

**QuickJSEngine** — это легковесный JavaScript-движок для Qt, задуманный как аналог **QJSEngine** для консольных приложений или приложений на основе виджетов (QtWidgets), в которых не используется библиотека QtQml. Проект представляет собой удобную C++ обертку над современным движком [quickjs-ng](https://github.com/quickjs-ng/quickjs).

## Зачем нужен JavaScript в C++ коде?

Лично я использую JavaScript для добавления автоматизации в свои приложения. Пользователь может самостоятельно написать скрипт, который добавляет реакцию в программу на какое либо событие (я пишу программы мониторинга).

Когда-то для этого я использовал модуль **QtScript**, который основан на JavaScriptCore (движок WebKit), который давно не обновляется. Ему на смену пришел **QJSEngine** (движок V4), однако **QJSEngine** является неотъемлемой частью модуля QtQml. Использование **QJSEngine** тянет за собой зависимости QML, что избыточно для чисто консольных или виджет-приложений. **QuickJSEngine** решает эту проблему, предлагая независимый, быстрый и современный JavaScript движок.

## Основа: quickjs-ng

В качестве ядра используется **quickjs-ng** (ng - next generation) — форк оригинального QuickJS от Фабриса Беллара (Fabrice Bellard) и Чарли Гордона (Charlie Gordon), который активно развивается сообществом.
* Поддерживает современные стандарты JavaScript (ES2023+).
* Относительно компактный и очень быстрый интерпретатор.
* Движок **quickjs-ng** распространяется под лицензией [MIT](https://opensource.org/license/mit).

## Философия и отличия от QJSEngine

**QuickJSEngine** — это *легковесный* аналог, он **не стремится** реализовать абсолютно весь функционал **QJSEngine**, так как это несовместимо с концепцией легковесности. Весь функционал **QJSEngine** и не требуется, так как проект нацелен на консольные приложения, а не на графику в QML. Но все же движок стремится быть максимально близким к API **QJSEngine** для легкого перевода существующего кода.

**Что не реализовано (или реализовано иначе) по сравнению с QJSEngine:**
* **Политика владения объектами:** в **QJSEngine** GC (сборщик мусора) может удалить C++ объект. В QuickJSEngine C++ объекты никогда не удаляются сборщиком мусора JS: время жизни контролируется исключительно C++ кодом (через `parent` или `delete`), что безопаснее.
* **Q_GADGET:** передача структур Q_GADGET по значению не поддерживается. Используйте `QObject*` или `QVariantMap`.
* **Lossy `toVariant()`:** в режиме по умолчанию JS-специфичные типы (Map/Set/WeakMap/WeakSet/Promise/RegExp/DataView) и символы дают *невалидный* QVariant, тогда как QJSEngine даёт пустой `QVariantMap`, `QRegularExpression` или описательную строку. Для лосслесс-обёртки используйте `toVariant(RetainJSObjects)` в `QVariant<QQuickJSValue>`.
* **ES-модули:** `importModule()` / `registerModule()` следуют семантике QJSEngine; там, где QJSEngine отказывает, побеждает спецификация ECMAScript — циклические импорты, `import.meta`, top-level `await` и динамический `import()` работают (каждый из них — SyntaxError в QJSEngine).
* **Намеренно отсутствуют:** межевой слой V4 (`QJSManagedValue`, `QJSPrimitiveValue`, `coerceValue()`, `fromVariant()` — он повторяет модель памяти V4), конструкторы вида `QJSValue(42)` без движка (каждое значение привязано к своему движку — потокобезопасность по дизайну), `isVariant()` / `isUrl()` (deprecated с Qt 6.9 / V4-специфичные URL-объекты — здесь QUrl конвертируется в строку), `uiLanguage()` (JS-переключение языка вне интересов консольных приложений).
* **Мелкие расхождения:** `newSymbol()` следует ECMA-262 (`description` установлен; V4 оставляет его undefined), `console.count()` ключует счётчики напечатанной меткой, а не скриптом и строкой, контекст перевода `qsTr()` фиксирован, а `Object.keys()` вызывает C++ геттеры (бросающий геттер валит перечисление), тогда как QJSEngine вообще не вызывает геттеры при перечислении ключей.

<details>
<summary>Подробности: точные расхождения</summary>

* **ES-модули, точный список:** `importModule()` / `registerModule()` реализованы с семантикой QJSEngine: singleton-кэш по каноническому пути, зарегистрированные модули матчатся голым спесификатором, default-экспорт = само зарегистрированное значение, снапшот enumerable own-свойств в момент регистрации, пути Qt-ресурсов `":/..."`, модуль, бросивший на верхнем уровне, при повторном импорте не исполняется заново. Сверх этого, каждое из перечисленного — SyntaxError в QJSEngine, но работает здесь: циклические импорты (разрешены по спецификации ECMAScript), `import.meta` (обычный объект по спецификации), top-level `await` (ES2022), динамический `import()` внутри скриптов `evaluate()` (и может запрашивать зарегистрированные модули), зарегистрированные имена без суффикса `.mjs`, голый спесификатор, не являющийся зарегистрированным модулем, — как файл относительно текущего рабочего каталога. Все ошибки загрузки модулей — объекты `SyntaxError`, как в QJSEngine, с нашими текстами сообщений (отсутствующий файл называется в сообщении вместо криптичного `Invalid null URL` у V4). `importModule()` по умолчанию прокручивает очередь микротасков (`setAutoProcessPromises(false)` отключает), поэтому модули с top-level `await` завершаются синхронно.
* **Доставка ошибок:** исключения JS-обработчиков сигналов репортятся (сигналом `uncaughtException` или `qWarning` без подписчиков), а эмиссия сигнала продолжается — как в QJSEngine; `Object.keys()` над QObject, чей C++ геттер бросает (через `throwError()`), падает с этой ошибкой, тогда как QJSEngine вообще не вызывает геттеры при перечислении ключей.
* **Отказы в lossy `toVariant()`:** в режиме по умолчанию (ConvertJSObjects) Map/Set/WeakMap/WeakSet/Promise/RegExp/DataView дают *невалидный* QVariant, тогда как QJSEngine даёт пустой `QVariantMap` (Map/Set/Promise) или `QRegularExpression` (RegExp). Символы тоже дают невалидный QVariant, тогда как QJSEngine даёт описательную строку `"Symbol(name)"`. Это осознанно: строкоподобный вариант молча бы конвертировался в metacall-конверсиях (например, nullptr записался бы в свойство `QObject*`) вместо бросания TypeError. Для лосслесс-обёртки используйте `toVariant(RetainJSObjects)` в `QVariant<QQuickJSValue>` (функции удерживаются в обоих режимах, как в Qt 6.12).
* **`newSymbol()` и description:** QuickJSEngine следует ECMA-262 (`Symbol('x').description === 'x'`); у V4 `newSymbol()` оставляет description неопределённым, хотя `String()` по-прежнему печатает имя.
* **Ключи `console.count()`:** счётчики ключуются напечатанной меткой (семантика Node.js), а не скриптом и строкой, как в V4 — quickjs-ng не даёт C-коллбэкам информации о точке вызова. Печатаемый формат (`метка: N`) идентичен.
* **Контекст `qsTr()`:** контекст перевода фиксирован (`"QQuickJSEngine"`); V4 выводит его из URL вызывающего скрипта, что недоступно C-коллбэкам quickjs-ng.
* **Кадры `exceptionStackTrace`:** разбираются из свойства `Error.stack` quickjs-ng в формат кадров V4 `функция:строка:столбец:файл`; нативные кадры несут `-1/-1` и пустой файл, как в V4. Брошенное не-Error значение (например, `throw 'str'`) даёт пустой список — quickjs-ng не прикрепляет к нему стек.

</details>

**Дополнительные возможности API**

Поскольку QuickJSEngine создавался для консольных приложений, в него были добавлены функции управления производительностью и безопасностью, которых нет в QJSEngine:

* **Управление лимитами:** `setExecutionTimeoutMs()`, `setMemoryLimit()`, `setMaxStackSize()`. Позволяют защититься от зловредных скриптов (DoS) и зависаний.
* **Управление кэшем байткода:** `setEvalCacheEnabled()`, `setEvalCacheLimit()`, `clearEvalCache()`. Позволяют тонко настроить потребление памяти при частых вызовах `evaluate()` с динамически генерируемыми строками.
* **Мониторинг:** `version()` (версия quickjs-ng), `memoryUsage()` (статистика памяти), `setGCThreshold()` (ручная настройка порога сборщика мусора).
* **Расширенный evaluate:** Поддержка передачи начального номера строки `evaluate(script, fileName, lineNumber)` для точной интеграции ошибок JS в логи C++.
* **BigInt:** Полная поддержка `BigInt` (конвертация в `qint64`/`quint64` и обратно без потери точности для чисел больше `2^53 - 1`).
* **QJSValue::engine():** Удалён ещё в Qt 5 и отсутствует в Qt 6, но он мне понадобился в многоскриптовом объекте для определения из какого скрипта запущена функция.

## Структура проекта

* **01_example** — простой пример использования QuickJSEngine и QJSEngine.
* **02_speed** — бенчмарк для сравнения скорости QuickJSEngine и QJSEngine.
* **03_tests** — тесты для проверки соответствия поведения QuickJSEngine и QJSEngine.
* **04_leakgate** — детектор утечек памяти в длительных прогонах (гейт регрессии роста памяти при многократном повторении рабочей нагрузки в одном движке).
* **quickjs-ng** — исходные коды ядра quickjs-ng.
* **src** — исходные коды обертки QuickJSEngine.

## ИИ инструменты

Весь код обертки разработан при помощи ИИ моделей, т.к. эта задача очень хорошо подходит для ИИ по следующим причинам:
1. Это всего лишь обертка к готовому существующему JavaScript движку, а не сам движок.
2. Результат работы обертки и альтернативного JavaScript движка можно сравнить с эталонным существующим движком и так быстро найти расхождения в работе.

Не думайте, что эта обертка была создана простым промптом типа сделай аналог **QJSEngine**. На самом деле это был достаточно длительный (полных ~10 дней) многоэтапный процесс поэтапного наращивания функциональности, тестирования регрессий, тестирования и улучшения производительности, code review и так по кругу много раз. Код писала модель **GLM-5.2/5.3**, code review проводили модели **ChatGPT**, **Claude Sonnet**, **Qwen**, **Kimi**, **DeepSeek**, **GLM-5.2**. Были места в коде, для которых я делал отдельные тестовые программы для выяснения реальной производительности разных подходов. Также я пользовался инструментом **Callgrind** для выявления горячих мест. 

Даже если вы пренебрежительно относитесь к ИИ коду, посмотрите хотя бы тесты производительности, там можно увидеть интересные моменты.

## Лицензия

Сама обертка **QuickJSEngine** распространяется на условиях лицензии [The Unlicense](https://unlicense.org/) (передача в общественное достояние). Вы можете использовать её в любых проектах без ограничений. Ядро **quickjs-ng** сохраняет свою оригинальную лицензию MIT.

---

## Тесты производительности

Производительность сильно зависит от архитектуры процессора. Выигрыш V4 обеспечивается JIT-компиляцией, которая требует быстрых ядер и больших кэшей, тогда как интерпретатор QuickJSEngine масштабируется по частоте одинаково на всех архитектурах.

На Orange Pi Zero (не Zero 3) (ARMv7, 512 МБ ОЗУ) QJSEngine не смог завершить набор тестов: OOM-killer завершает процесс на тесте Promise (движок удерживает ~420 МБ памяти, не освобождаемой до пересоздания движка). QuickJSEngine завершил все 24 теста с пиковым потреблением 88 МБ и нулевым удержанием памяти после сборщика мусора.

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
