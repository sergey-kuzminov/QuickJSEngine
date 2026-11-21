#include <QCoreApplication>
#include <QJSEngine>
#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDate>
#include <QTime>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>

#include "QuickJSEngine.h"
#include "JsErrorTranslator.h"

// ------------------------------------------------------------------------------------------------
// Test C++ Classes
// ------------------------------------------------------------------------------------------------
struct UnregisteredType {
    char payload[64] = {};
    int marker = 123456789;
};

class TestObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(int value READ getValue WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(int readOnlyProp READ getReadOnlyProp CONSTANT)
    Q_PROPERTY(QVariant variantProp READ getVariantProp WRITE setVariantProp)
    Q_PROPERTY(QDate dateProp READ getDateProp WRITE setDateProp)
    Q_PROPERTY(qint64 bigProp READ getBigProp WRITE setBigProp)
    Q_PROPERTY(UnregisteredType uProp READ getUProp)
    Q_PROPERTY(UnregisteredType uPropW WRITE setUPropW)

signals:
    void valueChanged(int newValue);
    void nameChanged(const QString& newName);
    void multiArgSignal(int a, const QString& b, double c);

private slots:
    void secretSlot() { ++m_secretCalls; }

public:
    explicit TestObject(QObject* p = nullptr) : QObject(p), m_child(nullptr) {
        if (!p) m_child = new TestObject(this);
    }

    Q_INVOKABLE int add(int a, int b) { return a + b; }
    Q_INVOKABLE QString concat(const QString &a, const QString &b) { return a + b; }
    Q_INVOKABLE QVariant echoVariant(QVariant v) { return v; }
    Q_INVOKABLE QByteArray echoBytes(const QByteArray &b) { return b; }
    Q_INVOKABLE QQuickJSValue echoJsValue(QQuickJSValue v) { return v; }
    Q_INVOKABLE void throwCxxException() { throw std::runtime_error("C++ Exception in TestObject"); }
    Q_INVOKABLE QString getName() const { return m_name; }
    Q_INVOKABLE qint64 echoBigInt(qint64 v) { return v; }
    Q_INVOKABLE void emitMultiArg() { emit multiArgSignal(42, QStringLiteral("test"), 3.14); }
    Q_INVOKABLE void throwCxxExceptionInSignal() { throw std::runtime_error("C++ Signal Exception"); }
    Q_INVOKABLE void throwUnknownException() { throw 42; }   // not a std::exception -> uncatchable InternalError policy

    Q_INVOKABLE QStringList manyArgs(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10) {
        QStringList l;
        l << QString::number(a1) << QString::number(a2) << QString::number(a3) << QString::number(a4) << QString::number(a5)
          << QString::number(a6) << QString::number(a7) << QString::number(a8) << QString::number(a9) << QString::number(a10);
        return l;
    }

    int getValue() const { return m_value; }
    void setValue(int v) { m_value = v; emit valueChanged(v); }
    void setName(const QString& n) { m_name = n; emit nameChanged(n); }
    int getReadOnlyProp() const { return 999; }

    QVariant getVariantProp() const { return m_variantProp; }
    void setVariantProp(const QVariant& v) { m_variantProp = v; }
    QDate getDateProp() const { return m_dateProp; }
    void setDateProp(const QDate& d) { m_dateProp = d; }

    qint64 getBigProp() const { return m_bigProp; }
    void setBigProp(qint64 v) { m_bigProp = v; }

    Q_INVOKABLE TestObject* getChild() const { return m_child; }
    Q_INVOKABLE TestObject* getNull() const { return nullptr; }
    Q_INVOKABLE TestObject* getParent() const { return qobject_cast<TestObject*>(parent()); }

    Q_INVOKABLE QList<int> getIntList() const { return QList<int>{1, 2, 3, 4, 5}; }
    Q_INVOKABLE QList<quint16> getUInt16List() const { return QList<quint16>{10, 20, 30}; }
    Q_INVOKABLE QVariantHash getVariantHash() const { return {{"name", "Test"}, {"value", 42}}; }
    Q_INVOKABLE QHash<QString, int> getIntHash() const { return {{"a", 1}, {"b", 2}}; }

    enum Status { Inactive = 0, Active = 1, Processing = 2 };
    Q_ENUM(Status)
    Q_INVOKABLE Status getStatus() const { return m_status; }
    Q_INVOKABLE void setStatus(Status s) { m_status = s; }
    Q_INVOKABLE Status processStatus(Status s) const { return static_cast<Status>(static_cast<int>(s) + 1); }

    Q_INVOKABLE int secretCalls() const { return m_secretCalls; }
    Q_INVOKABLE int unregArgs() const { return m_unregArgs; }
    void setJsEngine(QQuickJSEngine* e) { m_jsEngine = e; }

    Q_INVOKABLE int selfInterruptThenEval() {
        if (!m_jsEngine) return -1;
        m_jsEngine->setInterrupted(true);
        QQuickJSValue r = m_jsEngine->evaluate(QStringLiteral("1+1"));
        return r.isError() ? -2 : r.toInt();
    }

    Q_INVOKABLE void takeUnregistered(UnregisteredType t) { Q_UNUSED(t); ++m_unregArgs; }
    Q_INVOKABLE UnregisteredType makeUnregistered() { return UnregisteredType(); }
    UnregisteredType getUProp() const { return UnregisteredType(); }
    void setUPropW(const UnregisteredType& t) { Q_UNUSED(t); }

private:
    int m_value = 0;
    QString m_name = QStringLiteral("Default");
    Status m_status = Active;
    TestObject* m_child;
    QVariant m_variantProp;
    QDate m_dateProp;
    qint64 m_bigProp = 0;
    int m_secretCalls = 0;
    int m_unregArgs = 0;
    QQuickJSEngine* m_jsEngine = nullptr;
};
Q_DECLARE_METATYPE(TestObject*)

class DerivedTestObject : public TestObject {
    Q_OBJECT
public:
    explicit DerivedTestObject(QObject* p = nullptr) : TestObject(p) {}
    Q_INVOKABLE int derivedMethod() { return 99; }
};

// ------------------------------------------------------------------------------------------------
// Watchdog for timeout/interrupt tests to prevent infinite loops on unpatched code
// ------------------------------------------------------------------------------------------------
class InterruptWatchdog : public QThread {
public:
    QQuickJSEngine* engine = nullptr;
    int delayMs = 2500;
    std::atomic<bool> stopFlag{false};

    void run() override {
        QElapsedTimer t; t.start();
        // Poll stopFlag every 50 ms
        while (t.elapsed() < delayMs && !stopFlag.load()) {
            QThread::msleep(50);
        }
        if (!stopFlag.load() && engine) {
            engine->setInterrupted(true);
        }
    }
};

// ------------------------------------------------------------------------------------------------
// Helper object for regression tests that control the engine or emit signals from C++
// ------------------------------------------------------------------------------------------------
// Deliberately only forward-declared and never registered: with Qt 6.12 a
// by-value parameter type is lazily registered by QMetaMethod::parameterType(),
// so a forward-declared pointer type is the real "unregistered" case.
class UnregisteredForward;

class RegressionTestObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(int intProp READ intProp WRITE setIntProp)
    Q_PROPERTY(bool boolProp READ boolProp WRITE setBoolProp)
    Q_PROPERTY(double doubleProp READ doubleProp WRITE setDoubleProp)
    Q_PROPERTY(QObject* objProp READ objProp WRITE setObjProp)
public:
    explicit RegressionTestObject(QObject* p = nullptr) : QObject(p), m_obj(this) {}

    int intProp() const { return m_int; }
    void setIntProp(int v) { m_int = v; }
    bool boolProp() const { return m_bool; }
    void setBoolProp(bool v) { m_bool = v; }
    double doubleProp() const { return m_double; }
    void setDoubleProp(double v) { m_double = v; }
    QObject* objProp() const { return m_obj; }
    void setObjProp(QObject* v) { m_obj = v; }

    // Enables the execution timeout while a script is running
    Q_INVOKABLE void enableTimeout(int ms) { if (m_jsEngine) m_jsEngine->setExecutionTimeoutMs(static_cast<quint32>(ms)); }
    // Directly emits a signal with an unregistered parameter type
    Q_INVOKABLE void emitUnreg() { emit unregSignal(nullptr); }

    void setJsEngine(QQuickJSEngine* e) { m_jsEngine = e; }

signals:
    void unregSignal(UnregisteredForward* t);
    void pingSignal();

private:
    int m_int = -1;
    bool m_bool = false;
    double m_double = -1.0;
    QObject* m_obj = nullptr;   // initialized to `this`: distinguishes a silent nullptr write
    QQuickJSEngine* m_jsEngine = nullptr;
};

// ------------------------------------------------------------------------------------------------
// Test Helpers
// ------------------------------------------------------------------------------------------------
int passed = 0, failed = 0, warnings = 0;

static QStringList g_capturedWarnings;
static void captureWarningHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (type == QtWarningMsg) g_capturedWarnings.append(msg);
}

// RAII guard: routes qWarning output into g_capturedWarnings while in scope and
// always restores the previous handler (safe against early returns/exceptions)
class WarningCaptureGuard {
public:
    WarningCaptureGuard() : m_oldHandler(qInstallMessageHandler(captureWarningHandler)) { g_capturedWarnings.clear(); }
    ~WarningCaptureGuard() { qInstallMessageHandler(m_oldHandler); }
    WarningCaptureGuard(const WarningCaptureGuard&) = delete;
    WarningCaptureGuard& operator=(const WarningCaptureGuard&) = delete;
private:
    QtMessageHandler m_oldHandler;
};

#define TEST_RESULT(name, cond, details) do { \
if (cond) { qInfo().noquote() << name << QStringLiteral("- PASSED"); passed++; } \
    else { qCritical().noquote() << name << QStringLiteral("- FAILED:") << details; failed++; } \
} while(0)

#define PRINT_ERR_IF_ANY(val, testName) do { \
    if ((val).isError()) { \
            qWarning().noquote() << QStringLiteral(">>> WARNING: Unexpected error in test") << testName << QStringLiteral(":\n") << (val).toString(); \
            warnings++; \
    } \
} while(0)

    void compareEngines(const char* name, const char* code) {
        QJSEngine qtEng;
        QQuickJSEngine qEng;
        QJSValue qtRes = qtEng.evaluate(QString::fromUtf8(code));
        QQuickJSValue qRes = qEng.evaluate(QString::fromUtf8(code));

        bool qtErr = qtRes.isError();
        bool qErr = qRes.isError();

        if (qtErr && qErr) {
            // Both engines failed: keep the diagnostics line (without the pass marker,
            // so log-based test counting stays one line per test) and additionally
            // require the same error class (e.g. TypeError)
            qInfo().noquote() << name << QStringLiteral("(both engines threw an error)")
                              << QStringLiteral("\n  [QJS Error]:") << qRes.toString()
                              << QStringLiteral("\n  [Qt Error]:") << qtRes.toString();
            TEST_RESULT(name,
                        qtRes.property(QStringLiteral("name")).toString() == qRes.property(QStringLiteral("name")).toString(),
                        QStringLiteral("error class mismatch: QJSEngine='%1', QuickJS='%2'")
                            .arg(qtRes.property(QStringLiteral("name")).toString()).arg(qRes.property(QStringLiteral("name")).toString()));
        } else if (qtErr != qErr) {
            QString d = QString::fromUtf8("QJSEngine err=%1 (%2), QuickJS err=%3 (%4)")
            .arg(qtErr).arg(qtRes.toString()).arg(qErr).arg(qRes.toString());
            TEST_RESULT(name, false, d);
        } else {
            TEST_RESULT(name, qtRes.toString() == qRes.toString(), QString::fromUtf8("QJSEngine='%1', QuickJS='%2'").arg(qtRes.toString()).arg(qRes.toString()));
        }
    }

void compareQObjectEngines(const char* name, TestObject* obj, const char* code) {
    QJSEngine qtEng;
    QQuickJSEngine qEng;
    QJSEngine::setObjectOwnership(obj, QJSEngine::CppOwnership);

    qtEng.globalObject().setProperty(QStringLiteral("testObj"), qtEng.newQObject(obj));
    qEng.globalObject().setProperty(QStringLiteral("testObj"), qEng.newQObject(obj));

    QJSValue qtRes = qtEng.evaluate(QString::fromUtf8(code));
    QQuickJSValue qRes = qEng.evaluate(QString::fromUtf8(code));

    bool qtErr = qtRes.isError();
    bool qErr = qRes.isError();

    if (qtErr && qErr) {
        // Both engines failed: additionally require the same error class (e.g. TypeError)
        TEST_RESULT(name,
                    qtRes.property(QStringLiteral("name")).toString() == qRes.property(QStringLiteral("name")).toString(),
                    QStringLiteral("error class mismatch: QJSEngine='%1', QuickJS='%2'")
                        .arg(qtRes.property(QStringLiteral("name")).toString()).arg(qRes.property(QStringLiteral("name")).toString()));
    } else if (qtErr != qErr) {
        TEST_RESULT(name, false, QString::fromUtf8("QJSEngine err=%1, QuickJS err=%2").arg(qtErr).arg(qErr));
    } else {
        TEST_RESULT(name, qtRes.toString() == qRes.toString(), QString::fromUtf8("QJSEngine='%1', QuickJS='%2'").arg(qtRes.toString()).arg(qRes.toString()));
    }
}

// Runs the full test suite
static void runAllTests(const bool runDangerous) {
    // 1. Core JS Types & Equality
    compareEngines("Type: undefined", "String(undefined)");
    compareEngines("Type: null", "String(null)");
    compareEngines("Undefined: typeof", "typeof undefined");
    compareEngines("Undefined: == null", "undefined == null");
    compareEngines("Undefined: === null", "undefined === null");
    compareEngines("Null: typeof", "typeof null");
    compareEngines("NaN: == ", "NaN == NaN");
    compareEngines("NaN: Object.is", "Object.is(NaN, NaN)");
    compareEngines("Infinity: 1/0", "1/0");
    compareEngines("Infinity: -1/0", "-1/0");
    compareEngines("Infinity: +1", "Infinity+1");
    compareEngines("Prototypes: hasOwnProperty", "({a:1}).hasOwnProperty('a')");
    compareEngines("Symbol", "typeof Symbol() === 'symbol'");

    // 2. Strings & UTF-8
    compareEngines("UTF8: Cyrillic concat", "'Привет' + ' ' + 'Мир'");
    compareEngines("UTF8: Emoji substring", "'😀😂'.substring(0, 2)");
    compareEngines("UTF8: Chinese JSON", "JSON.parse('{\"x\":\"你好\"}').x");
    compareEngines("Big Data: 100k Array", "let a=[]; for(let i=0;i<100000;i++) a.push(i); a.length");
    compareEngines("Big Data: 100 Levels", "let o={}; let c=o; for(let i=0;i<100;i++) { c.x={}; c=c.x; } c.depth=100; o.x.x.x.x.x.depth;");

    // 3. Numbers & Arrays
    compareEngines("Number: INT32_MAX", "2147483647");
    compareEngines("Number: INT32_MIN", "-2147483648");
    compareEngines("Number: MAX_SAFE_INTEGER", "9007199254740991");
    compareEngines("ArrayBuffer: Uint8Array slice", "let u = new Uint8Array([1,2,3,4]); let s = u.slice(1, 3); s[1];");
    compareEngines("ArrayBuffer: subarray", "let u = new Uint8Array([1,2,3,4]); u.subarray(0,2).length;");

    // 4. Exceptions & Recovery
    compareEngines("Exception: TypeError", "null.property");
    {
        QQuickJSEngine qEng;
        qEng.evaluate(QStringLiteral("throw 123;"));
        TEST_RESULT("Exception: Recovery", qEng.evaluate(QStringLiteral("1 + 1")).toInt() == 2, "Engine broken after exception");

        qEng.evaluate(QStringLiteral("throw new Error('Sticky Test');"));
        TEST_RESULT("Recovery: No Sticky Exception", !qEng.evaluate(QStringLiteral("1+1")).isError(), "Exception stuck in context");

        qEng.evaluate(QStringLiteral("throw new Error('Crash 1');"));
        QQuickJSValue r1 = qEng.evaluate(QStringLiteral("1 + 1"));
        QQuickJSValue r2 = qEng.evaluate(QStringLiteral("2 + 2"));
        QQuickJSValue func = qEng.evaluate(QStringLiteral("(function(a,b){ return a+b; })"));
        QQuickJSValue r3 = func.call({QVariant(5), QVariant(5)});
        TEST_RESULT("Recovery: Full Cycle", r1.toInt() == 2 && r2.toInt() == 4 && r3.toInt() == 10, "Engine failed to recover");
    }

    // 5. Promises
    {
        QQuickJSEngine qEng;
        PRINT_ERR_IF_ANY(qEng.evaluate("globalThis.res = null; Promise.resolve(1).then(v => v+1).then(v => v+1).then(v => { globalThis.res = v; })"), "Promise: chain setup");
        TEST_RESULT("Promise: Chain (3 then)", qEng.globalObject().property("res").toInt() == 3, "Chain failed");

        PRINT_ERR_IF_ANY(qEng.evaluate("globalThis.res = null; Promise.all([1,2,3]).then(v => { globalThis.res = v.join('-'); })"), "Promise: Promise.all setup");
        TEST_RESULT("Promise: Promise.all", qEng.globalObject().property("res").toString() == "1-2-3", "All failed");

        PRINT_ERR_IF_ANY(qEng.evaluate("globalThis.res = 'init'; Promise.resolve(1).finally(() => { globalThis.res = 'finally'; })"), "Promise: finally setup");
        TEST_RESULT("Promise: finally", qEng.globalObject().property("res").toString() == "finally", "Finally failed");

        PRINT_ERR_IF_ANY(qEng.evaluate("globalThis.res = null; async function f() { return await 42; } f().then(v => { globalThis.res = v; })"), "Promise: async/await setup");
        TEST_RESULT("Promise: async/await", qEng.globalObject().property("res").toInt() == 42, "Async/await failed");

        PRINT_ERR_IF_ANY(qEng.evaluate("globalThis.res = null; let p = Promise.resolve(0); for(let i=0; i<1000; i++) p = p.then(v => v+1); p.then(v => { globalThis.res = v; })"), "Promise: 1000 Chain setup");
        TEST_RESULT("Promise: 1000 Chain", qEng.globalObject().property("res").toInt() == 1000, "Long chain failed");

        PRINT_ERR_IF_ANY(qEng.evaluate("globalThis.res = null; Promise.resolve().then(() => { throw new Error('Promise Err'); }).catch(e => { globalThis.res = e.message; })"), "Promise: Exception in Chain setup");
        TEST_RESULT("Promise: Exception in Chain", qEng.globalObject().property("res").toString() == "Promise Err", "Exception in chain failed");

        QQuickJSValue err = qEng.evaluate("Promise.resolve().then(() => { globalThis.res = 'executed'; }); throw new Error('Sync Error');");
        bool errOk = err.isError() && err.toString().contains("Sync Error");
        bool promOk = qEng.evaluate("globalThis.res").toString() == "executed";
        TEST_RESULT("Promise: Execute after sync exception", errOk && promOk, "Promise did not execute or exception was lost");

        // A rejection handled by a late .catch() in the same tick is not reported
        {
            QQuickJSEngine qEng;
            int reports = 0;
            QString lastMsg;
            QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString& m, int) { reports++; lastMsg = m; });
            qEng.evaluate("const p = Promise.reject(new Error('late catch')); p.catch(function(e) { globalThis.handled = e.message; });");
            TEST_RESULT("Promise: Late .catch() in same tick is not reported",
                        reports == 0 && qEng.globalObject().property("handled").toString() == "late catch",
                        QStringLiteral("reports=%1 - false report for a rejection handled later in the same tick").arg(reports));

            reports = 0;
            qEng.evaluate("Promise.reject(new Error('unhandled reason'))");
            TEST_RESULT("Promise: Unhandled rejection reported after job drain",
                        reports == 1 && lastMsg.contains("unhandled reason"),
                        QStringLiteral("reports=%1 msg='%2' - the report did not arrive after the job queue drain").arg(reports).arg(lastMsg));
            TEST_RESULT("Promise: Engine alive after rejection reporting", qEng.evaluate("1+1").toInt() == 2, "engine damaged");
        }

        // An unhandled rejection inside a top-level signal callback is reported
        // at the end of that callback (end-of-tick), not lost until the next evaluate
        {
            QQuickJSEngine qEng;
            RegressionTestObject* obj = new RegressionTestObject();
            qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
            int reports = 0;
            QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString&, int) { reports++; });
            qEng.evaluate("testObj.pingSignal.connect(function() { Promise.reject(new Error('sig rejection')); });");
            reports = 0;
            emit obj->pingSignal();   // direct C++ emission: the callback is the top-level entry point
            TEST_RESULT("Promise: Rejection in signal callback reported at tick end",
                        reports == 1,
                        QStringLiteral("reports=%1 - the report was lost until the next evaluate").arg(reports));
            delete obj;
        }
    }

    // 6. QVariant & Containers
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        TEST_RESULT("QVariant: Int", qEng.evaluate("testObj.echoVariant(42).toFixed(0)").toString() == "42", "Int failed");
        TEST_RESULT("QVariant: Bool", qEng.evaluate("testObj.echoVariant(true)").toBool(), "Bool failed");
        TEST_RESULT("QVariant: Double", qFuzzyCompare(qEng.evaluate("testObj.echoVariant(3.14)").toDouble(), 3.14), "Double failed");
        TEST_RESULT("QVariant: List", qEng.evaluate("testObj.echoVariant([1,2]).length").toInt() == 2, "List failed");
        TEST_RESULT("QVariant: Map", qEng.evaluate("testObj.echoVariant({a:1}).a").toInt() == 1, "Map failed");
        TEST_RESULT("QVariant: ByteArray", qEng.evaluate("new Uint8Array(testObj.echoBytes(new Uint8Array([1,2,3]).buffer)).length").toInt() == 3, "ByteArray failed");

        // Regression: quickjs-ng #1894 (v0.18.0) - JS_GetTypedArrayBuffer
        // returns a stale ta->length (the length captured at view creation) for a
        // length-tracking view over a resizable ArrayBuffer. The conversion must
        // clamp the span to the live buffer, aligned down to the element size
        // (JS byteLength semantics). These tests assert the CORRECT behavior -
        // they stay green after an upstream engine fix as well.
        {
            QQuickJSValue setup = qEng.evaluate(
                "var rab = new ArrayBuffer(64, {maxByteLength: 1024});"
                "var trk = new Uint8Array(rab);"
                "for (var i = 0; i < 64; i++) trk[i] = 65;"
                "rab.resize(7);");
            PRINT_ERR_IF_ANY(setup, "RAB setup");
            TEST_RESULT("QVariant: ByteArray RAB shrink",
                qEng.evaluate("new Uint8Array(testObj.echoBytes(trk)).length").toInt() == 7,
                QStringLiteral("length=%1, expected 7 (stale view length)").arg(qEng.evaluate("new Uint8Array(testObj.echoBytes(trk)).length").toInt()));
            TEST_RESULT("QVariant: ByteArray RAB shrink content",
                qEng.evaluate("Array.from(new Uint8Array(testObj.echoBytes(trk))).join(',')").toString() == "65,65,65,65,65,65,65",
                "content does not match the surviving bytes");
        }
        {
            QQuickJSValue setup = qEng.evaluate(
                "var rab2 = new ArrayBuffer(64, {maxByteLength: 1024});"
                "var u16v = new Uint16Array(rab2, 4);"
                "rab2.resize(9);");
            PRINT_ERR_IF_ANY(setup, "RAB2 setup");
            TEST_RESULT("QVariant: ByteArray RAB element alignment",
                qEng.evaluate("new Uint8Array(testObj.echoBytes(u16v)).length").toInt() == 4,
                QStringLiteral("length=%1, expected 4 = floor((9-4)/2)*2").arg(qEng.evaluate("new Uint8Array(testObj.echoBytes(u16v)).length").toInt()));
        }
        {
            QQuickJSValue setup = qEng.evaluate(
                "var rab3 = new ArrayBuffer(64, {maxByteLength: 1024});"
                "var tail = new Uint16Array(rab3, 60);"
                "rab3.resize(8);");
            PRINT_ERR_IF_ANY(setup, "RAB3 setup");
            TEST_RESULT("QVariant: ByteArray RAB past-end view",
                qEng.evaluate("try { testObj.echoBytes(tail); 'noerr'; } catch(e) { 'err'; }").toString() == "err",
                "a view entirely past the buffer end must produce a clean error, not garbage");
        }
        {
            // Buffer growth: the engine's JS layer is honest here (byteLength = 256) -
            // an engine-level regression guard. Binding-level truncation (returning
            // the length captured at view creation) is NOT tested here: fixed and
            // tracking views cannot be distinguished through the public API, so that
            // can only be fixed upstream (#1894).
            QQuickJSValue setup = qEng.evaluate(
                "var rab4 = new ArrayBuffer(64, {maxByteLength: 1024});"
                "var grw = new Uint8Array(rab4);"
                "rab4.resize(256);");
            PRINT_ERR_IF_ANY(setup, "RAB4 setup");
            TEST_RESULT("RAB grow: JS byteLength",
                qEng.evaluate("grw.byteLength").toInt() == 256,
                "the JS layer must see 256 after resize(256)");
        }

        TEST_RESULT("QVariant: Cycle (JSON)", qEng.evaluate("globalThis.cycleRes='safe'; try { let a={}; a.self=a; JSON.stringify(a); } catch(e) { globalThis.cycleRes='cycle'; } globalThis.cycleRes").toString() == "cycle", "Cycle not caught");

        QQuickJSValue resFunc = qEng.evaluate("testObj.echoJsValue(function() { return 123; })");
        TEST_RESULT("QQuickJSValue: Pass Function", resFunc.isCallable() && resFunc.call().toInt() == 123, "Failed to pass JS function");

        QQuickJSValue resObj = qEng.evaluate("testObj.echoJsValue({val: 999})");
        TEST_RESULT("QQuickJSValue: Pass Object", resObj.isObject() && resObj.property("val").toInt() == 999, "Failed to pass JS object");

        QQuickJSValue resIntList = qEng.evaluate("testObj.getIntList()");
        TEST_RESULT("QList<int> to JS Array", resIntList.isArray() && resIntList.property("length").toInt() == 5 && resIntList.property(0U).toInt() == 1, "Failed");

        QQuickJSValue resUInt16List = qEng.evaluate("testObj.getUInt16List()");
        TEST_RESULT("QList<quint16> to JS Array", resUInt16List.isArray() && resUInt16List.property("length").toInt() == 3 && resUInt16List.property(0U).toInt() == 10, "Failed");

        QQuickJSValue resVarHash = qEng.evaluate("testObj.getVariantHash()");
        TEST_RESULT("QVariantHash to JS Object", resVarHash.isObject() && resVarHash.property("name").toString() == "Test" && resVarHash.property("value").toInt() == 42, "Failed");

        QQuickJSValue resIntHash = qEng.evaluate("testObj.getIntHash()");
        TEST_RESULT("QHash<QString, int> to JS Object", resIntHash.isObject() && resIntHash.property("a").toInt() == 1, "Failed");

        obj->deleteLater();
    }

    // Conversion fast paths: exotic JS objects are refused; exact types for
    // plain objects, Date, numerics, QStringList, QObject* and containers
    {
        QQuickJSEngine qEng;

        const char* exotic[] = {
            "new Map([[1, 2]])", "new Set([1])", "new WeakMap()", "new WeakSet()",
            "Promise.resolve(1)", "/re?gex*/", "new DataView(new ArrayBuffer(8))",
            "(function(){})"
        };
        bool allRefused = true;
        for (const char* e : exotic) {
            QVariant v = qEng.evaluate(QString::fromUtf8(e)).toVariant();
            if (v.isValid()) allRefused = false;
        }
        TEST_RESULT("Convert: exotic objects refused", allRefused, "an exotic object produced a valid QVariant");

        QVariant plain = qEng.evaluate("({a: 1, b: 'x'})").toVariant();
        TEST_RESULT("Convert: plain object to QVariantMap",
                    plain.userType() == qMetaTypeId<QVariantMap>() &&
                        plain.toMap().value(QLatin1String("a")).toInt() == 1 &&
                        plain.toMap().value(QLatin1String("b")).toString() == QLatin1String("x"),
                    "plain object conversion broken");

        QVariant dateV = qEng.evaluate("new Date(1700000000000)").toVariant();
        TEST_RESULT("Convert: Date to QDateTime",
                    dateV.userType() == QMetaType::QDateTime &&
                        dateV.toDateTime().toMSecsSinceEpoch() == 1700000000000LL,
                    "Date conversion broken");

        TEST_RESULT("Convert: numeric fast paths exact",
                    qEng.evaluate("2.5").toDouble() == 2.5 &&
                        qEng.evaluate("3").toDouble() == 3.0 &&
                        qEng.evaluate("-7").toInt() == -7 &&
                        qEng.evaluate("2.9").toInt() == 2 &&
                        qEng.evaluate("true").toBool() &&
                        !qEng.evaluate("false").toBool(),
                    "numeric conversion broken");

        qEng.setProperty("slVar", QVariant(QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
        TEST_RESULT("Convert: QStringList to JS array",
                    qEng.evaluate("Array.isArray(slVar) && slVar.length === 3 && slVar[1] === 'b'").toBool(),
                    "QStringList conversion broken");

        TestObject* convObj = new TestObject();
        convObj->setObjectName("convTarget");
        qEng.setProperty("objVar", QVariant::fromValue<QObject*>(convObj));
        TEST_RESULT("Convert: QVariant<QObject*> to wrapper",
                    qEng.evaluate("objVar.objectName === 'convTarget'").toBool(),
                    "QObject* variant conversion broken");
        delete convObj;

        qEng.setProperty("listVar", QVariant(QVariantList{1, 2, 3}));
        QVariantMap mapVar;
        mapVar.insert(QStringLiteral("k"), 5);
        qEng.setProperty("mapVar", QVariant(mapVar));
        TEST_RESULT("Convert: container fast cases",
                    qEng.evaluate("listVar.length === 3 && listVar[2] === 3 && mapVar.k === 5").toBool(),
                    "container fast cases broken");
    }

    // 7. QObject Proxy
    {
        TestObject* obj = new TestObject();
        obj->setObjectName("SharedObj");
        obj->setValue(50);

        compareQObjectEngines("QObject: Const Method", obj, "testObj.getName()");
        compareQObjectEngines("QObject: Enum Read", obj, "testObj.getStatus()");
        compareQObjectEngines("QObject: Enum Set", obj, "testObj.setStatus(0); testObj.getStatus()");
        compareQObjectEngines("QObject: Enum Method", obj, "testObj.processStatus(1)");
        compareQObjectEngines("QObject: Nullptr ==", obj, "testObj.getNull() == null");
        compareQObjectEngines("QObject: Nullptr ===", obj, "testObj.getNull() === null");
        compareQObjectEngines("QObject: Nullptr if", obj, "if(testObj.getNull()) { 'truthy'; } else { 'falsy'; }");
        compareQObjectEngines("QObject: Child Access", obj, "testObj.getChild() ? 'has child' : 'no child'");
        compareQObjectEngines("QObject: Child Identity ==", obj, "testObj.getChild() == testObj.getChild()");
        compareQObjectEngines("QObject: Child Identity ===", obj, "testObj.getChild() === testObj.getChild()");
        compareQObjectEngines("QObject: objectName", obj, "testObj.objectName");
        compareQObjectEngines("QObject: inherits", obj, "testObj.inherits('QObject') ? 'yes' : 'no'");
        compareQObjectEngines("QObject: Const Prop Read", obj, "testObj.readOnlyProp");
        compareQObjectEngines("QObject: Const Prop Write", obj, "testObj.readOnlyProp = 10; testObj.readOnlyProp");
        compareQObjectEngines("QObject: Object.keys", obj, "Object.keys(testObj).length > 0");
        compareQObjectEngines("QObject: for...in", obj, "let count=0; for(let k in testObj) count++; count > 0");
        compareQObjectEngines("QObject: Reflect.ownKeys", obj, "Reflect.ownKeys(testObj).length > 0");
        compareQObjectEngines("QObject: getOwnPropertyDescriptor", obj, "typeof Object.getOwnPropertyDescriptor(testObj, 'value') === 'object'");
        compareQObjectEngines("Functions: typeof method", obj, "typeof testObj.add === 'function'");
        compareQObjectEngines("Functions: method.call", obj, "let f = testObj.add; f.call(testObj, 1, 2);");

        QQuickJSEngine qEngPriv;
        qEngPriv.globalObject().setProperty("testObj", qEngPriv.newQObject(obj));
        QQuickJSValue res1 = qEngPriv.evaluate("Object.keys(testObj).includes('secretSlot')");
        TEST_RESULT("QObject: Private methods hidden from Object.keys", !res1.toBool(), "Private method visible in Object.keys");

        QQuickJSValue res2 = qEngPriv.evaluate("Object.getOwnPropertyNames(testObj).includes('secretSlot')");
        TEST_RESULT("QObject: Private methods hidden from getOwnPropertyNames", !res2.toBool(), "Private method visible in getOwnPropertyNames");

        delete obj;
    }

    // 8. Inheritance & Lifecycle
    {
        DerivedTestObject* derived = new DerivedTestObject();
        QJSEngine qtEng;
        QQuickJSEngine qEng;
        qtEng.globalObject().setProperty("testObj", qtEng.newQObject(derived));
        qEng.globalObject().setProperty("testObj", qEng.newQObject(derived));
        QString qtRes = qtEng.evaluate("testObj.add(1,1) + testObj.derivedMethod()").toString();
        QString qRes = qEng.evaluate("testObj.add(1,1) + testObj.derivedMethod()").toString();
        TEST_RESULT("QObject: Inheritance", qtRes == qRes && qRes == "101", "Failed");

        QQuickJSEngine qEng2;
        qEng2.globalObject().setProperty("testObj", qEng2.newQObject(derived));
        derived->deleteLater();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        TEST_RESULT("QObject: deleteLater", qEng2.evaluate("try { testObj.add(1,1); } catch(e) { 'deleted'; }").toString() == "deleted", "Object still alive");
    }

    // 9. Signals
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        qEng.evaluate("globalThis.count = 0; let f1 = function() { globalThis.count++; }; let f2 = function() { globalThis.count+=10; };");
        qEng.evaluate("testObj.valueChanged.connect(f1); testObj.valueChanged.connect(f2);");
        qEng.evaluate("testObj.value = 1;");
        qEng.evaluate("testObj.valueChanged.disconnect(f1);");
        qEng.evaluate("testObj.value = 2;");
        TEST_RESULT("QObject: Multi-Signal Disconnect", qEng.globalObject().property("count").toInt() == 21, "Count mismatch");

        bool signalFired = false;
        // obj outlives this block (deleteLater), so the reference-capturing lambda
        // must be disconnected before signalFired goes out of scope
        QMetaObject::Connection notifyConn =
            QObject::connect(obj, &TestObject::valueChanged, [&signalFired](int) { signalFired = true; });
        obj->setValue(99);
        TEST_RESULT("Notification: C++ emit", signalFired && qEng.evaluate("testObj.value").toInt() == 99, "C++ notification failed");
        QObject::disconnect(notifyConn);

        QQuickJSValue setup = qEng.evaluate("globalThis.sigArgs = ''; testObj.multiArgSignal.connect(function(a, b, c) { globalThis.sigArgs = a + '_' + b + '_' + c.toFixed(2); });");
        QMetaObject::invokeMethod(obj, "emitMultiArg");
        TEST_RESULT("Signal: Multi-arg (3 args)", qEng.globalObject().property("sigArgs").toString() == "42_test_3.14", "Multi-arg signal failed");

        qEng.evaluate("globalThis.errMsg = ''; try { testObj.throwCxxExceptionInSignal(); } catch(e) { globalThis.errMsg = e.message; }");
        TEST_RESULT("Signal: C++ Exception in Emit", qEng.globalObject().property("errMsg").toString().contains("C++ Signal Exception"), "Failed to catch C++ exception");

        obj->deleteLater();
    }

    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        qEng.evaluate("globalThis.count = 0; let f = function() { globalThis.count++; };");
        qEng.evaluate("testObj.valueChanged.connect(f); testObj.valueChanged.connect(f);");
        qEng.evaluate("testObj.value = 1;");
        qEng.evaluate("testObj.valueChanged.disconnect(f);");
        qEng.evaluate("testObj.value = 2;");

        bool ok = qEng.globalObject().property("count").toInt() == 2;
        TEST_RESULT("Signal: Disconnect removes all matches", ok, "Disconnect did not remove all matching connections");
        delete obj;
    }

    // connect() to a signal with an unregistered parameter type warns;
    // direct emission still delivers undefined for that argument
    {
        QQuickJSEngine qEng;
        RegressionTestObject* obj = new RegressionTestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        QQuickJSValue conn;
        {
            WarningCaptureGuard capture;
            conn = qEng.evaluate("testObj.unregSignal.connect(function(a) { globalThis.unregArg = typeof a; });");
        }

        bool warned = false;
        for (const QString& w : g_capturedWarnings) {
            if (w.contains(QLatin1String("unregSignal")) && w.contains(QLatin1String("unregistered type"))) warned = true;
        }
        TEST_RESULT("Signal: connect warns about unregistered param type",
                    !conn.isError() && warned,
                    "no qWarning on connect() for a signal with an unregistered parameter type");

        qEng.evaluate("testObj.emitUnreg()");
        TEST_RESULT("Signal: emission delivers undefined for unregistered param type",
                    qEng.globalObject().property("unregArg").toString() == "undefined",
                    "direct emission did not deliver undefined");
        delete obj;
    }

    // 10. Limits & Stress
    {
        QQuickJSEngine timeoutEng;
        timeoutEng.setExecutionTimeoutMs(500);
        QElapsedTimer t; t.start();
        QQuickJSValue timeoutRes = timeoutEng.evaluate("while(true){}");
        TEST_RESULT("Limit: Timeout (while(true))", timeoutRes.isError() && t.elapsed() < 2000, "Timeout failed");

        QQuickJSEngine stackEng;
        TEST_RESULT("Limit: Stack Overflow", stackEng.evaluate("function f(){f();} f();").isError(), "Stack overflow not caught");

        QQuickJSEngine memEng;
        memEng.setMemoryLimit(1 * 1024 * 1024);
        TEST_RESULT("Limit: Memory", memEng.evaluate("let a = []; while(true) a.push('test string');").isError(), "Memory limit not triggered");

        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        qEng.setExecutionTimeoutMs(500);
        QElapsedTimer t2; t2.start();
        QQuickJSValue res = qEng.evaluate("while(true) { testObj.add(1, 2); }");
        TEST_RESULT("Limit: Absolute Timeout Deadline", res.isError() && res.toString().contains("interrupted") && t2.elapsed() < 2000, "Timeout was reset by nested calls");
        qEng.setExecutionTimeoutMs(0);
        delete obj;
    }

    // Enabling the timeout mid-execution must grant a fresh quota, not kill instantly
    {
        QQuickJSEngine qEng;
        RegressionTestObject* obj = new RegressionTestObject();
        obj->setJsEngine(&qEng);
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        QElapsedTimer t; t.start();
        QQuickJSValue res = qEng.evaluate("testObj.enableTimeout(600); while(true){}");
        const qint64 el = t.elapsed();
        qInfo().noquote() << QStringLiteral("Mid-run timeout enable: elapsed =") << el << "ms";
        TEST_RESULT("Limit: Timeout enabled mid-run grants a fresh quota",
                    res.isError() && el >= 300 && el < 2000,
                    QStringLiteral("elapsed=%1ms - enabling the timeout mid-run killed the script instantly or did not fire").arg(el));
        TEST_RESULT("Limit: Engine alive after mid-run timeout", qEng.evaluate("1+1").toInt() == 2, "engine damaged");
        delete obj;
    }

    // Disable -> re-enable mid-execution must also grant a fresh quota
    {
        QQuickJSEngine qEng;
        qEng.setExecutionTimeoutMs(300);
        TEST_RESULT("Limit: First timeout run interrupts", qEng.evaluate("while(true){}").isError(), "the first run was not interrupted by the timeout");
        qEng.setExecutionTimeoutMs(0);
        RegressionTestObject* obj = new RegressionTestObject();
        obj->setJsEngine(&qEng);
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        QElapsedTimer t; t.start();
        QQuickJSValue res = qEng.evaluate("testObj.enableTimeout(600); while(true){}");
        const qint64 el = t.elapsed();
        qInfo().noquote() << QStringLiteral("Timeout disable->re-enable: elapsed =") << el << "ms";
        TEST_RESULT("Limit: Re-enable mid-run grants a fresh quota",
                    res.isError() && el >= 300 && el < 2000,
                    QStringLiteral("elapsed=%1ms - a stale deadline killed the new script instantly").arg(el));
        delete obj;
    }

    // A stale interrupt flag must not kill the next entry point
    {
        QQuickJSEngine qEng;
        QQuickJSValue func = qEng.evaluate("(function() { return 42; })");
        qEng.setInterrupted(true);
        QQuickJSValue res = func.call();
        TEST_RESULT("Interrupt: Stale flag does not kill next call()",
                    res.toInt() == 42,
                    QStringLiteral("call() interrupted by a stale flag: '%1'").arg(res.toString()));
        qEng.setInterrupted(true);
        TEST_RESULT("Interrupt: Stale flag does not kill next evaluate()",
                    qEng.evaluate("1 + 1").toInt() == 2, "evaluate() interrupted by a stale flag");
    }

    // 11. API Compliance
    {
        QQuickJSEngine qEng;
        QQuickJSValue obj = qEng.newObject();
        obj.setProperty("x", 10);
        TEST_RESULT("API: newObject & setProperty", obj.property("x").toInt() == 10, "Failed");

        // Deliberately non-ASCII (Cyrillic) value: also covers the UTF-8 roundtrip
        qEng.setProperty("strVal", "Привет, мир!");
        TEST_RESULT("API: setProperty(QString, QString)", qEng.evaluate("strVal").toString() == "Привет, мир!", "Failed");
        qEng.setProperty(QByteArrayLiteral("cstrVal").constData(), "Test C-String");
        TEST_RESULT("API: setProperty(QByteArray, const char*)", qEng.evaluate("cstrVal").toString() == "Test C-String", "Failed");
        qEng.setProperty("intVal", 42);
        TEST_RESULT("API: setProperty(QString, int)", qEng.evaluate("intVal").toInt() == 42, "Failed");
        qEng.setProperty("boolVal", true);
        TEST_RESULT("API: setProperty(QString, bool)", qEng.evaluate("boolVal").toBool(), "Failed");
        qEng.setProperty("doubleVal", 3.14);
        TEST_RESULT("API: setProperty(QString, double)", qFuzzyCompare(qEng.evaluate("doubleVal").toDouble(), 3.14), "Failed");
        qEng.setProperty("varVal", QVariant(100));
        TEST_RESULT("API: setProperty(QString, QVariant)", qEng.evaluate("varVal").toInt() == 100, "Failed");

        QQuickJSValue arr = qEng.newArray(5);
        TEST_RESULT("API: newArray(5)", arr.property("length").toInt() == 5, "Array length mismatch");

        auto cppFunc = [](JSContext* ctx, JSValue, int, JSValueConst*) -> JSValue { return JS_NewString(ctx, "Hello from C++"); };
        qEng.globalObject().setProperty("myFunc", qEng.newFunction(cppFunc, "myFunc", 0));
        TEST_RESULT("API: newFunction", qEng.evaluate("myFunc()").toString() == "Hello from C++", "C++ function call failed");

        QQuickJSValue err = qEng.newErrorObject(QQuickJSValue::TypeError, "Test Error");
        TEST_RESULT("API: newErrorObject", err.isError() && err.toString().contains("Test Error"), "Error object creation failed");

        qEng.installExtensions();
        TEST_RESULT("API: installExtensions (qsTr)", qEng.evaluate("qsTr('test')").toString() == "test", "qsTr missing");
        TEST_RESULT("API: installExtensions (gc)", qEng.evaluate("typeof gc").toString() == "function", "gc missing");
        TEST_RESULT("API: installExtensions (console)", qEng.evaluate("typeof console.log").toString() == "function", "console missing");

        // installExtensions must reinstall extensions deleted from JS
        {
            QQuickJSEngine qEng;
            qEng.installExtensions(QQuickJSEngine::ConsoleExtension);
            TEST_RESULT("API: installExtensions installs console", qEng.evaluate("typeof console").toString() == "object", "console is not installed");
            qEng.evaluate("delete console");
            TEST_RESULT("API: console deleted from JS", qEng.evaluate("typeof console").toString() == "undefined", "console cannot be deleted from JS");
            qEng.installExtensions(QQuickJSEngine::ConsoleExtension);
            TEST_RESULT("API: installExtensions restores deleted console",
                        qEng.evaluate("typeof console").toString() == "object" && qEng.evaluate("typeof console.log").toString() == "function",
                        "a repeated installExtensions did not restore the deleted console");
        }

        qEng.setAutoProcessPromises(false);
        qEng.evaluate("globalThis.promiseRes = null; Promise.resolve(1).then(v => { globalThis.promiseRes = v; })");
        bool autoOff = qEng.globalObject().property("promiseRes").isNull();
        qEng.setAutoProcessPromises(true);
        TEST_RESULT("API: setAutoProcessPromises(false)", autoOff, "Promise executed when auto-processing was off");

        QQuickJSEngine qEngInt;
        auto interruptFunc = [](JSContext* ctx, JSValue, int, JSValueConst*) -> JSValue {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetContextOpaque(ctx));
            engine->setInterrupted(true);
            return JS_UNDEFINED;
        };
        qEngInt.globalObject().setProperty("interruptMe", qEngInt.newFunction(interruptFunc, "interruptMe", 0));
        TEST_RESULT("API: setInterrupted", qEngInt.evaluate("interruptMe(); while(true){}").isError(), "Manual interrupt failed");

        bool signalEmitted = false;
        QString errMsg;
        QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString& msg, int) { signalEmitted = true; errMsg = msg; });
        qEng.evaluate("throw new Error('Signal Test');");
        TEST_RESULT("API: uncaughtException Signal", signalEmitted && errMsg.contains("Signal Test"), "Signal not emitted on throw");
    }

    // 12. QQuickJSValue API
    {
        QQuickJSEngine qEng;
        QQuickJSValue obj = qEng.evaluate("({a: 1, b: undefined})");
        TEST_RESULT("API: hasProperty (true)", obj.hasProperty("a"), "hasProperty failed for existing");
        TEST_RESULT("API: hasProperty (undefined)", obj.hasProperty("b"), "hasProperty failed for undefined");
        TEST_RESULT("API: hasProperty (false)", !obj.hasProperty("c"), "hasProperty failed for missing");
        TEST_RESULT("API: deleteProperty", obj.deleteProperty("a") && !obj.hasProperty("a"), "deleteProperty failed");

        QQuickJSValue arr = qEng.evaluate("[10, 20, 30]");
        TEST_RESULT("API: property(int)", arr.property(1).toInt() == 20, "Array index property failed");
        arr.setProperty(1, 99);
        TEST_RESULT("API: setProperty(int)", arr.property(1).toInt() == 99, "Array index set failed");

        QQuickJSValue ctor = qEng.evaluate("function MyClass(v) { this.val = v; } MyClass");
        QQuickJSValue inst = ctor.construct({QVariant(42)});
        TEST_RESULT("API: construct", inst.property("val").toInt() == 42, "Constructor call failed");
        QQuickJSValue inst2 = ctor.callAsConstructor({qEng.evaluate("77")});
        TEST_RESULT("API: callAsConstructor", inst2.property("val").toInt() == 77, "callAsConstructor failed");
        TEST_RESULT("API: instanceOf", inst.instanceOf(ctor), "instanceOf failed");

        QQuickJSValue obj1 = qEng.evaluate("let x = {a:1}; x");
        QQuickJSValue obj2 = qEng.evaluate("x");
        QQuickJSValue obj3 = qEng.evaluate("({a:1})");
        TEST_RESULT("API: strictlyEquals (true)", obj1.strictlyEquals(obj2), "strictlyEquals failed for same object");
        TEST_RESULT("API: strictlyEquals (false)", !obj1.strictlyEquals(obj3), "strictlyEquals failed for different objects");

        TEST_RESULT("API: isUndefined", qEng.evaluate("undefined").isUndefined() && !qEng.evaluate("null").isUndefined(), "isUndefined failed");
        TEST_RESULT("API: isNull", qEng.evaluate("null").isNull() && !qEng.evaluate("undefined").isNull(), "isNull failed");
        TEST_RESULT("API: isBool", qEng.evaluate("true").isBool(), "isBool failed");
        TEST_RESULT("API: isNumber", qEng.evaluate("42").isNumber(), "isNumber failed");
        TEST_RESULT("API: isString", qEng.evaluate("'str'").isString(), "isString failed");
        TEST_RESULT("API: isObject", qEng.evaluate("({})").isObject(), "isObject failed");
        TEST_RESULT("API: isArray", qEng.evaluate("[]").isArray(), "isArray failed");
        TEST_RESULT("API: isCallable", qEng.evaluate("(function(){})").isCallable(), "isCallable failed");
        TEST_RESULT("API: isValid", qEng.evaluate("42").isValid() && qEng.evaluate("undefined").isValid() && !QQuickJSValue().isValid(), "isValid failed");

        QQuickJSValue invalid;
        TEST_RESULT("API: invalid value operations (no crash)", !invalid.property("test").isValid() && !invalid.hasProperty("test") && !invalid.deleteProperty("test") && invalid.toInt() == 0 && !invalid.toBool() && qIsNaN(invalid.toDouble()) && invalid.toString().isEmpty() && !invalid.toVariant().isValid() && !invalid.isValid() && !invalid.isError() && !invalid.isCallable() && !invalid.isObject() && !invalid.isArray() && !invalid.isString() && !invalid.isNumber() && !invalid.isBool() && !invalid.isNull() && invalid.isUndefined(), "Invalid value operations should be safe");

        qEng.evaluate("function sumArgs(a,b,c) { return a+b+c; }");
        QQuickJSValue func = qEng.evaluate("sumArgs");
        TEST_RESULT("API: call(QVariantList, thisObj)", func.call({QVariant(1), QVariant(2), QVariant(3)}).toInt() == 6, "call with QVariantList failed");
        TEST_RESULT("API: call() default args", qEng.evaluate("(function() { return 7; })").call().toInt() == 7, "call with default args failed");
        TEST_RESULT("API: call (exception)", qEng.evaluate("(function() { throw new Error('CallErr'); })").call().toString().contains("CallErr"), "Exception in call not captured");

        QQuickJSEngine otherEng;
        QQuickJSValue v1 = qEng.evaluate("42");
        QQuickJSValue v2 = otherEng.evaluate("42");
        TEST_RESULT("API: strictlyEquals (cross-context)", !v1.strictlyEquals(v2), "Cross-context should not be equal");
        TEST_RESULT("API: call (cross-context args)", !func.call(QList<QQuickJSValue>{v2}).isValid(), "Cross-context args should fail");

        QQuickJSValue fnP = qEng.evaluate("(function() { Promise.resolve(7).then(function(v) { globalThis.res = v; }); return 1; })");
        QQuickJSValue rP = fnP.call();
        TEST_RESULT("API: call() still processes microtasks",
                    rP.toInt() == 1 && qEng.globalObject().property("res").toInt() == 7,
                    "microtask lost after call()");
    }

    // 13. Recursion & Limits
    {
        QQuickJSEngine qEng;
        QVariant var = qEng.evaluate("(function() { let o = {}; o.self = o; return o; })()").toVariant();
        TEST_RESULT("Recursion Guard: Self-reference (Object)", var.userType() == QMetaType::QVariantMap && !var.toMap().value("self").isValid(), "Failed to handle self-reference");

        var = qEng.evaluate("(function() { let a = {}; let b = {}; a.b = b; b.a = a; return a; })()").toVariant();
        TEST_RESULT("Recursion Guard: Mutual reference", var.userType() == QMetaType::QVariantMap && var.toMap().value("b").userType() == QMetaType::QVariantMap && !var.toMap().value("b").toMap().value("a").isValid(), "Failed to handle mutual reference");

        auto testDepth = [&](int depth, bool shouldBeOk) {
            QString script = QStringLiteral("(function() { let d = {}; let cur = d; for(let i=0; i<%1; i++) { cur.next = {}; cur = cur.next; } return d; })()").arg(depth);
            QVariant v = qEng.evaluate(script).toVariant();
            for (int i = 0; i < depth; ++i) v = v.toMap().value("next");
            TEST_RESULT(QString::fromUtf8("Depth Limit: %1 levels").arg(depth), (v.userType() == QMetaType::QVariantMap) == shouldBeOk, "Depth limit failed");
        };
        testDepth(63, true);
        testDepth(64, true);
        testDepth(65, false);
    }

    // 14. Regressions
    {
        TEST_RESULT("Translate: unexpected token", translateJsError("unexpected token '}'").contains("неожиданный токен"), "Regex group failed");
        TEST_RESULT("Translate: is not defined", translateJsError("foo is not defined") == "foo не определён", "Regex group failed");
        TEST_RESULT("Translate: no match passthrough", translateJsError("some unrelated message") == "some unrelated message", "Passthrough failed");
        TEST_RESULT("Translate: cannot read property", translateJsError("Cannot read property 'x' of null") == "Невозможно прочитать свойство 'x' из null", "Regex group failed");
        // quickjs-ng emits native messages in lower case: a 0.18.0 run log showed
        // "TypeError: cannot read property 'property' of null" reaching uncaughtException
        // untranslated. The translator must match the engine's real format, not only the
        // Qt-compatible capitalization. A failure here means the regex is case-sensitive; the
        // fix is QRegularExpression::CaseInsensitiveOption in JsErrorTranslator (cosmetic, not a gate blocker).
        TEST_RESULT("Translate: lowercase 'cannot read property'", translateJsError("cannot read property 'x' of null") == "Невозможно прочитать свойство 'x' из null", "regex does not match quickjs-ng's real lowercase format");
        TEST_RESULT("Translate: maximum call stack", translateJsError("Maximum call stack size exceeded") == "Превышен максимальный размер стека вызовов", "Passthrough failed");

        // Distinguish a legitimately thrown empty string from a failed toString
        {
            QQuickJSEngine qEng;
            QString msg;
            bool got = false;
            QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString& m, int) { got = true; msg = m; });

            qEng.evaluate("throw '';");
            TEST_RESULT("Uncaught: throw '' keeps empty message",
                        got && msg.isEmpty(),
                        QStringLiteral("msg='%1' - the empty message was replaced by the fallback text").arg(msg));

            got = false;
            qEng.evaluate("throw { toString: function() { throw new Error('nope'); } };");
            TEST_RESULT("Uncaught: throwing toString gets fallback message",
                        got && msg == QStringLiteral("Unknown exception (toString failed)"),
                        QStringLiteral("msg='%1' - the fallback text was not substituted").arg(msg));
            TEST_RESULT("Uncaught: Engine alive", qEng.evaluate("1+1").toInt() == 2, "engine damaged");
        }

        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        TEST_RESULT("QProperty: Incompatible Type Assignment", qEng.evaluate("testObj.dateProp = {a: 1};").isError() && qEng.evaluate("testObj.dateProp = {a: 1};").toString().contains("incompatible type", Qt::CaseInsensitive), "Did not throw TypeError");
        TEST_RESULT("Exception: isError for primitive throw", qEng.evaluate("throw 123;").isError(), "throw 123 not detected as error");
        TEST_RESULT("Exception: isError for string throw", qEng.evaluate("throw 'string error';").isError(), "throw string not detected as error");
        delete obj;

        // Unrepresentable values must throw TypeError instead of silently writing defaults
        {
            QQuickJSEngine qEng;
            RegressionTestObject* obj = new RegressionTestObject();
            qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

            QQuickJSValue r1 = qEng.evaluate("testObj.intProp = Symbol()");
            TEST_RESULT("QProperty: Symbol to int throws TypeError",
                        r1.isError() && r1.toString().contains("incompatible type", Qt::CaseInsensitive) && obj->intProp() == -1,
                        QStringLiteral("silent default write: r='%1' intProp=%2").arg(r1.toString()).arg(obj->intProp()));

            QQuickJSValue r2 = qEng.evaluate("testObj.doubleProp = Symbol()");
            TEST_RESULT("QProperty: Symbol to double throws TypeError",
                        r2.isError() && r2.toString().contains("incompatible type", Qt::CaseInsensitive) && obj->doubleProp() == -1.0,
                        QStringLiteral("silent default write: r='%1'").arg(r2.toString()));

            QQuickJSValue r3 = qEng.evaluate("testObj.objProp = Symbol()");
            TEST_RESULT("QProperty: Symbol to QObject* throws TypeError",
                        r3.isError() && r3.toString().contains("incompatible type", Qt::CaseInsensitive) && obj->objProp() == obj,
                        QStringLiteral("silent nullptr write: r='%1'").arg(r3.toString()));

            QQuickJSValue r4 = qEng.evaluate("testObj.objProp = new Map()");
            TEST_RESULT("QProperty: Map to QObject* throws TypeError",
                        r4.isError() && r4.toString().contains("incompatible type", Qt::CaseInsensitive) && obj->objProp() == obj,
                        QStringLiteral("silent nullptr write: r='%1'").arg(r4.toString()));

            // Legacy semantics preserved: undefined/null keep the old default writes
            qEng.evaluate("testObj.intProp = undefined");
            bool intOk = obj->intProp() == 0;
            qEng.evaluate("testObj.intProp = null");
            intOk = intOk && obj->intProp() == 0;
            TEST_RESULT("QProperty: undefined/null to int keeps coercion default", intOk, "undefined/null no longer convert to 0");

            obj->setObjProp(obj);
            qEng.evaluate("testObj.objProp = undefined");
            bool objOk = obj->objProp() == nullptr;
            qEng.evaluate("testObj.objProp = null");
            objOk = objOk && obj->objProp() == nullptr;
            TEST_RESULT("QProperty: undefined/null to QObject* keeps nullptr write", objOk, "undefined/null no longer write nullptr");

            qEng.evaluate("testObj.objProp = testObj");
            TEST_RESULT("QProperty: QObject wrapper still assignable to QObject*", obj->objProp() == obj, "the QObject wrapper is no longer assignable");

            // Bool fast path keeps ECMAScript ToBoolean semantics (total, never throws)
            qEng.evaluate("testObj.boolProp = Symbol()");
            TEST_RESULT("QProperty: Symbol to bool keeps ToBoolean coercion",
                        obj->boolProp() == true,
                        "the bool path must not break total ToBoolean semantics (Symbol is truthy)");

            TEST_RESULT("QProperty: Engine alive after TypeErrors", qEng.evaluate("1+1").toInt() == 2, "engine damaged");
            delete obj;
        }

        QString code = "throw new Error('X');";
        QQuickJSValue e1 = qEng.evaluate(code, "file1.js");
        QQuickJSValue e2 = qEng.evaluate(code, "file2.js");
        TEST_RESULT("EvalCache: fileName not shared", e1.property("stack").toString().contains("file1.js") && e2.property("stack").toString().contains("file2.js"), "Cache returned wrong fileName");

        QThread thread;
        thread.start();
        TestObject* threadObj = new TestObject();
        threadObj->moveToThread(&thread);
        qEng.globalObject().setProperty("testObj", qEng.newQObject(threadObj));
        TEST_RESULT("Signal: Cross-thread connect fails", qEng.evaluate("testObj.valueChanged.connect(function() {});").isError(), "Should not allow cross-thread signal connection");
        QMetaObject::invokeMethod(threadObj, [threadObj](){ delete threadObj; }, Qt::DirectConnection);
        thread.quit();
        thread.wait();

        qEng.setProperty("dateVar", QVariant(QDate(2024, 1, 1)));
        TEST_RESULT("Types: QDate to JS Date", qEng.evaluate("dateVar instanceof Date && dateVar.getFullYear() === 2024 && dateVar.getMonth() === 0 && dateVar.getDate() === 1").toBool(), "QDate to JS Date failed");

        bool ok = true;
        for (int i = 0; i < 12; ++i) if (qEng.evaluate(QStringLiteral("var a_%1 = %1;").arg(i)).isError()) ok = false;
        TEST_RESULT("EvalCache: Eviction Limit (12 scripts)", ok, "Failed to handle >8 scripts in cache");

        obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        TEST_RESULT("Dynamic Prop: Over Cache Limit (100 props)", !qEng.evaluate("for(let i=0; i<100; i++) { testObj['dyn' + i] = i; }").isError() && obj->property("dyn50").toInt() == 50 && qEng.evaluate("testObj.dyn99").toInt() == 99, "Failed to handle >64 dynamic properties");
        qEng.evaluate("testObj.futureProp");
        obj->setProperty("futureProp", 123);
        TEST_RESULT("Dynamic Prop: Cache Invalidation on C++ Add", qEng.evaluate("testObj.futureProp").toInt() == 123, "Cache held stale state");
        delete obj;

        QQuickJSValue proxyObj = qEng.evaluate("new Proxy({}, { set: function(target, prop, value) { throw new Error('Proxy set trap failed'); } })");
        bool crashed = false;
        try {
            QQuickJSValue valObj = qEng.evaluate("({a: 1})");
            for (int i = 0; i < 100; ++i) {
                proxyObj.setProperty("x", "test string");
                proxyObj.setProperty(0U, valObj);
            }
        } catch (...) { crashed = true; }
        TEST_RESULT("Regression: No Double Free on JS_SetProperty failure", !crashed, "Crashed due to double free!");

        QVariant vNull = qEng.evaluate("null").toVariant();
        QVariant vUndef = qEng.evaluate("undefined").toVariant();
        TEST_RESULT("Regression: null vs undefined to QVariant", vNull.isValid() && vNull.userType() == QMetaType::VoidStar && vNull.value<void*>() == nullptr && !vUndef.isValid(), "null/undefined conversion mismatch");

        qEng.installExtensions(QQuickJSEngine::TranslationExtension);
        ok = true;
        for (int i = 0; i < 100; ++i) {
            QQuickJSValue res = qEng.evaluate(QStringLiteral("qsTr('test string %1')").arg(i));
            if (res.isError() || res.toString() != QString::fromUtf8("test string %1").arg(i)) { ok = false; break; }
        }
        TEST_RESULT("Regression: qsTr memory safety", ok, "qsTr failed or corrupted memory");

        QObject longLivedObject;
        longLivedObject.setObjectName("InitialName");
        {
            QQuickJSEngine qEng2;
            qEng2.globalObject().setProperty("obj", qEng2.newQObject(&longLivedObject));
            qEng2.evaluate("obj.objectName");
        }
        longLivedObject.setObjectName("Survived");
        TEST_RESULT("Regression: Destroy Engine while QObject Alive", longLivedObject.objectName() == "Survived", "Crashed or object corrupted");
    }

    // 15. BigInt Support
    {
        QQuickJSEngine qEng;
        qint64 hugeVal = 9007199254740993LL;
        qEng.setProperty("hugeVal", QVariant::fromValue(hugeVal));
        TEST_RESULT("QVariant: Large qint64 to BigInt", qEng.evaluate("typeof hugeVal === 'bigint' && hugeVal === 9007199254740993n").toBool(), "Precision lost");

        quint64 hugeUVal = 18446744073709551615ULL;
        qEng.setProperty("hugeUVal", QVariant::fromValue(hugeUVal));
        TEST_RESULT("QVariant: Large quint64 to BigInt", qEng.evaluate("typeof hugeUVal === 'bigint' && hugeUVal === 18446744073709551615n").toBool(), "Precision lost");

        QVariant varBigInt = qEng.evaluate("9007199254740993n").toVariant();
        TEST_RESULT("JS BigInt to QVariant (qint64)", varBigInt.isValid() && varBigInt.userType() == QMetaType::LongLong && varBigInt.toLongLong() == 9007199254740993LL, "Failed to convert");

        qint64 safeVal = 9007199254740991LL;
        qEng.setProperty("safeVal", QVariant::fromValue(safeVal));
        TEST_RESULT("QVariant: Safe qint64 remains Number", qEng.evaluate("typeof safeVal === 'number' && safeVal === 9007199254740991").toBool(), "Safe integer was incorrectly converted");

        auto testConversion = [&](const char* jsExpr, QMetaType::Type expectedType, qint64 expectedVal64, quint64 expectedValU64) {
            QVariant var = qEng.evaluate(QString::fromUtf8(jsExpr)).toVariant();
            bool typeOk = var.userType() == expectedType;
            bool valOk = (expectedType == QMetaType::LongLong) ? (var.toLongLong() == expectedVal64) : (var.toULongLong() == expectedValU64);
            TEST_RESULT(QString::fromUtf8("BigInt Boundary: %1").arg(QString::fromUtf8(jsExpr)), typeOk && valOk, "Conversion failed");
        };
        testConversion("9223372036854775807n", QMetaType::LongLong, 9223372036854775807LL, 0);
        testConversion("(-9223372036854775807n - 1n)", QMetaType::LongLong, -9223372036854775807LL - 1LL, 0);
        testConversion("9223372036854775808n", QMetaType::ULongLong, 0, 9223372036854775808ULL);
        testConversion("18446744073709551615n", QMetaType::ULongLong, 0, 18446744073709551615ULL);
        testConversion("9007199254740992n", QMetaType::LongLong, 9007199254740992LL, 0);
        testConversion("9007199254740993n", QMetaType::LongLong, 9007199254740993LL, 0);
        testConversion("9007199254740991n", QMetaType::LongLong, 9007199254740991LL, 0);

        auto testShortBigInt = [&](const char* jsExpr, qint64 expectedVal) {
            QVariant var = qEng.evaluate(QString::fromUtf8(jsExpr)).toVariant();
            TEST_RESULT(QString::fromUtf8("Short BigInt: %1 -> qint64").arg(QString::fromUtf8(jsExpr)), var.userType() == QMetaType::LongLong && var.toLongLong() == expectedVal, "Conversion failed");
        };
        testShortBigInt("0n", 0);
        testShortBigInt("1n", 1);
        testShortBigInt("-1n", -1);
        testShortBigInt("127n", 127);
        testShortBigInt("-128n", -128);
        testShortBigInt("2147483647n", 2147483647LL);
        testShortBigInt("-2147483648n", -2147483648LL);

        QVariant huge = qEng.evaluate("99999999999999999999999999999999n").toVariant();
        TEST_RESULT("BigInt: Overflow to QString fallback", huge.userType() == QMetaType::QString && huge.toString().contains("9999999"), "Overflow fallback failed");

        QVariant huge2 = qEng.evaluate("18446744073709551616n").toVariant();
        TEST_RESULT("BigInt: 2^64 Overflow to QString", huge2.userType() == QMetaType::QString && huge2.toString() == "18446744073709551616", "2^64 overflow fallback failed");

        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        QQuickJSValue res1 = qEng.evaluate("testObj.bigProp = 9007199254740993n;");
        QQuickJSValue res2 = qEng.evaluate("testObj.echoBigInt(9007199254740993n)");
        TEST_RESULT("QObject: BigInt Property/Method", !res1.isError() && obj->getBigProp() == 9007199254740993LL && !res2.isError() && res2.toVariant().toLongLong() == 9007199254740993LL, "BigInt interop through QObject failed");
        delete obj;
    }

    // 16. Advanced Engine API
    {
        QQuickJSEngine qEng;
        TEST_RESULT("API: version()", !QQuickJSEngine::version().isEmpty(), "Version is empty");
        TEST_RESULT("API: memoryUsage()", qEng.memoryUsage().memory_used_size >= 0, "Memory usage failed");
        qEng.setGCThreshold(1 * 1024 * 1024);
        TEST_RESULT("API: setGCThreshold()", qEng.evaluate("1 + 1").toInt() == 2, "Engine unusable after setGCThreshold");

        qEng.setEvalCacheEnabled(false);
        TEST_RESULT("API: EvalCache Management", qEng.evaluate("1 + 1").toInt() == 2, "EvalCache management failed");
        qEng.setEvalCacheEnabled(true);
        qEng.setEvalCacheLimit(2);
        qEng.clearEvalCache();

        QQuickJSValue err = qEng.evaluate("\n\n\n\nthrow new Error('LineTest');", "test.js", 10);
        TEST_RESULT("API: evaluate with lineNumber", err.isError() && err.property("lineNumber").toInt() == 14, "Line number offset failed");

        TEST_RESULT("API: fromException (no exception)", !QQuickJSValue::fromException(&qEng).isError(), "Created error from empty exception");

        qEng.installExtensions(QQuickJSEngine::ConsoleExtension);
        TEST_RESULT("API: console.error/warn routing", qEng.evaluate("console.error('Test Error Log'); console.warn('Test Warn Log');").isUndefined(), "Console routing crashed or failed");
    }

    // 17. REGRESSION TESTS FOR DEFECTS FOUND IN CODE REVIEWS (SAFE - NO CRASHES)
    qInfo().noquote() << QStringLiteral("\n--- Regression Tests for Defects ---\n");

    // Unregistered types in metacalls should throw TypeError instead of corrupting the stack
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        QQuickJSValue r1 = qEng.evaluate("testObj.takeUnregistered(5)");
        TEST_RESULT("UnregisteredType: Argument throws TypeError",
                    r1.isError() && obj->unregArgs() == 0,
                    QString("Call succeeded silently (unregArgs=%1, result='%2')").arg(obj->unregArgs()).arg(r1.toString()));

        QQuickJSValue r2 = qEng.evaluate("testObj.uPropW = 5");
        TEST_RESULT("UnregisteredType: Property write throws TypeError",
                    r2.isError(), "Write succeeded silently");

        TEST_RESULT("UnregisteredType: Engine alive after rejections", qEng.evaluate("1+1").toInt() == 2, "Engine damaged");
        delete obj;
    }

    // long/unsigned long should not lose precision on LP64 systems
#if QT_POINTER_SIZE == 8 && !defined(Q_OS_WIN)
    {
        QQuickJSEngine qEng;
        qEng.setProperty("longPos", QVariant::fromValue(static_cast<long>(5000000000LL)));
        TEST_RESULT("Long Precision: Positive > INT32_MAX",
                    qEng.evaluate("longPos === 5000000000").toBool(),
                    QString("Got: %1").arg(qEng.evaluate("String(longPos)").toString()));

        qEng.setProperty("longNeg", QVariant::fromValue(static_cast<long>(-5000000000LL)));
        TEST_RESULT("Long Precision: Negative < INT32_MIN",
                    qEng.evaluate("longNeg === -5000000000").toBool(),
                    QString("Got: %1").arg(qEng.evaluate("String(longNeg)").toString()));

        qEng.setProperty("ulongSmall", QVariant::fromValue(static_cast<unsigned long>(3000000000UL)));
        TEST_RESULT("ULong Precision: > INT32_MAX to Number",
                    qEng.evaluate("typeof ulongSmall === 'number' && ulongSmall === 3000000000").toBool(),
                    QString("Got: %1").arg(qEng.evaluate("String(ulongSmall)").toString()));

        qEng.setProperty("ulongBig", QVariant::fromValue(static_cast<unsigned long>(18446744073709551615ULL)));
        TEST_RESULT("ULong Precision: MAX to BigInt",
                    qEng.evaluate("typeof ulongBig === 'bigint' && ulongBig === 18446744073709551615n").toBool(),
                    QString("Got: %1").arg(qEng.evaluate("String(ulongBig)").toString()));
    }
#else
    qInfo().noquote() << "Long Precision: Skipped (not LP64 platform)";
#endif

    // Deeply nested QVariantMap should fail with an error, not silently return null
    {
        QQuickJSEngine qEng;
        QVariant deep;
        for (int i = 0; i < 70; ++i) {
            QVariantMap m;
            m.insert(QStringLiteral("next"), deep);
            deep = QVariant(m);
        }
        qEng.setProperty("deepVar", deep);
        QQuickJSValue t = qEng.evaluate("typeof deepVar");
        TEST_RESULT("Deep QVariantMap: Conversion aborted with error",
                    t.toString() == "undefined",
                    QString("typeof deepVar = '%1' (silent data loss)").arg(t.toString()));
        TEST_RESULT("Deep QVariantMap: Engine alive after abort", qEng.evaluate("1+1").toInt() == 2, "Engine damaged");
    }

    // Private slots should not be accessible from JS
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        QQuickJSValue t1 = qEng.evaluate("typeof testObj.secretSlot");
        TEST_RESULT("Private Slot: Hidden as property",
                    t1.toString() == "undefined",
                    QString("typeof = '%1'").arg(t1.toString()));

        QQuickJSValue t2 = qEng.evaluate("try { testObj.secretSlot(); 'called'; } catch(e) { 'error'; }");
        TEST_RESULT("Private Slot: Call impossible",
                    t2.toString() == "error" && obj->secretCalls() == 0,
                    QString("Slot executed (secretCalls=%1)").arg(obj->secretCalls()));
        delete obj;
    }

    // Signal callbacks should not reset the execution timeout deadline
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        qEng.evaluate("testObj.valueChanged.connect(function(){})");
        qEng.setExecutionTimeoutMs(500);

        InterruptWatchdog wd;
        wd.engine = &qEng;
        wd.delayMs = 2500;
        wd.start();

        QElapsedTimer t; t.start();
        QQuickJSValue res = qEng.evaluate("while(true) { testObj.value = 1; }");
        qint64 el = t.elapsed();
        wd.stopFlag = true;
        wd.wait();

        qInfo().noquote() << QStringLiteral("Signal Timeout: elapsed =") << el << "ms";
        TEST_RESULT("Signal Timeout: Not reset by callbacks",
                    res.isError() && el < 1500,
                    QString("elapsed=%1ms - Timeout bypassed via signal emit").arg(el));
        delete obj;
    }

    // Nested evaluate() should not clear the m_interrupted flag
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        obj->setJsEngine(&qEng);
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        InterruptWatchdog wd;
        wd.engine = &qEng;
        wd.delayMs = 2500;
        wd.start();

        QElapsedTimer t; t.start();
        QQuickJSValue res = qEng.evaluate("testObj.selfInterruptThenEval(); while(true){}");
        qint64 el = t.elapsed();
        wd.stopFlag = true;   // as in the test above: no 2.5s stall, no spurious late interrupt
        wd.wait();

        qInfo().noquote() << QStringLiteral("Nested Interrupt: elapsed =") << el << "ms";
        TEST_RESULT("Nested Interrupt: Flag not cleared by nested evaluate",
                    res.isError() && el < 1500,
                    QString("elapsed=%1ms - Interrupt flag lost").arg(el));
        delete obj;
    }

    // QuickJS-NG v0.18.0 (#1810/#1814): InternalError (OOM, interrupt) automatically
    // carries the is_uncatchable_error flag - JS try/catch does NOT swallow it: the error
    // must reach the C++ boundary of evaluate() and the engine must survive. Verified on a
    // live v0.18.0: OOM -> "InternalError: out of memory", interrupt -> "InternalError:
    // interrupted", `throw new InternalError()` from JS - all uncatchable; stack overflow
    // remains a catchable RangeError "Maximum call stack size exceeded". The other side of
    // the contract: converted C++ exceptions must remain catchable from JS - regression
    // coverage for the "Signal: C++ Exception in Emit" fix that failed on 0.18.0.
    // Wrapper policy (same as safeMetacall): a known std::exception ->
    // JS_ThrowPlainError (catchable Error, item (f)); an unknown type -> InternalError,
    // i.e. uncatchable (#1810/#1814) - a contract, not a version accident (item (g)).
    {
        // (a) OOM inside try/catch: the catch block never runs, the error reaches C++
        QQuickJSEngine oomEng;
        oomEng.setMemoryLimit(1 * 1024 * 1024);
        QQuickJSValue oomRes = oomEng.evaluate(
            "globalThis.sw = 'no'; "
            "try { let a = []; while(true) a.push('test string'); globalThis.sw = 'completed'; } "
            "catch(e) { globalThis.sw = 'caught'; }");
        TEST_RESULT("Uncatchable: OOM not intercepted by JS try/catch",
                    oomRes.isError() && oomEng.globalObject().property("sw").toString() == "no",
                    QStringLiteral("JS catch swallowed the OOM (sw='%1', r='%2')")
                        .arg(oomEng.globalObject().property("sw").toString()).arg(oomRes.toString()));
        TEST_RESULT("Uncatchable: engine alive after OOM",
                    oomEng.evaluate("1+1").toInt() == 2, "engine damaged after an uncatchable error");

        // (b) Interrupt inside try/catch - the interruptHandler path (ctx signature in 0.18.0)
        QQuickJSEngine intrEng;
        intrEng.setExecutionTimeoutMs(500);
        QQuickJSValue intrRes = intrEng.evaluate(
            "globalThis.sw = 'no'; "
            "try { while(true) {} } catch(e) { globalThis.sw = 'caught'; }");
        intrEng.setExecutionTimeoutMs(0);   // pattern from "Limit: Absolute Timeout Deadline"
        TEST_RESULT("Uncatchable: interrupt not intercepted by JS try/catch",
                    intrRes.isError() && intrEng.globalObject().property("sw").toString() == "no",
                    QStringLiteral("JS catch swallowed the interrupt (sw='%1', r='%2')")
                        .arg(intrEng.globalObject().property("sw").toString()).arg(intrRes.toString()));
        TEST_RESULT("Uncatchable: engine alive after the interrupt",
                    intrEng.evaluate("1+1").toInt() == 2, "engine damaged after the interrupt");

        // (c) Control point: stack overflow in 0.18.0 is a catchable RangeError
        //     (the message is pinned: the "Translate: maximum call stack" test depends on it)
        QQuickJSEngine soEng;
        QQuickJSValue soRes = soEng.evaluate(
            "globalThis.sw = 'no'; globalThis.soMsg = ''; "
            "try { function f(){ f(); } f(); } "
            "catch(e) { globalThis.sw = 'caught:' + e.name; globalThis.soMsg = e.message; }");
        bool soOk = !soRes.isError()
                 && soEng.globalObject().property("sw").toString() == "caught:RangeError"
                 && soEng.globalObject().property("soMsg").toString().contains("Maximum call stack size exceeded");
        TEST_RESULT("Uncatchable: stack overflow stays a catchable RangeError",
                    soOk,
                    QString("sw='%1', msg='%2'")
                        .arg(soEng.globalObject().property("sw").toString())
                        .arg(soEng.globalObject().property("soMsg").toString()));

        // (d) An explicit InternalError from JS is uncatchable too: pinning the semantics for script authors
        QQuickJSEngine ieEng;
        QQuickJSValue ieRes = ieEng.evaluate(
            "globalThis.sw = 'no'; "
            "try { throw new InternalError('native'); } catch(e) { globalThis.sw = 'caught'; }");
        TEST_RESULT("Uncatchable: InternalError from JS not intercepted by try/catch",
                    ieRes.isError() && ieEng.globalObject().property("sw").toString() == "no",
                    QString("sw='%1', r='%2'")
                        .arg(ieEng.globalObject().property("sw").toString()).arg(ieRes.toString()));
        TEST_RESULT("Uncatchable: engine alive after InternalError from JS",
                    ieEng.evaluate("1+1").toInt() == 2, "engine damaged");

        // (e) OOM inside a promise reaction: .catch() is not invoked, the error reaches
        //     the job drain - the JS_ExecutePendingJob loop must take it with JS_GetException,
        //     otherwise the sticky exception poisons every subsequent evaluate (critical for 24/7)
        QQuickJSEngine promEng;
        promEng.setMemoryLimit(1 * 1024 * 1024);
        promEng.evaluate(
            "globalThis.pstate = 'init'; "
            "Promise.resolve().then(function() { "
            "    globalThis.pstate = 'ran'; "
            "    let a = []; while(true) a.push('x'); "
            "}).catch(function(e) { globalThis.pstate = 'caught'; });");
        TEST_RESULT("Uncatchable: OOM in promise does not go to .catch()",
                    promEng.globalObject().property("pstate").toString() == "ran",
                    QStringLiteral("pstate='%1' - .catch() swallowed the uncatchable OOM")
                        .arg(promEng.globalObject().property("pstate").toString()));
        TEST_RESULT("Uncatchable: engine alive after OOM in promise",
                    promEng.evaluate("1+1").toInt() == 2,
                    "a sticky exception after OOM in the job queue poisoned the engine");

        // (f) The other side of the contract: a C++ exception stays CATCHABLE from JS
        QQuickJSEngine cxxEng;
        TestObject* obj = new TestObject();
        cxxEng.globalObject().setProperty("testObj", cxxEng.newQObject(obj));
        QQuickJSValue cxxRes = cxxEng.evaluate(
            "globalThis.caughtMsg = ''; "
            "try { testObj.throwCxxException(); } catch(e) { globalThis.caughtMsg = e.message; } "
            "globalThis.caughtMsg");
        TEST_RESULT("Uncatchable: C++ exception stays catchable from JS",
                    !cxxRes.isError() && cxxRes.toString().contains("C++ Exception in TestObject"),
                    QStringLiteral("catch did not run or the message was lost (r='%1')").arg(cxxRes.toString()));
        delete obj;

        // (g) An unknown C++ exception type (not std::exception) - the second half of
        //     the policy: it becomes an InternalError (uncatchable), JS try/catch cannot swallow it
        QQuickJSEngine unkEng;
        TestObject* unkObj = new TestObject();
        unkEng.globalObject().setProperty("testObj", unkEng.newQObject(unkObj));
        QQuickJSValue unkRes = unkEng.evaluate(
            "globalThis.sw = 'no'; "
            "try { testObj.throwUnknownException(); } catch(e) { globalThis.sw = 'caught'; }");
        TEST_RESULT("Uncatchable: unknown C++ exception not intercepted by JS try/catch",
                    unkRes.isError() && unkEng.globalObject().property("sw").toString() == "no",
                    QStringLiteral("JS catch swallowed the unknown C++ exception (sw='%1', r='%2')")
                        .arg(unkEng.globalObject().property("sw").toString()).arg(unkRes.toString()));
        TEST_RESULT("Uncatchable: unknown C++ exception yields InternalError",
                    unkRes.isError() && unkRes.property("name").toString() == QLatin1String("InternalError")
                        && unkRes.toString().contains("Unknown C++ exception in meta-call"),
                    QString("r='%1', name='%2'").arg(unkRes.toString()).arg(unkRes.property("name").toString()));
        TEST_RESULT("Uncatchable: engine alive after unknown C++ exception",
                    unkEng.evaluate("1+1").toInt() == 2, "engine damaged");
        delete unkObj;
    }

    // fromException should preserve throw null / throw undefined
    {
        QQuickJSEngine qEng;
        QString msg;
        bool got = false;
        QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString& m, int) { got = true; msg = m; });

        QQuickJSValue rNull = qEng.evaluate("throw null;");
        TEST_RESULT("Throw Null: Recognized as error", rNull.isError(), "Thrown value lost");
        TEST_RESULT("Throw Null: Value preserved", rNull.isNull(), "Null value not preserved");
        TEST_RESULT("Throw Null: Signal carries 'null'",
                    got && msg == QStringLiteral("null"),
                    QString("msg='%1'").arg(msg));

        QQuickJSValue rUndef = qEng.evaluate("throw undefined;");
        TEST_RESULT("Throw Undefined: Recognized as error", rUndef.isError(), "Thrown value lost");
    }

    // Method descriptors should not be writable or configurable
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

        QQuickJSValue d = qEng.evaluate("Object.getOwnPropertyDescriptor(testObj, 'add')");
        TEST_RESULT("Method Descriptor: writable=false",
                    d.isObject() && !d.property("writable").toBool(),
                    QString("writable=%1").arg(d.property("writable").toBool()));
        TEST_RESULT("Method Descriptor: configurable=false",
                    d.isObject() && !d.property("configurable").toBool(),
                    QString("configurable=%1").arg(d.property("configurable").toBool()));

        QQuickJSValue del = qEng.evaluate("delete testObj.add");
        TEST_RESULT("Method Deletion: Returns false and method stays",
                    !del.toBool() && qEng.evaluate("typeof testObj.add").toString() == "function",
                    QString("delete returned %1").arg(del.toBool()));
        delete obj;
    }

    // connect() on a non-signal should report 'not a signal'
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        QQuickJSValue e = qEng.evaluate("testObj.valueChanged.connect.call(testObj.add, function(){})");
        QString s = e.toString();
        TEST_RESULT("Connect Non-Signal: Reports 'not a signal'",
                    e.isError() && s.contains("signal", Qt::CaseInsensitive) && !s.contains("deleted"),
                    QString("msg='%1' (object is alive - misleading message)").arg(s));
        delete obj;
    }

    // Silent data loss during conversion should be diagnosed
    {
        QQuickJSEngine qEng;
        {
            WarningCaptureGuard capture;
            QVariant huge = qEng.evaluate("new Array(1500000).fill(0)").toVariant();
            QVariant throwing = qEng.evaluate("({get x(){ throw new Error('boom'); }})").toVariant();
            Q_UNUSED(huge); Q_UNUSED(throwing);
        }

        TEST_RESULT("Conversion Data Loss: Diagnosed with qWarning",
                    !g_capturedWarnings.isEmpty(),
                    QStringLiteral("No warnings emitted (array>1e6 and throwing getter silently return empty values)"));
    }

    // Invalid Date should convert to invalid QVariant
    {
        QQuickJSEngine qEng;
        QQuickJSValue d = qEng.evaluate("new Date(NaN)");
        QVariant v = d.toVariant();
        TEST_RESULT("Invalid Date: Converts to invalid QVariant",
                    !v.isValid() || !v.toDateTime().isValid(),
                    "Invalid Date turned into a valid date");
    }

    // Template setProperty overloads must reject exception values
    {
        QQuickJSEngine qEng;

        // QString overload (char* overload is already covered by existing tests)
        QVariant deep;
        for (int i = 0; i < 70; ++i) { QVariantMap m; m.insert(QStringLiteral("next"), deep); deep = QVariant(m); }
        qEng.setProperty(QStringLiteral("deepVar2"), deep);
        TEST_RESULT("Deep Variant: QString overload rejects exception value",
                    qEng.evaluate("typeof deepVar2").toString() == "undefined",
                    "the exception value leaked through the QString overload");

        // Template overload: QVariantMap argument -> template<T=QVariantMap> wins
        QVariantMap deepMap;
        for (int i = 0; i < 70; ++i) { QVariantMap m; m.insert(QStringLiteral("next"), deepMap); deepMap = m; }
        QQuickJSValue obj = qEng.newObject();
        obj.setProperty(QStringLiteral("deepProp"), deepMap);
        TEST_RESULT("Deep Variant: template setProperty rejects exception value",
                    !obj.hasProperty(QStringLiteral("deepProp")),
                    "the exception value was written through the template overload");

        // Template arrayIndex overload
        QQuickJSValue arr = qEng.newArray();
        arr.setProperty(0U, deepMap);
        TEST_RESULT("Deep Variant: template arrayIndex setProperty rejects exception value",
                    arr.property(0U).isUndefined(),
                    "the exception value was written into the array through the template overload");

        TEST_RESULT("Deep Variant: Engine alive", qEng.evaluate("1+1").toInt() == 2, "engine damaged");
    }

    // 18. DANGEROUS REGRESSION TESTS
    // These tests may crash the application on unpatched code.
    // Run with: ./tests (without --skip-dangerous)
    qInfo().noquote() << QStringLiteral("\n=== INTERMEDIATE RESULTS (SAFE TESTS) ===");
    qInfo().noquote() << QString::fromUtf8("PASSED: %1 | FAILED: %2").arg(passed).arg(failed);
    qInfo().noquote() << QStringLiteral("\n--- Dangerous Regression Tests ---\n");

    if (!runDangerous) {
        qInfo().noquote() << QStringLiteral("Dangerous regression tests skipped (--skip-dangerous)");
    } else {

        // Note: The "Zombie value" test was removed because it violates QuickJS's fundamental
        // C-API contract: all JSValues MUST be freed before JS_FreeRuntime is called.
        // Testing this scenario causes an unavoidable assertion failure in QuickJS debug builds.

        // Foreign engine value should be rejected by setProperty
        {
            qInfo().noquote() << QStringLiteral(">>> Foreign engine value (heap corruption on unpatched code)");
            QQuickJSEngine engA;
            QQuickJSEngine engB;
            QQuickJSValue foreign = engB.evaluate("({tag: 'foreign'})");
            engA.setProperty("foreignVal", foreign);
            QQuickJSValue t = engA.evaluate("typeof foreignVal");
            TEST_RESULT("Foreign Value: Rejected by setProperty",
                        t.toString() == "undefined",
                        QString("typeof='%1' - Foreign runtime value entered engine A").arg(t.toString()));
        }

        // Unregistered types in method return and property read
        {
            qInfo().noquote() << QStringLiteral(">>> Unregistered types in return/read (stack corruption on unpatched code)");
            QQuickJSEngine qEng;
            TestObject* obj = new TestObject();
            qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

            QQuickJSValue r1 = qEng.evaluate("testObj.uProp");
            TEST_RESULT("UnregisteredType: Property read is safe",
                        r1.isUndefined() || r1.isError(),
                        QString("Got '%1' from corrupted stack").arg(r1.toString()));

            QQuickJSValue r2 = qEng.evaluate("testObj.makeUnregistered()");
            TEST_RESULT("UnregisteredType: Method return throws TypeError",
                        r2.isError(),
                        QString("Got '%1' without error (stack corruption)").arg(r2.toString()));
            delete obj;
        }

        // setProperty(nullptr, ...) should be safe
        {
            qInfo().noquote() << QStringLiteral(">>> setProperty(nullptr) (segfault on unpatched code)");
            QQuickJSEngine qEng;
            QQuickJSValue val = qEng.newObject();
            qEng.setProperty(static_cast<const char*>(nullptr), val);
            TEST_RESULT("Nullptr SetProperty: Safe",
                        qEng.evaluate("1+1").toInt() == 2, "Crash or engine damaged");
        }
    }

    // A primitive zombie value: JS_TAG_NULL without refcount - engA tears down cleanly,
    // but the engine-identity guard checks engine identity, not the value tag.
    {
        QQuickJSValue zombie;
        {
            QQuickJSEngine engA;
            zombie = engA.evaluate("null");   // a primitive; refcount operations are no-ops
        } // engA destroyed cleanly, gc_obj_list is empty

        QQuickJSEngine engB;
        QQuickJSValue func = engB.evaluate("(function(x) { return typeof x; })");
        QQuickJSValue res = func.call(QList<QQuickJSValue>{zombie});
        TEST_RESULT("Zombie (primitive): Rejected by call()",
                    !res.isValid(),
                    QStringLiteral("result='%1' - the zombie value was accepted as an argument").arg(res.toString()));

        engB.setProperty("zombieProp", zombie);
        TEST_RESULT("Zombie (primitive): Rejected by setProperty",
                    engB.evaluate("typeof zombieProp").toString() == "undefined",
                    "the zombie value was written to a global");
    }

    {
        QQuickJSEngine qEng;
        QVariantMap m;
        m.insert(QString::fromUtf8("a\0b", 3), 7);
        qEng.setProperty("nulMap", QVariant(m));
        TEST_RESULT("AtomCache: embedded NUL key preserved",
                    qEng.evaluate("Object.keys(nulMap)[0].length").toInt() == 3,
                    "key truncated at '\\0'");   // the old code produced 1
    }

    {
        QQuickJSEngine qEng;
        QQuickJSValue obj = qEng.newObject();
        obj.setProperty(QStringLiteral("alpha"), 1);
        obj.setProperty(QStringLiteral("alpha"), 2);            // repeated name -> cache hit
        TEST_RESULT("AtomCache: setProperty(QString) cached path",
                    obj.property(QStringLiteral("alpha")).toInt() == 2 && obj.hasProperty(QStringLiteral("alpha")),
                    "repeated names break setProperty/hasProperty");

        QQuickJSValue err = qEng.evaluate("throw new Error('x');");
        TEST_RESULT("AtomCache: error lineNumber path intact",
                    err.property(QStringLiteral("lineNumber")).toInt() > 0,
                    "the lineNumber special path is broken");

        TEST_RESULT("AtomCache: deleteProperty(QString) works",
                    obj.deleteProperty(QStringLiteral("alpha")) && !obj.hasProperty(QStringLiteral("alpha")),
                    "delete via the cache is broken");
    }

    // 19. Final Engine Stats
    qInfo().noquote() << QStringLiteral("\n=== ENGINE INFO ===");
    qInfo().noquote() << QStringLiteral("QuickJS Version:") << QQuickJSEngine::version();
    {
        QQuickJSEngine statsEngine;
        statsEngine.evaluate("let a = []; for(let i=0; i<1000; i++) a.push({x:i, y:'str'});");
        statsEngine.collectGarbage();
        JSMemoryUsage mem = statsEngine.memoryUsage();
        qInfo().noquote() << QStringLiteral("Memory Usage:");
        qInfo().noquote() << QStringLiteral("  Memory Limit:") << mem.malloc_limit << "bytes";
        qInfo().noquote() << QStringLiteral("  Used:") << mem.memory_used_size << "bytes";
        qInfo().noquote() << QStringLiteral("  Allocated:") << mem.malloc_size << "bytes";
        qInfo().noquote() << QStringLiteral("  Atoms:") << mem.atom_count << "(" << mem.atom_size << "bytes)";
        qInfo().noquote() << QStringLiteral("  Strings:") << mem.str_count << "(" << mem.str_size << "bytes)";
        qInfo().noquote() << QStringLiteral("  Objects:") << mem.obj_count << "(" << mem.obj_size << "bytes)";
        qInfo().noquote() << QStringLiteral("  Properties:") << mem.prop_count << "(" << mem.prop_size << "bytes)";
        qInfo().noquote() << QStringLiteral("  Shapes:") << mem.shape_count << "(" << mem.shape_size << "bytes)";
    }

    qInfo().noquote() << QStringLiteral("\n=== FINAL RESULTS ===");
    qInfo().noquote() << QString::fromUtf8("Total tests: %1 | PASSED: %2 | FAILED: %3 | WARNINGS: %4")
                             .arg(passed + failed).arg(passed).arg(failed).arg(warnings);
}

// ------------------------------------------------------------------------------------------------
// Main Test Scenario
// ------------------------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    QCoreApplication a(argc, argv);

    const bool runDangerous = !a.arguments().contains(QStringLiteral("--skip-dangerous"));

    // Check whether the serial-run mode is enabled via the .pro file
#ifdef SERIAL_TEST_RUNS
    int runs = SERIAL_TEST_RUNS;
#else
    int runs = 1;
#endif

    int grandTotalFailed = 0;

    for (int run = 1; run <= runs; ++run) {
        if (runs > 1) {
            qInfo().noquote() << QStringLiteral("\n========== SERIAL RUN %1 / %2 ==========").arg(run).arg(runs);
        }

        // Reset the counters before each run
        passed = 0;
        failed = 0;
        warnings = 0;
        g_capturedWarnings.clear();

        // Run the tests
        runAllTests(runDangerous);

        grandTotalFailed += failed;
    }

    if (runs > 1) {
        qInfo().noquote() << QStringLiteral("\n=== SERIAL RUNS COMPLETE ===");
        qInfo().noquote() << QStringLiteral("Total failed tests across all runs: %1").arg(grandTotalFailed);
    }

    return (grandTotalFailed > 0) ? 1 : 0;
}

#include "main.moc"
