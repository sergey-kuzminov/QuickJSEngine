#include <QCoreApplication>
#include <QJSEngine>
#include <QJSValueIterator>
#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTime>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>

#include "QuickJSEngine.h"
#include "QuickJSValueIterator.h"
#include "JsErrorTranslator.h"

// Compile-time parity of the shared enums with QJSEngine (this test binary
// links both engines, so any drift fails the build).
static_assert(static_cast<int>(QQuickJSValue::NoError) == static_cast<int>(QJSValue::NoError));
static_assert(static_cast<int>(QQuickJSValue::GenericError) == static_cast<int>(QJSValue::GenericError));
static_assert(static_cast<int>(QQuickJSValue::EvalError) == static_cast<int>(QJSValue::EvalError));
static_assert(static_cast<int>(QQuickJSValue::RangeError) == static_cast<int>(QJSValue::RangeError));
static_assert(static_cast<int>(QQuickJSValue::ReferenceError) == static_cast<int>(QJSValue::ReferenceError));
static_assert(static_cast<int>(QQuickJSValue::SyntaxError) == static_cast<int>(QJSValue::SyntaxError));
static_assert(static_cast<int>(QQuickJSValue::TypeError) == static_cast<int>(QJSValue::TypeError));
static_assert(static_cast<int>(QQuickJSValue::URIError) == static_cast<int>(QJSValue::URIError));
static_assert(static_cast<int>(QQuickJSEngine::TranslationExtension) == static_cast<int>(QJSEngine::TranslationExtension));
static_assert(static_cast<int>(QQuickJSEngine::ConsoleExtension) == static_cast<int>(QJSEngine::ConsoleExtension));
static_assert(static_cast<int>(QQuickJSEngine::GarbageCollectionExtension) == static_cast<int>(QJSEngine::GarbageCollectionExtension));

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
    Q_PROPERTY(QTime timeProp READ getTimeProp WRITE setTimeProp)
    Q_PROPERTY(qint64 bigProp READ getBigProp WRITE setBigProp)
    Q_PROPERTY(float floatProp READ getFloatProp WRITE setFloatProp)
    Q_PROPERTY(uint uintProp READ getUintProp WRITE setUintProp)
    Q_PROPERTY(UnregisteredType uProp READ getUProp)
    Q_PROPERTY(UnregisteredType uPropW WRITE setUPropW)
    // Narrow scalar properties with negative values: regression coverage
    // for the exact-width metacall reads (a char read through a short
    // buffer turns -1 into 255; a 4-byte LLP64 long read through a qint64
    // buffer loses the sign).
    Q_PROPERTY(char charProp READ getCharProp CONSTANT)
    Q_PROPERTY(signed char scharProp READ getSCharProp CONSTANT)
    Q_PROPERTY(short shortProp READ getShortProp CONSTANT)
    Q_PROPERTY(long longProp READ getLongProp CONSTANT)
    Q_PROPERTY(unsigned long ulongProp READ getULongProp CONSTANT)
    Q_PROPERTY(short shortPropW READ getShortPropW WRITE setShortPropW)

signals:
    void valueChanged(int newValue);
    void nameChanged(const QString& newName);
    void multiArgSignal(int a, const QString& b, double c);
    void narrowSignal(char c, long l, unsigned long ul);

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
    Q_INVOKABLE float echoFloat(float v) { return v; }
    Q_INVOKABLE uint echoUint(uint v) { return v; }
    Q_INVOKABLE char getChar() { return -1; }
    Q_INVOKABLE signed char getSChar() { return -2; }
    Q_INVOKABLE short getShort() { return -3; }
    Q_INVOKABLE long getLong() { return -4; }
    Q_INVOKABLE unsigned long getULong() { return 5; }
    Q_INVOKABLE void emitNarrowSignal() { emit narrowSignal(-1, -2, 5); }
    Q_INVOKABLE void emitMultiArg() { emit multiArgSignal(42, QStringLiteral("test"), 3.14); }
    Q_INVOKABLE void throwCxxExceptionInSignal() { throw std::runtime_error("C++ Signal Exception"); }
    Q_INVOKABLE void throwUnknownException() { throw 42; }   // not a std::exception -> uncatchable InternalError policy

    Q_INVOKABLE QStringList manyArgs(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10) {
        QStringList l;
        l << QString::number(a1) << QString::number(a2) << QString::number(a3) << QString::number(a4) << QString::number(a5)
          << QString::number(a6) << QString::number(a7) << QString::number(a8) << QString::number(a9) << QString::number(a10);
        return l;
    }

    // Argument-count and overload-selection coverage (V4 parity): no
    // defaults (missing arguments must throw), C++ defaults (moc emits
    // truncated synthetic signatures), and a same-name int/QString
    // overload pair.
    Q_INVOKABLE QString needTwo(int a, const QString& b) { return QString::number(a) + QLatin1Char(',') + b; }
    Q_INVOKABLE QString withDefaults(int a, int b = 7) { return QString::number(a) + QLatin1Char(',') + QString::number(b); }
    Q_INVOKABLE QString ovOverload(int v) { return QStringLiteral("int:%1").arg(v); }
    Q_INVOKABLE QString ovOverload(const QString& v) { return QStringLiteral("str:%1").arg(v); }
    Q_INVOKABLE QDateTime invalidDateTime() { return QDateTime(); }
    Q_INVOKABLE QDate invalidDate() { return QDate(); }
    Q_INVOKABLE QTime invalidTime() { return QTime(); }
    Q_INVOKABLE QTime echoTime(const QTime& t) { return t; }
    Q_INVOKABLE QDate echoDate(const QDate& d) { return d; }
    Q_INVOKABLE QDateTime echoDateTime(const QDateTime& dt) { return dt; }

    int getValue() const { return m_value; }
    void setValue(int v) { m_value = v; emit valueChanged(v); }
    void setName(const QString& n) { m_name = n; emit nameChanged(n); }
    int getReadOnlyProp() const { return 999; }

    QVariant getVariantProp() const { return m_variantProp; }
    void setVariantProp(const QVariant& v) { m_variantProp = v; }
    QDate getDateProp() const { return m_dateProp; }
    void setDateProp(const QDate& d) { m_dateProp = d; }
    QTime getTimeProp() const { return m_timeProp; }
    void setTimeProp(const QTime& t) { m_timeProp = t; }

    qint64 getBigProp() const { return m_bigProp; }
    void setBigProp(qint64 v) { m_bigProp = v; }

    float getFloatProp() const { return m_floatProp; }
    void setFloatProp(float v) { m_floatProp = v; }
    uint getUintProp() const { return m_uintProp; }
    void setUintProp(uint v) { m_uintProp = v; }

    char getCharProp() const { return -1; }
    signed char getSCharProp() const { return -2; }
    short getShortProp() const { return -3; }
    long getLongProp() const { return -4; }
    unsigned long getULongProp() const { return 5; }
    short getShortPropW() const { return m_shortPropW; }
    void setShortPropW(short v) { m_shortPropW = v; }

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

    // Width-regression enums: a quint64-backed and a qint8-backed enum class,
    // each exposed as a property, a method parameter and a return value. moc
    // reads and writes sizeof(declared type) bytes per metacall slot, so any
    // dispatch path that rewrites these to QMetaType::Int corrupts the stack
    // (an 8-byte write into a 4-byte buffer) or loses the sign (a 1-byte
    // write read out of a zeroed 4-byte buffer as 255).
    enum class Wide : quint64 { None = 0, Big = 1ULL << 40, Bigger = 1ULL << 42 };
    Q_ENUM(Wide)
    enum class Narrow : qint8 { Neg = -1, Pos = 1 };
    Q_ENUM(Narrow)
    Q_PROPERTY(Wide wideProp READ getWideProp WRITE setWideProp)
    Q_PROPERTY(Narrow narrowProp READ getNarrowProp WRITE setNarrowProp)
    Q_INVOKABLE Wide echoWide(Wide v) const { return v; }
    Q_INVOKABLE Narrow echoNarrow(Narrow v) const { return v; }
    Q_INVOKABLE quint64 wideAsInt(Wide v) const { return static_cast<quint64>(v); }
    Q_INVOKABLE int narrowAsInt(Narrow v) const { return static_cast<int>(v); }
    Wide getWideProp() const { return m_wide; }
    void setWideProp(Wide v) { m_wide = v; }
    Narrow getNarrowProp() const { return m_narrow; }
    void setNarrowProp(Narrow v) { m_narrow = v; }

    Q_INVOKABLE int secretCalls() const { return m_secretCalls; }
    Q_INVOKABLE int unregArgs() const { return m_unregArgs; }
    void setJsEngine(QQuickJSEngine* e) { m_jsEngine = e; }

    Q_INVOKABLE int selfInterruptThenEval() {
        if (!m_jsEngine) return -1;
        m_jsEngine->setInterrupted(true);
        QQuickJSValue r = m_jsEngine->evaluate(QStringLiteral("1+1"));
        return r.isError() ? -2 : r.toInt();
    }

    // Nested evaluate() probe (r13, D-62): invoked from JS that runs inside engine
    // entries - the pending-exception report (a hostile "stack" getter) or a
    // script-installed global setter behind setProperty(). The nested entry
    // must be a NESTED entry: it may not drain the microtask queue
    // (run-to-completion belongs to the outer top-level tick) and must not
    // re-arm the guard/quota machinery.
    Q_INVOKABLE int reenterEval(const QString& code) {
        if (!m_jsEngine) return -1;
        QQuickJSValue r = m_jsEngine->evaluate(code);
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
    Wide m_wide = Wide::Big;
    Narrow m_narrow = Narrow::Neg;
    TestObject* m_child;
    QVariant m_variantProp;
    QDate m_dateProp;
    QTime m_timeProp{13, 45, 36, 150};
    qint64 m_bigProp = 0;
    float m_floatProp = 0.0f;
    uint m_uintProp = 0;
    short m_shortPropW = 0;
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
    Q_PROPERTY(QString strProp READ strProp WRITE setStrProp)
    Q_PROPERTY(QByteArray baProp READ baProp WRITE setBaProp)
    Q_PROPERTY(float floatProp READ floatProp WRITE setFloatProp)
    Q_PROPERTY(QStringList slProp READ slProp WRITE setSlProp)
    Q_PROPERTY(QVariant varProp READ varProp WRITE setVarProp)
    Q_PROPERTY(QDateTime dtProp READ dtProp WRITE setDtProp)
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
    QString strProp() const { return m_str; }
    void setStrProp(const QString& v) { m_str = v; }
    QByteArray baProp() const { return m_ba; }
    void setBaProp(const QByteArray& v) { m_ba = v; }
    float floatProp() const { return m_float; }
    void setFloatProp(float v) { m_float = v; }
    QStringList slProp() const { return m_sl; }
    void setSlProp(const QStringList& v) { m_sl = v; }
    QVariant varProp() const { return m_var; }
    void setVarProp(const QVariant& v) { m_var = v; }
    QDateTime dtProp() const { return m_dt; }
    void setDtProp(const QDateTime& v) { m_dt = v; }

    // Argument-path null/undefined coercion probes (V4 parity)
    Q_INVOKABLE void takeStr(const QString& s) { m_str = s; }
    Q_INVOKABLE void takeBa(const QByteArray& b) { m_ba = b; }
    // A no-op sink whose argument coercion runs user JS (valueOf)
    Q_INVOKABLE int sink(int v) { return v; }
    // Float twin of sink() for the valueOf-propagation coverage (r13, D-62)
    Q_INVOKABLE float sinkFlt(float v) { return v; }

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
    QString m_str = QStringLiteral("INIT");
    QByteArray m_ba = "INIT";
    float m_float = -1.0f;
    QStringList m_sl = { QStringLiteral("x") };
    QVariant m_var = 7;
    QDateTime m_dt = QDateTime(QDate(2020, 1, 1), QTime(1, 2, 3));
    QQuickJSEngine* m_jsEngine = nullptr;
};

// Deletes a target QObject on demand from JS (H-3 regression coverage:
// the deletion happens DURING the argument/value conversion of another
// wrapper's call or property write).
class DeleterObject : public QObject {
    Q_OBJECT
public:
    QObject* m_target = nullptr;
    Q_INVOKABLE void deleteTarget() { delete m_target; }
};

// ------------------------------------------------------------------------------------------------
// Dual-engine object for throwError() parity scenarios: each instance is
// exposed to exactly one engine, and every invokable acts on that engine.
// ------------------------------------------------------------------------------------------------
class ThrowProbeObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(int throwingProp READ getThrowingProp WRITE setThrowingProp)
public:
    QJSEngine* qtEngine = nullptr;
    QQuickJSEngine* qjsEngine = nullptr;
    bool sawError = false;

    Q_INVOKABLE void fail() {
        if (qtEngine) qtEngine->throwError(QStringLiteral("slot boom"));
        if (qjsEngine) qjsEngine->throwError(QStringLiteral("slot boom"));
    }
    Q_INVOKABLE void failTyped() {
        if (qtEngine) qtEngine->throwError(QJSValue::RangeError, QStringLiteral("slot range"));
        if (qjsEngine) qjsEngine->throwError(QQuickJSValue::RangeError, QStringLiteral("slot range"));
    }
    Q_INVOKABLE void failValue() {
        if (qtEngine) {
            QJSValue e = qtEngine->newErrorObject(QJSValue::TypeError, QStringLiteral("typed boom"));
            e.setProperty(QStringLiteral("code"), 42);
            qtEngine->throwError(e);
        } else if (qjsEngine) {
            QQuickJSValue e = qjsEngine->newErrorObject(QQuickJSValue::TypeError, QStringLiteral("typed boom"));
            e.setProperty(QStringLiteral("code"), 42);
            qjsEngine->throwError(e);
        }
    }
    Q_INVOKABLE void throwDefault() {
        if (qtEngine) qtEngine->throwError(QJSValue());
        if (qjsEngine) qjsEngine->throwError(QQuickJSValue());
    }
    Q_INVOKABLE void failAndRecord() {
        // throwError() first: hasError() must already be true inside the slot.
        if (qtEngine) { qtEngine->throwError(QStringLiteral("record boom")); sawError = qtEngine->hasError(); }
        if (qjsEngine) { qjsEngine->throwError(QStringLiteral("record boom")); sawError = qjsEngine->hasError(); }
    }
    Q_INVOKABLE void emitPing() { emit ping(); }

    // Inner evaluate() from inside a slot: used by the nested-stack test —
    // the stack of the inner error must keep the outer script frames.
    QStringList innerStack;
    Q_INVOKABLE void runInnerEval() {
        innerStack.clear();
        if (qjsEngine) {
            qjsEngine->evaluate(QStringLiteral(
                "function deep(n) { if (n === 0) throw new Error('inner boom'); return deep(n - 1); }\n"
                "deep(1);"), QStringLiteral("inner.js"), 1, &innerStack);
        }
    }

    // Nested evaluate() with runaway recursion from inside a slot: the stack
    // guard must keep it a catchable error (Windows MinGW Debug+ASan crash
    // class: a tight budget anchored at engine creation made every script
    // throw on entry, and the recursion could exhaust the process stack).
    bool innerCaught = false;
    Q_INVOKABLE void runDeepEval() {
        innerCaught = false;
        if (qjsEngine) {
            innerCaught = qjsEngine->evaluate(
                QStringLiteral("function f(){f();} f();")).isError();
        }
    }

    int getThrowingProp() const {
        if (qtEngine) qtEngine->throwError(QStringLiteral("getter boom"));
        if (qjsEngine) qjsEngine->throwError(QStringLiteral("getter boom"));
        return 0;
    }
    void setThrowingProp(int) {
        if (qtEngine) qtEngine->throwError(QStringLiteral("setter boom"));
        if (qjsEngine) qjsEngine->throwError(QStringLiteral("setter boom"));
    }

signals:
    void ping();
};

// ------------------------------------------------------------------------------------------------
// Test Helpers
// ------------------------------------------------------------------------------------------------
int passed = 0, failed = 0, warnings = 0;

static QStringList g_capturedWarnings;
static void captureWarningHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    if (type == QtWarningMsg) g_capturedWarnings.append(msg);
}

static QStringList g_capturedMessages;
static void captureAllMessagesHandler(QtMsgType, const QMessageLogContext&, const QString& msg) {
    g_capturedMessages.append(msg);
}

// RAII guard: routes every message type (console output verification) into
// g_capturedMessages while in scope and always restores the previous handler
class MessageCaptureGuard {
public:
    MessageCaptureGuard() : m_oldHandler(qInstallMessageHandler(captureAllMessagesHandler)) { g_capturedMessages.clear(); }
    ~MessageCaptureGuard() { qInstallMessageHandler(m_oldHandler); }
    MessageCaptureGuard(const MessageCaptureGuard&) = delete;
    MessageCaptureGuard& operator=(const MessageCaptureGuard&) = delete;
private:
    QtMessageHandler m_oldHandler;
};

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

// Runs the same JS with a ThrowProbeObject exposed as "obj" in both engines
// and compares the outcome (stage 3: throwError parity).
static void compareThrowEngines(const char* name, const char* code) {
    QJSEngine qtEng;
    QQuickJSEngine qEng;
    ThrowProbeObject qtObj, qObj;
    qtObj.qtEngine = &qtEng;
    qObj.qjsEngine = &qEng;
    qtEng.globalObject().setProperty(QStringLiteral("obj"), qtEng.newQObject(&qtObj));
    qEng.globalObject().setProperty(QStringLiteral("obj"), qEng.newQObject(&qObj));

    QJSValue qtRes = qtEng.evaluate(QString::fromUtf8(code));
    QQuickJSValue qRes = qEng.evaluate(QString::fromUtf8(code));

    const bool qtErr = qtRes.isError();
    const bool qErr = qRes.isError();

    if (qtErr && qErr) {
        TEST_RESULT(name,
                    qtRes.property(QStringLiteral("name")).toString() == qRes.property(QStringLiteral("name")).toString()
                        && qtRes.toString() == qRes.toString(),
                    QStringLiteral("error mismatch: QJSEngine='%1', QuickJS='%2'")
                        .arg(qtRes.toString(), qRes.toString()));
    } else if (qtErr != qErr) {
        QString d = QString::fromUtf8("QJSEngine err=%1 (%2), QuickJS err=%3 (%4)")
                        .arg(qtErr).arg(qtRes.toString()).arg(qErr).arg(qRes.toString());
        TEST_RESULT(name, false, d);
    } else {
        TEST_RESULT(name, qtRes.toString() == qRes.toString(),
                    QString::fromUtf8("QJSEngine='%1', QuickJS='%2'").arg(qtRes.toString(), qRes.toString()));
    }
}

// Qt reports a failed importModule() as an Error value in some builds
// (Qt 6.12: isError() + errorType()); other builds return a plain value whose
// string form is "<ErrorClass>: message". Reduce both forms to a single
// QJSValue::ErrorType id so module parity holds across Qt versions.
static int moduleFailureClass(const QJSValue& v)
{
    static const struct { QLatin1String prefix; QJSValue::ErrorType type; } kErrorPrefixes[] = {
        { QLatin1String("EvalError:"),      QJSValue::EvalError },
        { QLatin1String("RangeError:"),     QJSValue::RangeError },
        { QLatin1String("ReferenceError:"), QJSValue::ReferenceError },
        { QLatin1String("SyntaxError:"),    QJSValue::SyntaxError },
        { QLatin1String("TypeError:"),      QJSValue::TypeError },
        { QLatin1String("URIError:"),       QJSValue::URIError },
        { QLatin1String("Error:"),          QJSValue::GenericError },
    };
    if (v.isError())
        return static_cast<int>(v.errorType());
    if (v.isUndefined() || v.isNull())
        return static_cast<int>(QJSValue::NoError);
    const QString text = v.toString();
    for (const auto& e : kErrorPrefixes) {
        if (text.startsWith(e.prefix))
            return static_cast<int>(e.type);
    }
    return static_cast<int>(QJSValue::NoError);
}

// The guard's error shape: quickjs-ng raises its stack overflow (and this
// engine's starved-entry refusal deliberately mirrors it) as a RangeError
// whose message mentions the stack. isError() alone passes for ANY error
// (e.g. a TypeError from an unrelated engine failure) and would mask a
// real regression - assert the shape instead.
static bool isStackGuardError(const QQuickJSValue& v)
{
    return v.isError()
        && v.errorType() == QQuickJSValue::RangeError
        && v.toString().contains(QLatin1String("stack"));
}

// A QThread with a deliberately small stack hosting an engine that runs an
// unbounded recursion: the default recursion budget is clamped to the real
// thread bounds, so the guard must fire (a catchable error) instead of the
// OS stack limit being hit (a hard process crash).
class StackProbeThread : public QThread {
public:
    enum Outcome { NotRun = 0, GuardFired = 1, ReturnedOther = 2 };
    Outcome result = NotRun;
    void run() override {
        QQuickJSEngine eng;
        const QQuickJSValue res = eng.evaluate(QStringLiteral("function f(){f();} f();"));
        result = isStackGuardError(res) ? GuardFired : ReturnedOther;
    }
};

// A QThread whose whole stack (128 KiB) is SMALLER than the guard's safety
// margin (also 128 KiB): the entry anchor necessarily sits inside the
// margin, i.e. above = SP - floor < kSafetyMargin. Regression test for the
// unsigned underflow in the budget clamp: sp - floor - kSafetyMargin used to
// wrap around ~2^64, sail past the minimum-usable check and install the FULL
// unclamped budget (hundreds of KiB) on a 128 KiB stack - a guard line far
// below the real bottom, unreachable, so runaway recursion ran straight
// into the guard page and killed the whole process. The fixed clamp must
// recognize the starved window and refuse script entries with a catchable
// error instead (see QQuickJSEngine::evaluate, m_stackStarved); a hard
// crash in this test would have killed the suite itself, not just a check.
class StarvedStackThread : public QThread {
public:
    enum Outcome { NotRun = 0, Refused = 1, ReturnedOther = 2, Completed = 3 };
    Outcome result = NotRun;
    Outcome trivial = NotRun;   // second entry ("1+1"): refused, or legitimately run
    // The registerModule hole probed on the same starved anchor: its snapshot
    // reads user getters, so the entry refusal must apply there too. The
    // registration result (bool) and the follow-up importModule() - which
    // surfaces the pending RangeError of the refused registration - pin
    // both halves of the refusal contract.
    bool registered = true;
    Outcome imported = NotRun;
    // A pending exception set from C++ between starved entries: evaluate()
    // must consume and return it through the pure C path (JS_GetException
    // runs no script) instead of returning the refusal RangeError and
    // leaving the old error pending behind every subsequent entry.
    bool pendingConsumed = false;
    bool pendingWasRangeError = true;
    void run() override {
        QQuickJSEngine eng;
        const QQuickJSValue res = eng.evaluate(QStringLiteral("function f(){f();} f();"));
        result = isStackGuardError(res) ? Refused : ReturnedOther;
        const QQuickJSValue res2 = eng.evaluate(QStringLiteral("1 + 1"));
        trivial = res2.isError() ? Refused : ReturnedOther;
        registered = eng.registerModule(QStringLiteral("m"), eng.newObject());
        const QQuickJSValue imp = eng.importModule(QStringLiteral("m"));
        imported = isStackGuardError(imp) ? Refused
            : (imp.isError() ? ReturnedOther : Completed);
        eng.throwError(QStringLiteral("pending boom"));
        const QQuickJSValue res3 = eng.evaluate(QStringLiteral("1 + 1"));
        pendingConsumed = res3.isError() && res3.toString().contains(QLatin1String("pending boom"));
        pendingWasRangeError = isStackGuardError(res3);
    }
};

// Runs the full test suite
// Property-write/argument type-coercion contracts (r11, D-60): strict V4
// null/undefined table for meta-properties, JS coercion for arguments,
// UAF regression (H-3), dynamic-property semantics (M-2). Extracted into
// its own function: with ASan's per-scope stack slots these locals would
// inflate runAllTests' frame and push the depth-limit conversion tests
// below the reserve line on 1 MiB stacks (D-60 starved-matrix note).
static void runPropertyTypeCoercionTests()
{
// Unrepresentable values must throw TypeError instead of silently writing defaults
    QQuickJSEngine qEng;
    RegressionTestObject* obj = new RegressionTestObject();
    qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));

    // (r13, D-62): a failed numeric coercion propagates the VENDOR TypeError
    // instead of the generic "incompatible type" text (V4's double
    // set-path throws a TypeError for a Symbol as well - probed; its int
    // set-path wraps the object into a QVariant and reports a generic
    // "Cannot assign QJSValue to int" Error - the documented divergence
    // of the wrapper's ECMAScript-coercion contract).
    QQuickJSValue r1 = qEng.evaluate("testObj.intProp = Symbol()");
    TEST_RESULT("QProperty: Symbol to int throws TypeError",
                r1.isError() && r1.toString().contains("cannot convert symbol to number", Qt::CaseInsensitive) && obj->intProp() == -1,
                QStringLiteral("silent default write: r='%1' intProp=%2").arg(r1.toString()).arg(obj->intProp()));

    QQuickJSValue r2 = qEng.evaluate("testObj.doubleProp = Symbol()");
    TEST_RESULT("QProperty: Symbol to double throws TypeError",
                r2.isError() && r2.toString().contains("cannot convert symbol to number", Qt::CaseInsensitive) && obj->doubleProp() == -1.0,
                QStringLiteral("silent default write: r='%1'").arg(r2.toString()));

    QQuickJSValue r3 = qEng.evaluate("testObj.objProp = Symbol()");
    TEST_RESULT("QProperty: Symbol to QObject* throws TypeError",
                r3.isError() && r3.toString().contains("incompatible type", Qt::CaseInsensitive) && obj->objProp() == obj,
                QStringLiteral("silent nullptr write: r='%1'").arg(r3.toString()));

    QQuickJSValue r4 = qEng.evaluate("testObj.objProp = new Map()");
    TEST_RESULT("QProperty: Map to QObject* throws TypeError",
                r4.isError() && r4.toString().contains("incompatible type", Qt::CaseInsensitive) && obj->objProp() == obj,
                QStringLiteral("silent nullptr write: r='%1'").arg(r4.toString()));

    // V4 parity (probed against live Qt 6.12): property writes coerce
    // JS null/undefined through a strict table, NOT through the JS
    // valueOf/ToNumber machinery.
    // undefined: only a QVariant-typed property accepts it (an
    // invalid QVariant); everything else throws
    // "Cannot assign [undefined] to <type>".
    // null: nullable/JS-coercible types take their null value (the
    // setter IS invoked - V4 does not reset to the default); other
    // types throw "Cannot assign std::nullptr_t to <type>".
    QQuickJSValue u1 = qEng.evaluate("testObj.intProp = undefined");
    TEST_RESULT("QProperty: undefined to int throws (V4 parity)",
                u1.isError() && u1.toString().contains("Cannot assign [undefined] to int")
                    && obj->intProp() == -1,
                QStringLiteral("got='%1'").arg(u1.toString()));
    QQuickJSValue u2 = qEng.evaluate("testObj.strProp = undefined");
    TEST_RESULT("QProperty: undefined to QString throws (V4 parity)",
                u2.isError() && u2.toString().contains("Cannot assign [undefined] to QString")
                    && obj->strProp() == QStringLiteral("INIT"),
                QStringLiteral("got='%1'").arg(u2.toString()));
    QQuickJSValue u3 = qEng.evaluate("testObj.varProp = undefined; 'ok'");
    TEST_RESULT("QProperty: undefined to QVariant writes invalid variant (V4 parity)",
                !u3.isError() && !obj->varProp().isValid(),
                QStringLiteral("got='%1'").arg(u3.toString()));
    obj->setObjProp(obj);
    QQuickJSValue u4 = qEng.evaluate("testObj.objProp = undefined");
    TEST_RESULT("QProperty: undefined to QObject* throws (V4 parity)",
                u4.isError() && u4.toString().contains("Cannot assign [undefined] to QObject*")
                    && obj->objProp() == obj,
                QStringLiteral("got='%1' - undefined must not write nullptr").arg(u4.toString()));

    QQuickJSValue n1 = qEng.evaluate("testObj.intProp = null");
    TEST_RESULT("QProperty: null to int throws (V4 parity)",
                n1.isError() && n1.toString().contains("Cannot assign std::nullptr_t to int")
                    && obj->intProp() == -1,
                QStringLiteral("got='%1' - null must not coerce to 0").arg(n1.toString()));
    QQuickJSValue n2 = qEng.evaluate("testObj.dtProp = null");
    TEST_RESULT("QProperty: null to QDateTime throws (V4 parity)",
                n2.isError() && n2.toString().contains("Cannot assign std::nullptr_t to QDateTime")
                    && obj->dtProp().isValid(),
                QStringLiteral("got='%1'").arg(n2.toString()));
    QQuickJSValue n3 = qEng.evaluate("testObj.strProp = null; 'ok'");
    TEST_RESULT("QProperty: null to QString writes null QString (V4 parity)",
                !n3.isError() && obj->strProp().isNull(),
                QStringLiteral("got='%1' str='%2'").arg(n3.toString(), obj->strProp()));
    QQuickJSValue n3r = qEng.evaluate("String(testObj.strProp) + '|' + (testObj.strProp === '')");
    TEST_RESULT("QProperty: null QString reads back as empty string (V4 parity)",
                n3r.toString() == QStringLiteral("|true"),
                QStringLiteral("got='%1' - a null QString must not become JS null").arg(n3r.toString()));
    QQuickJSValue n4 = qEng.evaluate("testObj.baProp = null; 'ok'");
    TEST_RESULT("QProperty: null to QByteArray writes null QByteArray (V4 parity)",
                !n4.isError() && obj->baProp().isNull(),
                QStringLiteral("got='%1'").arg(n4.toString()));
    qEng.evaluate("testObj.boolProp = 1");
    QQuickJSValue n5 = qEng.evaluate("testObj.boolProp = null; 'ok'");
    TEST_RESULT("QProperty: null to bool writes false (V4 parity)",
                !n5.isError() && obj->boolProp() == false,
                QStringLiteral("got='%1' b=%2").arg(n5.toString()).arg(obj->boolProp()));
    QQuickJSValue n6 = qEng.evaluate("testObj.doubleProp = null; 'ok'");
    QQuickJSValue n7 = qEng.evaluate("testObj.floatProp = null; 'ok'");
    QQuickJSValue n8 = qEng.evaluate("testObj.slProp = null; 'ok'");
    TEST_RESULT("QProperty: null to double/float/QStringList writes zero/empty (V4 parity)",
                !n6.isError() && !n7.isError() && !n8.isError()
                    && obj->doubleProp() == 0.0 && obj->floatProp() == 0.0f
                    && obj->slProp().isEmpty(),
                QStringLiteral("d=%1 f=%2 sl=%3").arg(obj->doubleProp()).arg(obj->floatProp()).arg(obj->slProp().size()));
    QQuickJSValue n9 = qEng.evaluate("testObj.varProp = null; 'ok'");
    TEST_RESULT("QProperty: null to QVariant writes nullptr_t variant reading back as null (V4 parity)",
                !n9.isError() && obj->varProp().isValid()
                    && obj->varProp().metaType().id() == QMetaType::Nullptr
                    && qEng.evaluate("String(testObj.varProp)").toString() == QStringLiteral("null")
                    && qEng.evaluate("testObj.varProp === null").toBool(),
                QStringLiteral("type='%1'").arg(QString::fromLatin1(obj->varProp().metaType().name())));
    QQuickJSValue n10 = qEng.evaluate("testObj.objProp = null; 'ok'");
    TEST_RESULT("QProperty: null to QObject* writes nullptr (V4 parity)",
                !n10.isError() && obj->objProp() == nullptr,
                QStringLiteral("got='%1'").arg(n10.toString()));

    // Argument path: JS coercion (V4 parity, probed): null coerces to
    // a NULL QString/QByteArray (NOT the "null" string), undefined to
    // the "undefined" string, numbers follow ToNumber.
    qEng.evaluate("testObj.takeStr(null)");
    TEST_RESULT("QArgs: null to QString argument is a null QString (V4 parity)",
                obj->strProp().isNull(),
                QStringLiteral("got='%1' - null became the \"null\" string").arg(obj->strProp()));
    qEng.evaluate("testObj.takeStr(undefined)");
    TEST_RESULT("QArgs: undefined to QString argument is the 'undefined' string (V4 parity)",
                obj->strProp() == QStringLiteral("undefined"),
                QStringLiteral("got='%1'").arg(obj->strProp()));
    qEng.evaluate("testObj.takeBa(null)");
    TEST_RESULT("QArgs: null to QByteArray argument is a null QByteArray (V4 parity)",
                obj->baProp().isNull(),
                QStringLiteral("got='%1'").arg(QString::fromLatin1(obj->baProp())));

    // (r13, D-62): a failed numeric coercion is always user JS (valueOf/toString
    // throwing, Symbol, BigInt). V4 propagates the ORIGINAL exception -
    // probed against live Qt 6.12 on the int/double/float property writes
    // and the int/double/float method arguments; the wrapper must not
    // replace it with a generic "incompatible type" TypeError.
    {
        // Sentinels: earlier tests in this function legitimately wrote
        // 0/empty into these slots through the null-coercion table.
        qEng.evaluate(QStringLiteral("testObj.intProp = 101; testObj.doubleProp = 102.5; testObj.floatProp = 103.5;"));
        QQuickJSValue v1 = qEng.evaluate(
            "try { testObj.intProp = { valueOf() { throw new Error('Original error'); } }; 'no-throw'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QCoerce: valueOf exception propagates from an int property write",
                    v1.toString() == QStringLiteral("Original error") && obj->intProp() == 101,
                    QStringLiteral("got='%1' intProp=%2").arg(v1.toString()).arg(obj->intProp()));
        QQuickJSValue v2 = qEng.evaluate(
            "try { testObj.doubleProp = { valueOf() { throw new Error('Original error'); } }; 'no-throw'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QCoerce: valueOf exception propagates from a double property write",
                    v2.toString() == QStringLiteral("Original error") && obj->doubleProp() == 102.5,
                    QStringLiteral("got='%1' dbl=%2").arg(v2.toString()).arg(obj->doubleProp()));
        QQuickJSValue v3 = qEng.evaluate(
            "try { testObj.floatProp = { valueOf() { throw new Error('Original error'); } }; 'no-throw'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QCoerce: valueOf exception propagates from a float property write",
                    v3.toString() == QStringLiteral("Original error") && obj->floatProp() == 103.5f,
                    QStringLiteral("got='%1' flt=%2").arg(v3.toString()).arg(obj->floatProp()));
        QQuickJSValue v3b = qEng.evaluate(
            "try { testObj.sinkFlt({ valueOf() { throw new Error('Original error'); } }); 'no-throw'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QCoerce: valueOf exception propagates from a float method argument",
                    v3b.toString() == QStringLiteral("Original error"),
                    QStringLiteral("got='%1' - the user's error was replaced").arg(v3b.toString()));
        // Float object-coercion controls (V4 parity, probed): a returning
        // valueOf assigns; a valueOf-less object (and a QObject wrapper)
        // silently yields NaN - full ToNumber semantics, no TypeError.
        QQuickJSValue v3c = qEng.evaluate(
            "testObj.floatProp = { valueOf() { return 1.25; } }; testObj.floatProp;");
        TEST_RESULT("QCoerce: returning valueOf coerces in a float property write (V4 parity)",
                    v3c.toDouble() == 1.25 && obj->floatProp() == 1.25f,
                    QStringLiteral("got='%1' flt=%2").arg(v3c.toString()).arg(obj->floatProp()));
        QQuickJSValue v3d = qEng.evaluate(
            "var r1 = testObj.sinkFlt({ valueOf() { return 1.25; } });"
            "var r2 = testObj.sinkFlt({});"
            "var r3 = testObj.sinkFlt(testObj);"
            "String(r1) + ',' + String(r2) + ',' + String(r3);");
        TEST_RESULT("QCoerce: float argument coerces objects - valueOf, NaN, wrapper NaN (V4 parity)",
                    v3d.toString() == QLatin1String("1.25,NaN,NaN"),
                    QStringLiteral("got='%1'").arg(v3d.toString()));
        QQuickJSValue v4 = qEng.evaluate(
            "try { testObj.sink({ valueOf() { throw new Error('Original error'); } }); 'no-throw'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QCoerce: valueOf exception propagates from an int method argument",
                    v4.toString() == QStringLiteral("Original error"),
                    QStringLiteral("got='%1' - the user's error was replaced").arg(v4.toString()));
        QQuickJSValue v5 = qEng.evaluate(
            "try { testObj.sink({ valueOf() { throw 'plain-string'; } }); 'no-throw'; }"
            " catch (e) { typeof e + ':' + e; }");
        TEST_RESULT("QCoerce: a thrown non-Error value keeps its shape",
                    v5.toString() == QStringLiteral("string:plain-string"),
                    QStringLiteral("got='%1'").arg(v5.toString()));
        // Controls: a returning valueOf still coerces - our documented
        // ECMAScript-coercion contract (V4's int set-path rejects objects
        // outright with "Cannot assign QJSValue to int" - a pre-existing
        // intentional divergence, unchanged by the propagation fix; V4's
        // double set-path and both argument paths coerce, probed).
        QQuickJSValue v6 = qEng.evaluate("testObj.sink({ valueOf() { return 7; } });");
        TEST_RESULT("QCoerce: returning valueOf coerces in an int argument (control)",
                    v6.toInt() == 7,
                    QStringLiteral("got='%1'").arg(v6.toString()));
        QQuickJSValue v7 = qEng.evaluate("testObj.intProp = { valueOf() { return 42; } }; testObj.intProp;");
        TEST_RESULT("QCoerce: returning valueOf coerces in an int property write (control)",
                    v7.toInt() == 42 && obj->intProp() == 42,
                    QStringLiteral("got='%1' intProp=%2").arg(v7.toString()).arg(obj->intProp()));
        QQuickJSValue v8 = qEng.evaluate("testObj.doubleProp = '2.5'; testObj.doubleProp;");
        TEST_RESULT("QCoerce: string into a double property keeps generic coercion (control)",
                    v8.toDouble() == 2.5 && obj->doubleProp() == 2.5,
                    QStringLiteral("got='%1'").arg(v8.toString()));
        // BigInt into an int argument: the vendor TypeError (quickjs.c
        // "cannot convert BigInt to number") instead of a silent 1 through
        // the QVariant fallback. V4's own evaluate() cannot parse BigInt
        // literals in this position (SyntaxError, probed), so there is no
        // V4 reference for the shape; the JS-semantics answer is a throw.
        QQuickJSValue v9 = qEng.evaluate(
            "try { testObj.sink(1n); 'no-throw'; } catch (e) { e.constructor.name + '/' + e.message; }");
        TEST_RESULT("QCoerce: BigInt into an int argument throws the vendor TypeError",
                    v9.toString().contains(QLatin1String("TypeError"))
                        && v9.toString().contains(QLatin1String("cannot convert BigInt to number")),
                    QStringLiteral("got='%1' - a silent conversion happened").arg(v9.toString()));
    }

    // H-3: user JS running during argument/value conversion may
    // delete the target QObject; the metacall must be skipped with a
    // TypeError instead of going through a dangling pointer (an
    // unfixed build crashes here under ASan).
    {
        QQuickJSEngine dEng;
        auto* target = new RegressionTestObject();
        auto* target2 = new RegressionTestObject();
        DeleterObject* deleter = new DeleterObject();
        deleter->m_target = target;
        dEng.globalObject().setProperty("victim", dEng.newQObject(target));
        dEng.globalObject().setProperty("victim2", dEng.newQObject(target2));
        dEng.globalObject().setProperty("deleter", dEng.newQObject(deleter));
        QQuickJSValue c1 = dEng.evaluate(
            "try { victim.sink({ valueOf() { deleter.deleteTarget(); return 1; } }); 'ran'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QObject: method call survives deletion during argument coercion",
                    c1.toString() == QStringLiteral("C++ object has been deleted"),
                    QStringLiteral("got='%1'").arg(c1.toString()));
        TEST_RESULT("QObject: engine alive after deletion-during-call",
                    dEng.evaluate("1+1").toInt() == 2, "engine damaged");
        deleter->m_target = target2;
        QQuickJSValue c2 = dEng.evaluate(
            "try { victim2.intProp = { valueOf() { deleter.deleteTarget(); return 5; } }; 'ran'; }"
            " catch (e) { e.message; }");
        TEST_RESULT("QObject: property write survives deletion during value coercion",
                    c2.toString() == QStringLiteral("QObject has been deleted"),
                    QStringLiteral("got='%1'").arg(c2.toString()));
        TEST_RESULT("QObject: engine alive after deletion-during-write",
                    dEng.evaluate("1+1").toInt() == 2, "engine damaged");
        delete deleter;   // both targets were already deleted from JS
    }

    // Dynamic properties (wrapper extension): Symbol refuses with a
    // TypeError instead of silently DELETING the property; undefined
    // removes; null stores the null marker reading back as null.
    {
        QQuickJSEngine dEng;
        auto* dObj = new RegressionTestObject();
        dEng.globalObject().setProperty("d", dEng.newQObject(dObj));
        dEng.evaluate("d.dynX = 41");
        TEST_RESULT("QDynamic: unknown write creates a C++ dynamic property",
                    dObj->property("dynX").isValid() && dObj->property("dynX").toInt() == 41,
                    "the dynamic property was not created");
        QQuickJSValue s1 = dEng.evaluate("d.dynX = Symbol()");
        TEST_RESULT("QDynamic: Symbol to dynamic property throws instead of deleting",
                    s1.isError() && s1.toString().contains("incompatible type", Qt::CaseInsensitive)
                        && dObj->property("dynX").isValid() && dObj->property("dynX").toInt() == 41,
                    QStringLiteral("got='%1' - the property was silently deleted").arg(s1.toString()));
        QQuickJSValue s2 = dEng.evaluate("d.dynU = 'x'; d.dynU = undefined; 'ok'");
        TEST_RESULT("QDynamic: undefined removes the dynamic property",
                    !s2.isError() && !dObj->property("dynU").isValid(),
                    QStringLiteral("got='%1'").arg(s2.toString()));
        QQuickJSValue s3 = dEng.evaluate("d.dynN = null; 'ok'");
        TEST_RESULT("QDynamic: null stores the null marker reading back as JS null",
                    !s3.isError() && dObj->property("dynN").isValid()
                        && dEng.evaluate("d.dynN === null").toBool(),
                    QStringLiteral("got='%1'").arg(s3.toString()));
        delete dObj;
    }

    qEng.evaluate("testObj.objProp = testObj");
    TEST_RESULT("QProperty: QObject wrapper still assignable to QObject*", obj->objProp() == obj, "the QObject wrapper is no longer assignable");

    // V4 parity (probed): only a QObject wrapper assigns to a
    // QObject* property; other values throw instead of a silent
    // nullptr write.
    QQuickJSValue p1 = qEng.evaluate("testObj.objProp = {a:1}");
    QQuickJSValue p2 = qEng.evaluate("testObj.objProp = 42");
    TEST_RESULT("QProperty: object/number to QObject* throws (V4 parity)",
                p1.isError() && p2.isError() && obj->objProp() == obj,
                QStringLiteral("p1='%1' p2='%2' - silent nullptr write").arg(p1.toString(), p2.toString()));

    // Bool fast path keeps ECMAScript ToBoolean semantics (total, never throws)
    qEng.evaluate("testObj.boolProp = Symbol()");
    TEST_RESULT("QProperty: Symbol to bool keeps ToBoolean coercion",
                obj->boolProp() == true,
                "the bool path must not break total ToBoolean semantics (Symbol is truthy)");

    TEST_RESULT("QProperty: Engine alive after TypeErrors", qEng.evaluate("1+1").toInt() == 2, "engine damaged");
    delete obj;
}


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

        // The same end-of-tick contract for the light accessors (r14, V2):
        // the JsEntryScope of toString()/toVariant() must COVER the
        // conversion - user JS inside an object's toString(), or a getter
        // read by toVariant(), runs under the entry's bookkeeping, and a
        // rejection it raises is flushed by that same entry (the scope's
        // top-level exit). With the scope expiring before the conversion
        // (the r13 bug) the report stays buffered and reports==0.
        {
            QQuickJSEngine qEng;
            int reports = 0;
            QString lastMsg;
            QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString& m, int) { reports++; lastMsg = m; });
            QQuickJSValue obj = qEng.evaluate(
                "({ toString() { Promise.reject(new Error('toStr rejection')); return 'ok'; } })");
            reports = 0;
            const QString s = obj.toString();
            TEST_RESULT("Promise: Rejection inside toString reported by the entry",
                        s == QLatin1String("ok") && reports == 1 && lastMsg.contains("toStr rejection"),
                        QStringLiteral("s='%1' reports=%2 - the scope expired before the conversion ran").arg(s).arg(reports));

            QQuickJSValue obj2 = qEng.evaluate(
                "({ get x() { Promise.reject(new Error('getter rejection')); return 42; } })");
            reports = 0;
            const QVariant v = obj2.toVariant();
            TEST_RESULT("Promise: Rejection inside toVariant getter reported by the entry",
                        v.toMap().value("x").toInt() == 42 && reports == 1,
                        QStringLiteral("reports=%1 - the scope expired before the conversion ran").arg(reports));
        }

        // Soft ceiling of the unhandled-rejection buffer (r8): with
        // auto-processing off and a stuck job queue the buffer never drains
        // and used to grow without bound (the r5 probe's "stuck job +
        // fresh rejections" leak scenario). Past the ceiling (1024 entries)
        // the OLDEST buffered rejection is evicted and reported immediately,
        // so every rejection is still reported exactly once and the buffer
        // stays bounded.
        {
            QQuickJSEngine qEng;
            int reports = 0;
            QString firstMsg, lastMsg;
            QObject::connect(&qEng, &QQuickJSEngine::uncaughtException,
                             [&](const QString& m, int) { ++reports; if (firstMsg.isNull()) firstMsg = m; lastMsg = m; });

            qEng.setAutoProcessPromises(false);
            // Plant a never-drained job, then reject 2000 distinct promises:
            // 1024 stay buffered, 976 overflow. The overflow reports arrive
            // immediately (during the rejection tracker callback), oldest
            // first.
            const QQuickJSValue run = qEng.evaluate(QStringLiteral(
                "Promise.resolve().then(()=>{});"
                "for (let i = 0; i < 2000; ++i) Promise.reject(new Error('cap_r' + i));"));
            TEST_RESULT("Promise: Rejection buffer ceiling evicts oldest and reports it",
                        !run.isError() && reports == 976 && firstMsg.contains(QLatin1String("cap_r0")),
                        QStringLiteral("reports=%1 firstMsg='%2' - expected 976 immediate overflow "
                                       "reports starting with the oldest (cap_r0)")
                            .arg(reports).arg(firstMsg));

            // Re-enabling auto-processing drains the stuck job and flushes the
            // 1024 buffered entries: total must be exactly 2000 (every
            // rejection reported exactly once, none lost to the ceiling).
            qEng.setAutoProcessPromises(true);
            qEng.evaluate(QStringLiteral("0;"));
            TEST_RESULT("Promise: Every rejection reported exactly once after ceiling overflow",
                        reports == 2000 && lastMsg.contains(QLatin1String("cap_r1999")),
                        QStringLiteral("reports=%1 lastMsg='%2' - entries were lost or double-reported "
                                       "across the ceiling")
                            .arg(reports).arg(lastMsg));
            TEST_RESULT("Promise: Engine alive after ceiling overflow",
                        qEng.evaluate(QStringLiteral("1+1")).toInt() == 2,
                        "engine damaged");
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

        // QJSEngine parity: function objects are retained even in the lossy
        // conversion (verified against live Qt 6.12); the rest is still refused.
        const char* exotic[] = {
            "new Map([[1, 2]])", "new Set([1])", "new WeakMap()", "new WeakSet()",
            "Promise.resolve(1)", "/re?gex*/", "new DataView(new ArrayBuffer(8))"
        };
        bool allRefused = true;
        for (const char* e : exotic) {
            QVariant v = qEng.evaluate(QString::fromUtf8(e)).toVariant();
            if (v.isValid()) allRefused = false;
        }
        TEST_RESULT("Convert: exotic objects refused", allRefused, "an exotic object produced a valid QVariant");
        TEST_RESULT("Convert: function retained (Qt parity)",
                    qEng.evaluate("(function(){})").toVariant().userType() == qMetaTypeId<QQuickJSValue>(),
                    "function toVariant() is not a QVariant<QQuickJSValue>");

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

    // Narrow scalar types must keep their sign through the metacall fast
    // paths: moc writes exactly sizeof(declared type) bytes into the return
    // slot, so a char return read through a short buffer turns -1 into 255,
    // and a 4-byte LLP64 long read through a qint64 buffer loses the sign.
    // Live parity: the same object is exposed to both engines.
    {
        TestObject* obj = new TestObject();
        compareQObjectEngines("QObject: narrow signed method returns", obj,
                              "String(testObj.getChar()) + ' ' + String(testObj.getSChar()) + ' ' + String(testObj.getShort()) + ' ' + String(testObj.getLong()) + ' ' + String(testObj.getULong())");
        compareQObjectEngines("QObject: narrow property reads", obj,
                              "String(testObj.charProp) + ' ' + String(testObj.scharProp) + ' ' + String(testObj.shortProp) + ' ' + String(testObj.longProp) + ' ' + String(testObj.ulongProp)");
        compareQObjectEngines("QObject: narrow property write", obj, "testObj.shortPropW = -7; String(testObj.shortPropW)");
        delete obj;
    }

    // Signal arguments take the toJSValueFromVoid() fast path: an LLP64 long
    // argument read as a full qint64 would also go out of bounds of the
    // caller's storage; the sign must survive on every ABI.
    {
        QQuickJSEngine qEng;
        TestObject* obj = new TestObject();
        qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
        qEng.evaluate("testObj.narrowSignal.connect(function(c, l, ul){ globalThis.narrow = [c, l, ul]; });");
        obj->emitNarrowSignal();
        TEST_RESULT("Types: narrow signal arguments keep the sign",
                    qEng.evaluate("globalThis.narrow[0] === -1 && globalThis.narrow[1] === -2"
                                  " && globalThis.narrow[2] === 5").toBool(),
                    "a narrow signal argument lost its sign");
        delete obj;
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

        // Width regressions: non-int enum widths must flow through their
        // declared storage in every direction (property read/write, method
        // argument, method return). A quint64-backed enum rewritten to Int
        // smashes a 4-byte stack buffer (moc writes 8); a qint8-backed one
        // loses its sign (-1 read as 255). Run under ASan in the suite build,
        // the old code fails these by memory corruption, not just values.
        compareQObjectEngines("QObject: wide quint64 enum property read", obj, "testObj.wideProp");
        compareQObjectEngines("QObject: narrow qint8 enum property read", obj, "String(testObj.narrowProp)");
        compareQObjectEngines("QObject: wide enum method round-trip", obj, "testObj.echoWide(4398046511104)");
        compareQObjectEngines("QObject: narrow enum method round-trip", obj, "String(testObj.echoNarrow(-1))");
        {
            QQuickJSEngine qEngWidth;
            TestObject* wobj = new TestObject();
            qEngWidth.globalObject().setProperty("testObj", qEngWidth.newQObject(wobj));
            const QQuickJSValue wideWrite = qEngWidth.evaluate(
                "testObj.wideProp = 2199023255552; testObj.wideAsInt(2199023255552)");
            TEST_RESULT("QObject: wide quint64 enum write keeps the full width",
                        wideWrite.toVariant().toDouble() == 2199023255552.0
                            && static_cast<quint64>(wobj->getWideProp()) == 2199023255552ULL,
                        QStringLiteral("JS=%1 C++=%2")
                            .arg(wideWrite.toVariant().toDouble())
                            .arg(qint64(static_cast<quint64>(wobj->getWideProp()))));
            const QQuickJSValue narrowWrite = qEngWidth.evaluate(
                "testObj.narrowProp = -1; testObj.narrowAsInt(-1)");
            TEST_RESULT("QObject: narrow qint8 enum keeps the sign",
                        narrowWrite.toInt() == -1 && wobj->getNarrowProp() == TestObject::Narrow::Neg,
                        QStringLiteral("JS=%1").arg(narrowWrite.toInt()));
            delete wobj;
        }
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
        TEST_RESULT("Limit: Stack Overflow", isStackGuardError(stackEng.evaluate("function f(){f();} f();")), "Stack overflow not caught");

        // A tight budget must catch runaway recursion and leave the engine usable.
        // (The requested budget is the JS recursion share; the effective guard
        // adds the calibrated entry-machinery allowance on top, see
        // QQuickJSEngine::setMaxStackSize and calibrateMachineryAllowance().)
        QQuickJSEngine budgetEng;
        budgetEng.setMaxStackSize(64 * 1024);
        TEST_RESULT("Limit: setMaxStackSize tightens the recursion budget",
                    isStackGuardError(budgetEng.evaluate("function f(){f();} f();"))
                        && budgetEng.evaluate("1+1").toInt() == 2,
                    "a custom stack budget did not catch recursion");

        // Regression (Windows MinGW Debug+ASan): runaway recursion in a nested
        // evaluate() called from a slot under a tight budget must stay a
        // catchable error, and both the outer script and the engine must stay
        // usable afterwards.
        {
            QQuickJSEngine deepEng;
            deepEng.setMaxStackSize(64 * 1024);
            ThrowProbeObject deepProbe;
            deepProbe.qjsEngine = &deepEng;
            deepEng.globalObject().setProperty(QStringLiteral("obj"), deepEng.newQObject(&deepProbe));
            QQuickJSValue outer = deepEng.evaluate(QStringLiteral("obj.runDeepEval(); 40 + 2;"));
            TEST_RESULT("Limit: nested runaway recursion stays catchable",
                        outer.toInt() == 42 && deepProbe.innerCaught
                            && deepEng.evaluate("1+1").toInt() == 2,
                        "nested recursion under a tight budget did not stay catchable");
        }

        // A small worker-thread stack: the default budget must be clamped to
        // the real thread bounds, so the guard fires instead of the process
        // stack being exhausted (Windows ASan Debug crash class). Moved to
        // the dangerous regression section: on unpatched code the misclamped
        // guard line kills the whole process, which must not mask the safe
        // tests' results (see runDangerous).

        // Starved stack, the underflow regression: the whole thread stack
        // (128 KiB) is below the safety margin, so the anchor sits INSIDE the
        // margin - the exact case where the old clamp arithmetic underflowed
        // and installed an unreachable guard line (hard process crash on the
        // first runaway recursion). Also moved to the dangerous section:
        // pre-fix code never gets this far (SIGSEGV at the first entry).

        // The guard must fire at real recursion depth, not on entry: count
        // the frames actually reached before the catchable RangeError. A
        // depth of 0-2 means the guard fired inside the entry machinery (the
        // degenerate-window failure class seen on Windows Debug+ASan: the
        // calibrated machinery allowance alone consumed the whole usable
        // window below the entry).
        {
            QQuickJSEngine depthEng;
            const QQuickJSValue depthRes = depthEng.evaluate(QStringLiteral(
                "globalThis.depth = 0; function f(){ globalThis.depth++; f(); } f();"));
            const int depth = depthEng.globalObject().property(QStringLiteral("depth")).toInt();
            TEST_RESULT("Limit: guard fires at real recursion depth",
                        isStackGuardError(depthRes) && depth >= 5,
                        QStringLiteral("depth=%1 (0-2 means the guard fired inside the entry machinery)")
                            .arg(depth));
            TEST_RESULT("Limit: engine usable after a stack overflow",
                        depthEng.evaluate(QStringLiteral("1+1")).toInt() == 2,
                        "engine damaged after a stack overflow");
        }

        // Repeated runaway recursion must stay catchable every time (the
        // throw/unwind/backtrace cycle must not corrupt guard or engine
        // state); the engine must remain usable after the whole series.
        {
            QQuickJSEngine repeatEng;
            int firstNotCaught = -1;
            for (int i = 0; i < 10; ++i) {
                const QQuickJSValue r = repeatEng.evaluate(QStringLiteral("function f(){f();} f();"));
                if (!isStackGuardError(r)) {
                    firstNotCaught = i;
                    break;
                }
            }
            TEST_RESULT("Limit: repeated stack overflows stay catchable",
                        firstNotCaught < 0 && repeatEng.evaluate(QStringLiteral("1+1")).toInt() == 2,
                        QStringLiteral("run %1 returned a non-error").arg(firstNotCaught));
        }

        // Runaway recursion inside a promise job - the deepest legitimate
        // entry (evaluate -> job drain -> JS_ExecutePendingJob ->
        // JS_CallInternal) - must surface as a reported uncaught exception
        // for that job, never as a process crash; the job body before the
        // recursion point must have run.
        {
            QQuickJSEngine jobEng;
            int reports = 0;
            QObject::connect(&jobEng, &QQuickJSEngine::uncaughtException,
                             [&](const QString&, int) { reports++; });
            const QQuickJSValue setup = jobEng.evaluate(QStringLiteral(
                "globalThis.res = 'pending'; Promise.resolve().then(function(){ "
                "globalThis.res = 'job ran'; function f(){ f(); } f(); });"));
            TEST_RESULT("Limit: recursion in a promise job stays catchable",
                        !setup.isError()
                            && jobEng.globalObject().property(QStringLiteral("res")).toString() == QLatin1String("job ran")
                            && reports == 1
                            && jobEng.evaluate(QStringLiteral("1+1")).toInt() == 2,
                        QStringLiteral("reports=%1 res=%2 - job recursion did not stay catchable")
                            .arg(reports)
                            .arg(jobEng.globalObject().property(QStringLiteral("res")).toString()));
        }

        // The guard's usable window must not be degenerate on this
        // configuration: a fresh engine on the main thread must not emit the
        // "degenerate stack guard window" warning (machinery allowance
        // eating the whole usable window - the Windows Debug+ASan failure
        // class: false RangeErrors from promise machinery plus a hard crash
        // on the overflow test). The capture scope must end before TEST_RESULT:
        // the installed handler swallows every message type, including the
        // qInfo() of the result line itself.
        bool degenerate = false;
        {
            WarningCaptureGuard capture;
            QQuickJSEngine saneEng;
            saneEng.evaluate(QStringLiteral("1+1"));
            for (const QString& w : g_capturedWarnings) {
                if (w.contains(QLatin1String("degenerate stack guard window"))) degenerate = true;
            }
        }
        TEST_RESULT("Limit: guard budget window not degenerate",
                    !degenerate,
                    "the calibrated machinery allowance ate the whole usable window - "
                    "raise the executable stack reserve (link with --stack,16777216)");

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

    // Step budget (r8): a deterministic per-run complement to the wall-clock
    // timeout. One "step" is one counted operation of the engine's interrupt
    // heartbeat (interpreter branch/function-entry/builtin-loop operations,
    // regex backtracking steps), charged 10000 at a time at each poll - so
    // the tests below assert pure counting behavior with no timing. Budgets
    // are multiples of 10000 (the poll interval) for exact boundaries, and
    // the loop sizes leave a >=3x margin on both sides of the boundary so
    // the exact per-iteration branch count of the compiled bytecode cannot
    // flip a verdict.
    {
        // A run whose step total stays far below the budget completes and
        // returns its result (1000 iterations compile to at most a few
        // thousand counted operations - far below 100000; the handler is
        // never even reached before the run ends).
        QQuickJSEngine okEng;
        okEng.setStepBudget(100000);
        const QQuickJSValue shortRun = okEng.evaluate(
            QStringLiteral("var s = 0; for (var i = 0; i < 1000; i++) s += i; s;"));
        TEST_RESULT("Limit: Step budget - short run completes",
                    !shortRun.isError() && shortRun.toInt() == 499500,
                    QStringLiteral("res=%1 - a run under the budget did not complete")
                        .arg(shortRun.toString()));

        // A runaway loop is killed deterministically: 100000 iterations
        // execute at least 100000 counted operations >= the 50000 budget, so
        // the fifth poll fires. No wall-clock assertion anywhere.
        QQuickJSEngine killEng;
        killEng.setStepBudget(50000);
        const QQuickJSValue killed = killEng.evaluate(
            QStringLiteral("var s = 0; for (var i = 0; i < 100000; i++) s++; s;"));
        TEST_RESULT("Limit: Step budget kills a runaway loop deterministically",
                    killed.isError() && killed.toString().contains(QLatin1String("interrupted")),
                    QStringLiteral("res=%1 - the loop was not interrupted by the step budget")
                        .arg(killed.toString()));
        TEST_RESULT("Limit: Engine alive after step-budget kill",
                    killEng.evaluate(QStringLiteral("1+1")).toInt() == 2,
                    "engine damaged");

        // The budget is a per-run quota: an unbounded run exhausts it, and
        // the NEXT top-level entry starts fresh (20000 iterations cost at
        // most ~60000 counted operations - below the 200000 budget, but only
        // if the counter was reset; a carried-over count would kill it at the
        // first poll of the second run).
        QQuickJSEngine quotaEng;
        quotaEng.setStepBudget(200000);
        const QQuickJSValue first = quotaEng.evaluate(QStringLiteral("while (true) {}"));
        const QQuickJSValue second = quotaEng.evaluate(
            QStringLiteral("var s = 0; for (var i = 0; i < 20000; i++) s += i; s;"));
        TEST_RESULT("Limit: Step budget is a per-run quota",
                    first.isError() && !second.isError() && second.toInt() == 199990000,
                    QStringLiteral("first=%1 second=%2 - the quota was not re-armed at the "
                                   "second top-level entry")
                        .arg(first.isError() ? QLatin1String("killed") : first.toString())
                        .arg(second.isError() ? QLatin1String("killed") : second.toString()));

        // The regex engine shares the same heartbeat (libregexp polls the
        // interrupt handler every 10000 backtracking steps): a fast match
        // completes under a small budget...
        QQuickJSEngine reEng;
        reEng.setStepBudget(20000);
        const QQuickJSValue fastMatch = reEng.evaluate(
            QStringLiteral("var re = /(?:a+)+b/; re.test('aaab');"));
        TEST_RESULT("Limit: Step budget - fast regex match completes",
                    !fastMatch.isError() && fastMatch.isBool() && fastMatch.toBool(),
                    QStringLiteral("res=%1 - a fast regex match was interrupted")
                        .arg(fastMatch.toString()));

        // ...while catastrophic backtracking is policed by the same budget:
        // (?:a+)+b against a^28 + 'c' explores exponentially many
        // partitions (hundreds of millions of backtracking steps - orders of
        // magnitude over the 100000 budget), so the run must die at the
        // tenth poll regardless of machine speed.
        QQuickJSEngine btEng;
        btEng.setStepBudget(100000);
        const QQuickJSValue backtrack = btEng.evaluate(
            QStringLiteral("var re = /(?:a+)+b/; re.test('aaaaaaaaaaaaaaaaaaaaaaaaaaaa' + 'c');"));
        TEST_RESULT("Limit: Step budget polices regex backtracking",
                    backtrack.isError() && backtrack.toString().contains(QLatin1String("interrupted")),
                    QStringLiteral("res=%1 - catastrophic backtracking was not policed by "
                                   "the step budget")
                        .arg(backtrack.toString()));

        // Budget 0 (the default) disables the accounting entirely: the loop
        // below runs until the watchdog's sticky interrupt flag fires at
        // ~300 ms - if the budget path fired instead, the run would die
        // within microseconds of entry.
        {
            QQuickJSEngine offEng;
            offEng.setStepBudget(0);
            InterruptWatchdog watchdog;
            watchdog.engine = &offEng;
            watchdog.delayMs = 300;
            watchdog.start();
            QElapsedTimer t; t.start();
            const QQuickJSValue res = offEng.evaluate(QStringLiteral("while (true) {}"));
            watchdog.stopFlag = true;
            watchdog.wait();
            const qint64 el = t.elapsed();
            TEST_RESULT("Limit: Step budget 0 (default) does not limit execution",
                        res.isError() && el >= 250,
                        QStringLiteral("elapsed=%1ms - the disabled budget still interrupted "
                                       "the run").arg(el));
        }
    }

    // Qt parity: the interrupt flag is sticky - any execution aborts until
    // setInterrupted(false) (QJSEngine documented semantics)
    {
        QQuickJSEngine qEng;
        QQuickJSValue func = qEng.evaluate("(function() { return 42; })");
        qEng.setInterrupted(true);
        TEST_RESULT("Interrupt: sticky flag aborts evaluate()",
                    qEng.evaluate("1 + 1").isError()
                        && qEng.evaluate("1 + 1").toString().contains(QStringLiteral("Interrupted")),
                    QStringLiteral("evaluate() ran with the interrupt flag set"));
        TEST_RESULT("Interrupt: sticky flag aborts call()",
                    func.call().isError()
                        && func.call().toString().contains(QStringLiteral("Interrupted")),
                    QStringLiteral("call() ran with the interrupt flag set"));
        qEng.setInterrupted(false);
        TEST_RESULT("Interrupt: cleared flag resumes execution",
                    qEng.evaluate("1 + 1").toInt() == 2 && func.call().toInt() == 42,
                    "execution did not resume after setInterrupted(false)");
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

        // newSymbol: ECMAScript symbol semantics. V4's QJSEngine::newSymbol()
        // does not expose the description property (it stays undefined there
        // even though String(sym) still shows the name), so this check
        // deliberately verifies the spec-compliant behavior instead of
        // comparing the engines.
        {
            QQuickJSEngine qEng2;
            qEng2.globalObject().setProperty("sym", qEng2.newSymbol("mysym"));
            qEng2.globalObject().setProperty("emptySym", qEng2.newSymbol(QString()));
            const QString res = qEng2.evaluate(QStringLiteral(
                "typeof sym + '|' + sym.description + '|' + String(sym) + '|' + (sym == sym) + ';' + "
                "typeof emptySym + '|' + String(emptySym) + '|' + (emptySym.description === '')")).toString();
            TEST_RESULT("API: newSymbol", res == QStringLiteral("symbol|mysym|Symbol(mysym)|true;symbol|Symbol()|true"), res);
        }

        // toScriptValue / fromScriptValue round-trips and coercions
        TEST_RESULT("API: toScriptValue(int)", qEng.toScriptValue(42).toInt() == 42, "int round-trip failed");
        TEST_RESULT("API: toScriptValue(QString)", qEng.toScriptValue(QStringLiteral("привет")).toString() == QStringLiteral("привет"), "QString round-trip failed");
        TEST_RESULT("API: toScriptValue(double)", qFuzzyCompare(qEng.toScriptValue(3.5).toDouble(), 3.5), "double round-trip failed");
        {
            QVariantList list = {QVariant(1), QVariant(2)};
            QQuickJSValue v = qEng.toScriptValue(list);
            TEST_RESULT("API: toScriptValue(QVariantList)", v.isArray() && v.property(1).toInt() == 2, "QVariantList round-trip failed");
            TEST_RESULT("API: fromScriptValue(QVariantList)", qEng.fromScriptValue<QVariantList>(v) == list, "QVariantList back-conversion failed");
        }
        TEST_RESULT("API: fromScriptValue(int)", qEng.fromScriptValue<int>(qEng.evaluate("42")) == 42, "int back-conversion failed");
        TEST_RESULT("API: fromScriptValue(QString)", qEng.fromScriptValue<QString>(qEng.evaluate("42")) == QStringLiteral("42"), "QString back-conversion failed");
        TEST_RESULT("API: fromScriptValue default on unrepresentable", qEng.fromScriptValue<int>(qEng.evaluate("(function(){})")) == 0, "function must yield a default T");
        {
            QJSEngine qtEng;
            const int qtI = qtEng.fromScriptValue<int>(qtEng.evaluate("4.7"));
            const int qI = qEng.fromScriptValue<int>(qEng.evaluate("4.7"));
            TEST_RESULT("API: fromScriptValue<int> number coercion parity", qI == qtI,
                        QStringLiteral("QJSEngine=%1, QuickJS=%2").arg(qtI).arg(qI));
        }

        // errorType(): the class of an error object, NoError otherwise
        {
            QJSEngine qtEng;
            QJSValue qtErr = qtEng.evaluate("null.x");
            QQuickJSValue qErr = qEng.evaluate("null.x");
            TEST_RESULT("API: errorType parity",
                        qErr.errorType() == QQuickJSValue::TypeError
                            && static_cast<int>(qErr.errorType()) == static_cast<int>(qtErr.errorType()),
                        QStringLiteral("QJSEngine=%1, QuickJS=%2")
                            .arg(static_cast<int>(qtErr.errorType())).arg(static_cast<int>(qErr.errorType())));
            TEST_RESULT("API: errorType (NoError)", qEng.evaluate("'str'").errorType() == QQuickJSValue::NoError, "non-error must be NoError");

            // A thrown non-Error value carries no error class: NoError, like
            // QJSEngine (isError() stays true for such values - a deliberate
            // documented divergence).
            {
                QJSEngine qtPrim;
                TEST_RESULT("API: errorType of thrown primitives is NoError (parity)",
                            qEng.evaluate("throw 42").errorType() == QQuickJSValue::NoError
                                && qEng.evaluate("throw 'str'").errorType() == QQuickJSValue::NoError
                                && qEng.evaluate("throw {}").errorType() == QQuickJSValue::NoError
                                && qtPrim.evaluate("throw 42").errorType() == QJSValue::NoError,
                            "a thrown non-Error value must classify as NoError");
            }

            // errorType() classifies by prototype identity against the
            // construction-time intrinsics, not by the mutable 'name'
            // property (QJSEngine branches on the internal error class).
            // Dedicated engine: the global-constructor replacement below
            // must not leak into the other tests.
            {
                QQuickJSEngine tEng;
                const QQuickJSValue tampered = tEng.evaluate(
                    "(function(){ var e = new Error('x'); e.name = 'RangeError'; return e; })()");
                TEST_RESULT("API: errorType ignores 'name' tampering",
                            tampered.errorType() == QQuickJSValue::GenericError,
                            "a plain Error with e.name='RangeError' must stay GenericError");

                const QQuickJSValue sub = tEng.evaluate(
                    "class MyRE extends RangeError {}; (function(){ try { throw new MyRE('sub'); } catch (e) { return e; } })()");
                TEST_RESULT("API: errorType of an error subclass is the base class",
                            sub.errorType() == QQuickJSValue::RangeError,
                            "MyRE extends RangeError must classify as RangeError");

                const QQuickJSValue plain = tEng.evaluate(
                    "(function(){ try { null.x; } catch (e) { return e; } })()");
                const bool wasTypeError = plain.errorType() == QQuickJSValue::TypeError;
                tEng.evaluate("globalThis.TypeError = function Stolen(){};");
                TEST_RESULT("API: errorType stable after global constructor replacement",
                            wasTypeError && plain.errorType() == QQuickJSValue::TypeError,
                            "classification must use the construction-time intrinsics");
            }

            // toDateTime() reads the internal date slot through the captured
            // intrinsic getTime: overriding d.getTime or
            // Date.prototype.getTime must not change the converted value
            // (QJSEngine reads the slot directly).
            {
                QQuickJSEngine dEng;
                const QQuickJSValue ownOverride = dEng.evaluate(
                    "var d = new Date(1234567890123); d.getTime = function(){ return 42; }; d");
                const QQuickJSValue protoOverride = dEng.evaluate(
                    "Date.prototype.getTime = function(){ return 7; }; var d2 = new Date(1000); d2");
                TEST_RESULT("API: toDateTime ignores getTime tampering",
                            ownOverride.toDateTime().toMSecsSinceEpoch() == 1234567890123
                                && protoOverride.toDateTime().toMSecsSinceEpoch() == 1000,
                            "the Date conversion must call the captured intrinsic");
            }

            // newErrorObject(): a message with an embedded NUL survives
            // whole (the "%s" C constructors stop at the first NUL; the
            // full payload is restored through a plain data-property write).
            {
                QQuickJSEngine nEng;
                const QString nulMsg = QString::fromUtf8(QByteArray("before\0after", 11));
                const QQuickJSValue err = nEng.newErrorObject(QQuickJSValue::TypeError, nulMsg);
                const QString back = err.property("message").toString();
                TEST_RESULT("API: newErrorObject keeps messages with an embedded NUL",
                            err.errorType() == QQuickJSValue::TypeError
                                && back == nulMsg && back.size() == 11,
                            QStringLiteral("message came back %1 bytes long").arg(back.size()));
            }

            // Detached buffers convert to an empty QByteArray (V4 reads the
            // data pointer + length, null/0 after a detach) instead of
            // falling into the property enumeration and yielding an empty
            // QVariantMap.
            {
                QQuickJSEngine bEng;
                const QVariant ab = bEng.evaluate("var b = new ArrayBuffer(8); b.transfer(); b").toVariant();
                const QVariant ta = bEng.evaluate("var b = new ArrayBuffer(8); var v = new Uint8Array(b); b.transfer(); v").toVariant();
                TEST_RESULT("Buffer: detached ArrayBuffer converts to an empty QByteArray",
                            ab.userType() == QMetaType::QByteArray && ab.toByteArray().isEmpty(),
                            QStringLiteral("userType=%1").arg(ab.userType()));
                TEST_RESULT("Buffer: typed array over a detached buffer converts to an empty QByteArray",
                            ta.userType() == QMetaType::QByteArray && ta.toByteArray().isEmpty(),
                            QStringLiteral("userType=%1").arg(ta.userType()));
            }

            // A wrapper whose QObject died keeps reporting isQObject()==true
            // with a null toQObject() - both the QJSEngine behavior (the
            // wrapper object survives; the classification does not branch on
            // the wrapped pointer being alive).
            {
                QQuickJSEngine zEng;
                auto* zombie = new TestObject();
                const QQuickJSValue wrap = zEng.newQObject(zombie);
                const bool wasQObject = wrap.isQObject();
                delete zombie;
                TEST_RESULT("QObject: wrapper stays isQObject() after C++ deletion (Qt parity)",
                            wasQObject && wrap.isQObject() && wrap.toQObject() == nullptr,
                            "isQObject must survive the wrapped object, like QJSEngine");
            }

            // console.count()/console.time() label tables are capped: dynamic
            // labels cannot grow them without bound, and the cap says so
            // once (the JSMemoryUsage-based leakgate cannot see C++-side
            // containers).
            {
                QQuickJSEngine cEng;
                cEng.installExtensions(QQuickJSEngine::ConsoleExtension);
                int ceilingWarnings = 0;
                {
                    MessageCaptureGuard guard;
                    cEng.evaluate(QStringLiteral("for (let i = 0; i < 1200; ++i) console.count('lbl' + i);"));
                    for (const QString& m : g_capturedMessages)
                        if (m.contains(QLatin1String("label tables are capped"))) ++ceilingWarnings;
                }
                TEST_RESULT("Console: label tables capped with a one-shot warning",
                            ceilingWarnings == 1,
                            QStringLiteral("ceiling warnings: %1").arg(ceilingWarnings));
            }

            // A primitive this follows JS call semantics in both engines (a
            // sloppy callee boxes it); callWithInstance() binds the global
            // object for non-object instances (QJSEngine semantics).
            {
                QJSEngine qtCall;
                const QString src = QStringLiteral("function f(){ return typeof this; } f;");
                QQuickJSValue qf = qEng.evaluate(src);
                const QJSValue qtf = qtCall.evaluate(src);
                const QString primThis = qf.call(QVariantList{}, qEng.evaluate("42")).toString();
                const QString oursInstance = qf.callWithInstance(qEng.evaluate("42"), QVariantList{}).toString();
                const QString theirsInstance = qtf.callWithInstance(qtCall.evaluate("42")).toString();
                TEST_RESULT("API: callWithInstance with a primitive instance (parity)",
                            primThis == QLatin1String("object") && oursInstance == theirsInstance,
                            QStringLiteral("call(prim-this)=%1 callWithInstance(prim)=%2 QJSEngine=%3")
                                .arg(primThis, oursInstance, theirsInstance));
            }
            QQuickJSValue evalErr = qEng.newErrorObject(QQuickJSValue::EvalError, "eval fail");
            TEST_RESULT("API: newErrorObject(EvalError)",
                        evalErr.isError() && evalErr.property("name").toString() == QStringLiteral("EvalError")
                            && evalErr.errorType() == QQuickJSValue::EvalError,
                        "EvalError creation failed");

            // Engine-created errors use the built-in prototypes: scripts
            // replacing globalThis.TypeError cannot affect them (QJSEngine
            // parity — live V4 probe, tools/probe-qt-apiaudit.cpp).
            {
                QJSEngine qtEng;
                QQuickJSEngine qEng2;
                const QString sabotage = QStringLiteral(
                    "globalThis.TypeError = function(m) { this.name = 'Replaced'; this.message = m; };");
                qtEng.evaluate(sabotage);
                qEng2.evaluate(sabotage);
                QJSValue qtErr2 = qtEng.newErrorObject(QJSValue::TypeError, QStringLiteral("proto boom"));
                QQuickJSValue qErr2 = qEng2.newErrorObject(QQuickJSValue::TypeError, QStringLiteral("proto boom"));
                TEST_RESULT("API: error prototype immune to constructor override",
                            qtErr2.isError() && qErr2.isError()
                                && qtErr2.property(QStringLiteral("name")).toString() == qErr2.property(QStringLiteral("name")).toString()
                                && qErr2.property(QStringLiteral("name")).toString() == QStringLiteral("TypeError")
                                && qErr2.property(QStringLiteral("message")).toString() == QStringLiteral("proto boom"),
                            QStringLiteral("qt name=%1, q name=%2, q msg=%3")
                                .arg(qtErr2.property(QStringLiteral("name")).toString(),
                                     qErr2.property(QStringLiteral("name")).toString(),
                                     qErr2.property(QStringLiteral("message")).toString()));
            }
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
        invalid.setPrototype(invalid);   // no-op on an invalid value: must not crash
        invalid.callWithInstance(invalid);   // no engine: must not crash
        TEST_RESULT("API: invalid value operations (no crash)", !invalid.property("test").isValid() && !invalid.hasProperty("test") && !invalid.deleteProperty("test") && invalid.toInt() == 0 && invalid.toUInt() == 0 && !invalid.toBool() && qIsNaN(invalid.toDouble()) && invalid.toString().isEmpty() && !invalid.toVariant().isValid() && !invalid.toDateTime().isValid() && invalid.toQObject() == nullptr && !invalid.isValid() && !invalid.isError() && invalid.errorType() == QQuickJSValue::NoError && !invalid.isCallable() && !invalid.isObject() && !invalid.isArray() && !invalid.isString() && !invalid.isNumber() && !invalid.isBool() && !invalid.isNull() && !invalid.isRegExp() && !invalid.isDate() && !invalid.isQObject() && invalid.isUndefined() && !invalid.prototype().isValid() && !invalid.hasOwnProperty("test") && !invalid.equals(invalid), "Invalid value operations should be safe");

        qEng.evaluate("function sumArgs(a,b,c) { return a+b+c; }");
        QQuickJSValue func = qEng.evaluate("sumArgs");
        TEST_RESULT("API: call(QVariantList, thisObj)", func.call({QVariant(1), QVariant(2), QVariant(3)}).toInt() == 6, "call with QVariantList failed");
        TEST_RESULT("API: call() default args", qEng.evaluate("(function() { return 7; })").call().toInt() == 7, "call with default args failed");
        TEST_RESULT("API: call (exception)", qEng.evaluate("(function() { throw new Error('CallErr'); })").call().toString().contains("CallErr"), "Exception in call not captured");

        // call() with no thisObject binds the engine global object, even for
        // strict callees (QJSEngine parity — live V4 probe, tools/probe-qt-apiaudit.cpp).
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng2;
            QJSValue qtStrict = qtEng.evaluate(QStringLiteral("(function() { 'use strict'; return this === globalThis; })"));
            QQuickJSValue qStrict = qEng2.evaluate(QStringLiteral("(function() { 'use strict'; return this === globalThis; })"));
            const bool qtOk = qtStrict.call().toBool();
            const bool qOk = qStrict.call().toBool();
            TEST_RESULT("API: call() default this is the global object (strict)",
                        qtOk && qOk, QStringLiteral("qt=%1, q=%2").arg(qtOk).arg(qOk));
            QJSValue qtSloppy = qtEng.evaluate(QStringLiteral("(function() { return this === globalThis; })"));
            QQuickJSValue qSloppy = qEng2.evaluate(QStringLiteral("(function() { return this === globalThis; })"));
            TEST_RESULT("API: call() default this is the global object (sloppy)",
                        qtSloppy.call().toBool() && qSloppy.call().toBool(),
                        QStringLiteral("qt=%1, q=%2").arg(qtSloppy.call().toBool()).arg(qSloppy.call().toBool()));
        }

        // Deliberate documented divergence from QJSEngine: our isError() is
        // true for ANY thrown value (V4 reports Error objects only; live probe
        // tools/probe-qt-apiaudit.cpp — Qt: throw 42 -> isError false).
        TEST_RESULT("QQuickJSValue: isError covers thrown primitives (deliberate)",
                    qEng.evaluate(QStringLiteral("(function(){ throw 42; })()")).isError(),
                    "thrown primitive must be flagged as an error here");

        QQuickJSEngine otherEng;
        QQuickJSValue v1 = qEng.evaluate("42");
        QQuickJSValue v2 = otherEng.evaluate("42");
        TEST_RESULT("API: strictlyEquals (cross-context)", !v1.strictlyEquals(v2), "Cross-context should not be equal");
        TEST_RESULT("API: call (cross-context args)", !func.call(QList<QQuickJSValue>{v2}).isValid(), "Cross-context args should fail");

        // equals(): ECMA-262 abstract equality
        {
            QQuickJSValue num42 = qEng.evaluate("42");
            QQuickJSValue str42 = qEng.evaluate("'42'");
            QQuickJSValue str42b = qEng.evaluate("'42'");
            TEST_RESULT("API: equals (loose)", num42.equals(str42) && !num42.strictlyEquals(str42) && str42.equals(str42b), "equals != abstract equality");
            TEST_RESULT("API: equals (false)", !qEng.evaluate("'abc'").equals(qEng.evaluate("42")), "equals mismatch");
            TEST_RESULT("API: equals (cross-engine)", !num42.equals(otherEng.evaluate("42")), "cross-engine equals must fail");
            TEST_RESULT("API: equals (invalid)", !QQuickJSValue().equals(num42) && !num42.equals(QQuickJSValue()), "invalid equals must be false");
        }

        // new predicates
        TEST_RESULT("API: isRegExp", qEng.evaluate("/ab+c/").isRegExp() && !qEng.evaluate("'ab+c'").isRegExp() && !qEng.evaluate("({})").isRegExp(), "isRegExp failed");
        TEST_RESULT("API: isDate", qEng.evaluate("new Date()").isDate() && !qEng.evaluate("Date.now()").isDate() && !qEng.evaluate("({})").isDate(), "isDate failed");
        {
            TestObject obj;
            QQuickJSValue jsObj = qEng.newQObject(&obj);
            TEST_RESULT("API: isQObject", jsObj.isQObject() && !qEng.evaluate("({})").isQObject() && !qEng.evaluate("null").isQObject(), "isQObject failed");
            TEST_RESULT("API: toQObject", jsObj.toQObject() == &obj && qEng.evaluate("({})").toQObject() == nullptr, "toQObject failed");
        }

        // hasOwnProperty / prototype
        {
            QQuickJSValue o = qEng.evaluate("const base = {inherited: 1}; const derived = Object.create(base); derived.own = 2; derived");
            TEST_RESULT("API: hasOwnProperty", o.hasOwnProperty("own") && !o.hasOwnProperty("inherited") && o.hasProperty("inherited"), "hasOwnProperty failed");
            TEST_RESULT("API: hasOwnProperty (char*/QByteArray/QStringView)", o.hasOwnProperty("own") && !o.hasOwnProperty(QByteArrayLiteral("inherited")) && !o.hasOwnProperty(QStringView(u"something-else")), "hasOwnProperty overloads failed");
            // Accessor properties: the descriptor cleanup must free the
            // getter/setter refs as well (regression test).
            QQuickJSValue acc = qEng.evaluate("({get x() { return 1; }, set x(v) {}})");
            TEST_RESULT("API: hasOwnProperty (accessor)", acc.hasOwnProperty("x") && !acc.hasOwnProperty("y"), "accessor hasOwnProperty failed");
            QQuickJSValue proto = o.prototype();
            TEST_RESULT("API: prototype", proto.isValid() && proto.isObject() && proto.property("inherited").toInt() == 1, "prototype failed");
            TEST_RESULT("API: prototype (non-object)", !qEng.evaluate("42").prototype().isValid() && !qEng.evaluate("'s'").prototype().isValid(), "prototype of primitive must be invalid");
            QQuickJSValue replacement = qEng.evaluate("({inherited: 5})");
            o.setPrototype(replacement);
            TEST_RESULT("API: setPrototype", o.property("inherited").toInt() == 5, "setPrototype failed");
            o.setPrototype(qEng.evaluate("null"));
            TEST_RESULT("API: setPrototype (null)", !o.hasProperty("inherited"), "null prototype failed");
            o.setPrototype(qEng.evaluate("42"));
            TEST_RESULT("API: setPrototype (primitive ignored)", !o.hasProperty("inherited"), "primitive prototype must be ignored");
            // parity with QJSEngine for the prototype of a primitive
            {
                QJSEngine qtEng;
                QJSValue qtProto = qtEng.evaluate("42").prototype();
                QQuickJSValue qProto = qEng.evaluate("42").prototype();
                TEST_RESULT("API: prototype parity (primitive)", qtProto.isUndefined() == qProto.isUndefined() && qtProto.isObject() == qProto.isObject(), "prototype of primitive diverges from QJSEngine");
            }
        }

        // toUInt / toDateTime
        TEST_RESULT("API: toUInt", qEng.evaluate("42").toUInt() == 42 && qEng.evaluate("-1").toUInt() == 4294967295u && qEng.evaluate("4294967296").toUInt() == 0, "toUInt failed");
        {
            QJSEngine qtEng;
            TEST_RESULT("API: toUInt parity", qEng.evaluate("-1").toUInt() == qtEng.evaluate("-1").toUInt() && qEng.evaluate("4294967296").toUInt() == qtEng.evaluate("4294967296").toUInt(), "toUInt diverges from QJSEngine");
        }
        {
            QJSEngine qtEng;
            QQuickJSValue qDate = qEng.evaluate("new Date(1234567890)");
            QJSValue qtDate = qtEng.evaluate("new Date(1234567890)");
            TEST_RESULT("API: toDateTime parity", qDate.toDateTime() == qtDate.toDateTime(), "toDateTime diverges from QJSEngine");
            TEST_RESULT("API: toDateTime (invalid)", !qEng.evaluate("new Date(NaN)").toDateTime().isValid() && !qEng.evaluate("'str'").toDateTime().isValid(), "toDateTime invalid handling failed");
        }

        // callWithInstance(): QJSEngine 'this' semantics
        qEng.evaluate("function readTag() { return this.tag; } function isGlobal() { return this === globalThis; }");
        QQuickJSValue tagFn = qEng.evaluate("readTag");
        QQuickJSValue globFn = qEng.evaluate("isGlobal");
        QQuickJSValue receiver = qEng.evaluate("({tag: 'obj'})");
        TEST_RESULT("API: callWithInstance (object)", tagFn.callWithInstance(receiver).toString() == QStringLiteral("obj"), "object instance failed");
        TEST_RESULT("API: callWithInstance (primitive -> global)", globFn.callWithInstance(qEng.evaluate("42")).toBool(), "primitive instance must bind the global object");
        TEST_RESULT("API: callWithInstance (QVariantList args)", tagFn.callWithInstance(receiver, QVariantList()).toString() == QStringLiteral("obj"), "QVariantList overload failed");
        TEST_RESULT("API: callWithInstance (QList args)", tagFn.callWithInstance(receiver, QList<QQuickJSValue>()).toString() == QStringLiteral("obj"), "QList overload failed");
        {
            QJSEngine qtEng;
            qtEng.evaluate("function isGlobal() { return this === globalThis; }");
            const bool qtPrim = qtEng.evaluate("isGlobal").callWithInstance(qtEng.evaluate("42")).toBool();
            const bool qPrim = globFn.callWithInstance(qEng.evaluate("42")).toBool();
            TEST_RESULT("API: callWithInstance parity (primitive)", qPrim == qtPrim, "primitive-instance 'this' diverges from QJSEngine");
        }

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

        // r11 (D-60): type-coercion contracts moved to their own function (see
        // the note above it) to keep this frame slim for the 1 MiB starved matrix.
        runPropertyTypeCoercionTests();

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
        // UTC getters pin the V4 anchor: a valid QDate becomes the UTC
        // midnight of that date (qv4dateobject.cpp Date::init(QDate)), so
        // the UTC date components equal the QDate in every timezone. Local
        // getters would shift a day in negative-offset zones - in V4 too.
        TEST_RESULT("Types: QDate to JS Date", qEng.evaluate("dateVar instanceof Date && dateVar.getUTCFullYear() === 2024 && dateVar.getUTCMonth() === 0 && dateVar.getUTCDate() === 1 && dateVar.getUTCHours() === 0 && dateVar.getUTCMinutes() === 0").toBool(), "QDate to JS Date failed");

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

        // Review round-5 regressions: run-to-completion, invalid dates,
        // argument counts, the __proto__ export, stale deadlines and the
        // qsTr n coercion.
        {
            QQuickJSEngine qEng;
            TestObject* robj = new TestObject();
            qEng.globalObject().setProperty("testObj", qEng.newQObject(robj));

            // Run-to-completion: a signal callback emitted from a running
            // script is a NESTED entry and must not drain the microtask
            // queue; the top-level evaluate exit owns the drain.
            QQuickJSValue order1 = qEng.evaluate(QStringLiteral(
                "var order = [];"
                "testObj.valueChanged.connect(function(v) { order.push('sig'); });"
                "Promise.resolve().then(function() { order.push('micro'); });"
                "testObj.value = 7;"
                "order.push('after');"
                "order.join(',')"));
            QQuickJSValue order2 = qEng.evaluate(QStringLiteral("order.join(',')"));
            TEST_RESULT("Microtasks: nested signal callback does not drain the queue",
                        order1.toString() == QStringLiteral("sig,after")
                            && order2.toString() == QStringLiteral("sig,after,micro"),
                        QStringLiteral("join1='%1' join2='%2' - the nested callback drained the microtask queue")
                            .arg(order1.toString()).arg(order2.toString()));

            // Invalid QDateTime/QDate/QTime convert to an Invalid Date (V4
            // parity), never to the 1970 epoch: the method-return path and
            // the QVariant setProperty path.
            QQuickJSValue dates = qEng.evaluate(QStringLiteral(
                "String(testObj.invalidDateTime()) + '|' + String(testObj.invalidDate())"
                " + '|' + String(testObj.invalidTime())"));
            qEng.globalObject().setProperty(QStringLiteral("invDt"), QVariant(QDateTime()));
            qEng.globalObject().setProperty(QStringLiteral("invDate"), QVariant(QDate()));
            qEng.globalObject().setProperty(QStringLiteral("invTime"), QVariant(QTime()));
            QQuickJSValue dates2 = qEng.evaluate(QStringLiteral(
                "String(invDt) + '|' + String(invDate) + '|' + String(invTime)"));
            TEST_RESULT("Convert: invalid QDateTime/QDate/QTime are Invalid Date",
                        dates.toString() == QStringLiteral("Invalid Date|Invalid Date|Invalid Date")
                            && dates2.toString() == QStringLiteral("Invalid Date|Invalid Date|Invalid Date"),
                        QStringLiteral("method='%1' variant='%2' - an invalid date became the epoch")
                            .arg(dates.toString()).arg(dates2.toString()));

            // QTime -> Date is real V4 parity (probed against live Qt 6.12 in
            // UTC and Europe/Moscow): a real Date whose epoch is the LOCAL
            // QDateTime(1971-04-01, time) - the "April Fools' day" anchor
            // qtdeclarative uses for time-without-a-date. The epoch is
            // therefore TZ-dependent; the test computes the expected value
            // with the same QDateTime construction, so it holds in any zone.
            // Property read, method return and QVariant setProperty paths.
            const qint64 qtimeAnchorEpoch =
                QDateTime(QDate(1971, 4, 1), QTime(13, 45, 36, 150)).toMSecsSinceEpoch();
            qEng.globalObject().setProperty(QStringLiteral("timeVar"), QVariant(QTime(13, 45, 36, 150)));
            QQuickJSValue epochRes = qEng.evaluate(QStringLiteral(
                "var e = testObj.timeProp.getTime();"
                "var f = testObj.echoTime(testObj.timeProp).getTime();"
                "var g = timeVar.getTime(); e + ',' + f + ',' + g"));
            const QString epochWant = QString::number(qtimeAnchorEpoch) + QLatin1Char(',')
                + QString::number(qtimeAnchorEpoch) + QLatin1Char(',') + QString::number(qtimeAnchorEpoch);
            TEST_RESULT("Convert: QTime Date epoch is the local 1971-04-01 anchor (V4 parity)",
                        epochRes.toString() == epochWant,
                        QStringLiteral("got='%1' want='%2' - QTime anchor diverges from V4")
                            .arg(epochRes.toString(), epochWant));

            // QDate -> Date anchors the UTC midnight of that date (V4 parity,
            // probed in UTC and Europe/Moscow; qv4dateobject.cpp builds
            // d.startOfDay(QTimeZone::UTC)). Unlike the QTime anchor above
            // the epoch is therefore timezone-independent. Property read and
            // QVariant setProperty paths; the Date -> QDate argument round
            // trip is covered by the dual-engine parity suite below, because
            // in negative-offset zones it legitimately shifts the date - an
            // inherited V4 wart, not something to assert away.
            robj->setDateProp(QDate(2020, 5, 17));
            const qint64 qdateEpoch =
                QDateTime::fromString(QStringLiteral("2020-05-17T00:00:00Z"), Qt::ISODateWithMs)
                    .toMSecsSinceEpoch();
            qEng.globalObject().setProperty(QStringLiteral("dateV"), QVariant(QDate(2020, 5, 17)));
            QQuickJSValue dEpochRes = qEng.evaluate(QStringLiteral(
                "var a = testObj.dateProp.getTime();"
                "var b = dateV.getTime(); a + ',' + b"));
            const QString dEpochWant = QString::number(qdateEpoch) + QLatin1Char(',')
                + QString::number(qdateEpoch);
            TEST_RESULT("Convert: QDate Date epoch is the UTC midnight of that date (V4 parity)",
                        dEpochRes.toString() == dEpochWant,
                        QStringLiteral("got='%1' want='%2' - QDate anchor diverges from V4")
                            .arg(dEpochRes.toString(), dEpochWant));

            // V4 parity (probed against live Qt 6.12): toVariant() does NOT
            // punch through Proxies - quickjs-ng 0.18's JS_IsArray stopped
            // doing it, and V4's QVariant conversion enumerates proxied
            // arrays as maps with index keys just the same. Pinned so a
            // future "fix" cannot silently change the shape.
            {
                QQuickJSValue pa = qEng.evaluate(QStringLiteral("new Proxy([1,2,3],{})"));
                QVariant va = pa.toVariant();
                const bool arrayMap = va.metaType().id() == QMetaType::QVariantMap
                    && va.toMap().size() == 3 && va.toMap().value(QStringLiteral("1")).toInt() == 2;
                TEST_RESULT("Convert: Proxy(array).toVariant() is an index-keyed map (V4 parity)",
                            arrayMap,
                            QStringLiteral("type='%1' - proxy punching changed the shape")
                                .arg(QString::fromLatin1(va.metaType().name())));
                QQuickJSValue po = qEng.evaluate(QStringLiteral("new Proxy({a:1,b:'x'},{})"));
                QVariant vo = po.toVariant();
                TEST_RESULT("Convert: Proxy(object).toVariant() is a map (V4 parity)",
                            vo.metaType().id() == QMetaType::QVariantMap && vo.toMap().size() == 2,
                            QStringLiteral("type='%1'").arg(QString::fromLatin1(vo.metaType().name())));
                QQuickJSValue pd = qEng.evaluate(QStringLiteral("new Proxy(new Date(123),{})"));
                QVariant vd = pd.toVariant();
                TEST_RESULT("Convert: Proxy(Date).toVariant() is a map, not a QDateTime (V4 parity)",
                            vd.metaType().id() == QMetaType::QVariantMap,
                            QStringLiteral("type='%1'").arg(QString::fromLatin1(vd.metaType().name())));
            }

            // L-5 (probed against live Qt 6.12): a NULL QString converts to
            // an EMPTY JS string, not to JS null; a NULL QByteArray converts
            // to an EMPTY ArrayBuffer (byteLength 0), exactly like V4 (V4's
            // own toScriptValue of a QByteArray is an ArrayBuffer view too).
            {
                QString nullStr;
                QByteArray nullBa;
                const QQuickJSValue jNullStr = qEng.toScriptValue<QString>(nullStr);
                const QQuickJSValue jNullBa = qEng.toScriptValue<QByteArray>(nullBa);
                const QQuickJSValue baLen = qEng.evaluate(QStringLiteral(
                    "(function(v){ try { return 'len=' + v.byteLength; } catch(e){ return 'ERR'; } })"))
                    .call(QList<QQuickJSValue>() << jNullBa);
                TEST_RESULT("Convert: null QString gives '' and null QByteArray an empty ArrayBuffer (V4 parity)",
                            jNullStr.isString() && jNullStr.toString().isEmpty() && !jNullStr.isNull()
                                && jNullBa.isObject() && baLen.toString() == QStringLiteral("len=0"),
                            QStringLiteral("str: null=%1 string=%2; ba: object=%3 %4")
                                .arg(jNullStr.isNull()).arg(jNullStr.isString()).arg(jNullBa.isObject()).arg(baLen.toString()));
            }

            // M-3: prototype-chain classification. A Proxy itself is not an
            // Error object (class check), so errorType() reports NoError for
            // one; the REACHABLE trap scenario is an Error re-parented onto
            // a Proxy - the in-loop prototype fetch throws and must classify
            // as GenericError with no sticky exception left behind.
            {
                QQuickJSValue rawProxy = qEng.evaluate(QStringLiteral(
                    "new Proxy(new Error('x'), { getPrototypeOf() { throw 1; } })"));
                const QQuickJSValue::ErrorType proxyType = rawProxy.errorType();
                QQuickJSValue chainErr = qEng.evaluate(QStringLiteral(
                    "var e = new Error('x');"
                    "Object.setPrototypeOf(e, new Proxy(Error.prototype, { getPrototypeOf() { throw 1; } })); e"));
                const QQuickJSValue::ErrorType et = chainErr.errorType();
                const bool nextClean = qEng.evaluate(QStringLiteral("1+1")).toInt() == 2
                    && !qEng.hasError();
                TEST_RESULT("Error: throwing prototype-chain trap yields GenericError with no sticky exception",
                            proxyType == QQuickJSValue::NoError
                                && et == QQuickJSValue::GenericError && nextClean,
                            QStringLiteral("proxy=%1 chain=%2 nextClean=%3")
                                .arg(static_cast<int>(proxyType)).arg(static_cast<int>(et)).arg(nextClean));
            }

            // Q9 (V4 parity, probed against live Qt 6.12): a sticky interrupt
            // replaces even a REAL pending exception with the plain
            // "Interrupted" error - V4 behaves the same way.
            {
                QQuickJSEngine iEng;
                iEng.setInterrupted(true);
                QQuickJSValue ir = iEng.evaluate(QStringLiteral(
                    "var o = { get x() { throw new TypeError('REAL'); } }; o.x"));
                TEST_RESULT("Interrupt: sticky interrupt wins over a pending real error (V4 parity)",
                            ir.isError() && ir.toString() == QStringLiteral("Error: Interrupted"),
                            QStringLiteral("got='%1'").arg(ir.toString()));
                iEng.setInterrupted(false);
            }

            // L-3: a QByteArray property name with an embedded NUL installs
            // the FULL name (the QString/atom path), not a truncated one.
            {
                const QByteArray nulName(QByteArray("a\0b", 3));
                const int fortyTwo = 42;
                qEng.setProperty(nulName, qEng.toScriptValue<int>(fortyTwo));
                const QQuickJSValue v = qEng.evaluate(QStringLiteral(
                    "typeof globalThis['a\\u0000b'] !== 'undefined' ? globalThis['a\\u0000b'] : 'missing'"));
                TEST_RESULT("API: setProperty(QByteArray) keeps embedded NUL in the name",
                            v.toInt() == 42,
                            QStringLiteral("got='%1' - the name was truncated at the NUL").arg(v.toString()));
            }

            // (r13, D-62): the QQuickJSValue QByteArray overloads keep embedded NULs
            // as well (they used to truncate at constData()); V4's QString
            // path preserves such names (probed), and Qt 6 QJSValue has no
            // QByteArray overloads - the QString behavior is the reference.
            {
                QQuickJSValue holder = qEng.evaluate(QStringLiteral(
                    "({ 'p\\u0000q': 11, 'p': 22 })"));
                const QByteArray nulKey(QByteArray("p\0q", 3));
                TEST_RESULT("API: value property(QByteArray) keeps embedded NUL",
                            holder.property(nulKey).toInt() == 11
                                && holder.property(QByteArray("p")).toInt() == 22,
                            "the NUL name was truncated and hit the prefix property");
                TEST_RESULT("API: value hasProperty/hasOwnProperty(QByteArray) keep embedded NUL",
                            holder.hasProperty(nulKey) && holder.hasOwnProperty(nulKey)
                                && !holder.hasProperty(QByteArray("p\0z", 3)),
                            "the NUL name was truncated");
                const QQuickJSValue ok = qEng.evaluate(QStringLiteral("'ok'"));
                holder.setProperty(nulKey, ok);
                TEST_RESULT("API: value setProperty(QByteArray) keeps embedded NUL",
                            holder.property(nulKey).toString() == QStringLiteral("ok")
                                && holder.property(QByteArray("p")).toInt() == 22,
                            QStringLiteral("got='%1' - the write hit the prefix property")
                                .arg(holder.property(nulKey).toString()));
                // deleteProperty round trip on the NUL-named property
                TEST_RESULT("API: value deleteProperty(QByteArray) keeps embedded NUL",
                            holder.deleteProperty(nulKey)
                                && !holder.hasProperty(nulKey)
                                && holder.property(QByteArray("p")).toInt() == 22,
                            "delete missed the NUL-named property");
            }

            // (r13, D-62): object -> QVariantMap keeps NUL-bearing keys whole (V4
            // parity, probed: map size 2, keys 61 00 62 / 61 00 63); the
            // old atomName() truncated at the first NUL and silently merged
            // two distinct JS properties into one key.
            {
                const QVariant mv = qEng.evaluate(QStringLiteral(
                    "({ ['a\\u0000b']: 11, ['a\\u0000c']: 22 })")).toVariant();
                const QVariantMap map = mv.toMap();
                const QString keyB = QString::fromUtf8(QByteArray("a\0b", 3));
                const QString keyC = QString::fromUtf8(QByteArray("a\0c", 3));
                TEST_RESULT("Convert: toVariant keeps embedded-NUL map keys (V4 parity)",
                            map.size() == 2 && map.value(keyB).toInt() == 11 && map.value(keyC).toInt() == 22,
                            QStringLiteral("size=%1 - distinct keys merged at the NUL").arg(map.size()));
            }

            // Q3 (probed against live Qt 6.12): console.count() without
            // arguments uses the EMPTY label, like Qt's own qml console
            // (": 1"), not Chromium's "default".
            {
                QQuickJSEngine cEng;
                cEng.installExtensions(QQuickJSEngine::ConsoleExtension);
                bool sawEmptyLabel = false;
                {
                    MessageCaptureGuard guard;
                    cEng.evaluate(QStringLiteral("console.count()"));
                    for (const QString& m : g_capturedMessages)
                        if (m.contains(QLatin1String("[JS console] : 1"))) sawEmptyLabel = true;
                }
                TEST_RESULT("Console: count() without label prints ': 1' (qml parity)",
                            sawEmptyLabel,
                            "the empty label was replaced by a 'default' label");
            }

            // Argument counts (V4 parity, live Qt 6.12): missing arguments
            // throw "Insufficient arguments" instead of silently filling
            // value-initialized defaults; extra arguments are ignored; C++
            // default arguments resolve through moc's truncated synthetic
            // signatures; same-name overloads select by argument type.
            QQuickJSValue miss = qEng.evaluate(QStringLiteral(
                "try { testObj.needTwo(1); 'ran'; } catch (e) { e.message; }"));
            TEST_RESULT("QObject: missing arguments throw 'Insufficient arguments'",
                        miss.toString() == QStringLiteral("Insufficient arguments"),
                        QStringLiteral("got='%1' - missing arguments were silently defaulted").arg(miss.toString()));
            TEST_RESULT("QObject: extra arguments are ignored",
                        qEng.evaluate("testObj.needTwo(1, 'a', true)").toString() == QStringLiteral("1,a"),
                        "an extra argument changed the outcome");
            TEST_RESULT("QObject: default arguments resolve via truncated signatures",
                        qEng.evaluate("testObj.withDefaults(1)").toString() == QStringLiteral("1,7")
                            && qEng.evaluate("testObj.withDefaults(1, 2)").toString() == QStringLiteral("1,2"),
                        "the default-argument signature was not selected");
            TEST_RESULT("QObject: overloads select by argument type",
                        qEng.evaluate("testObj.ovOverload(42)").toString() == QStringLiteral("int:42")
                            && qEng.evaluate("testObj.ovOverload('x')").toString() == QStringLiteral("str:x"),
                        "the wrong overload was selected");

            // Stale deadline: after the previous run's timeout quota
            // expired, a C++-driven accessor entry (a getter through
            // property()) must start a FRESH quota, not inherit the dead
            // one. No evaluate() may run between the sleep and the read.
            {
                QQuickJSEngine tEng;
                tEng.setExecutionTimeoutMs(150);
                QQuickJSValue getterObj = tEng.evaluate(QStringLiteral(
                    "({ get slow() { var s = 0; for (var i = 0; i < 1000000; ++i) s += i; return s; } })"));
                QThread::msleep(400);   // the old deadline is long past
                QQuickJSValue v = getterObj.property(QStringLiteral("slow"));
                TEST_RESULT("Limit: stale deadline does not kill a fresh accessor entry",
                            !v.isError() && v.isNumber(),
                            QStringLiteral("v='%1' - the getter was interrupted by the previous run's deadline")
                                .arg(v.toString()));
            }

            // qsTr/qsTranslate/qsTrId with a non-integral n: ToInt32 coercion
            // (NaN -> 0, huge -> 0), no UB. The UBSan gate turns the old raw
            // double-to-int cast into a runtime error here.
            qEng.installExtensions(QQuickJSEngine::TranslationExtension);
            QQuickJSValue tr1 = qEng.evaluate(QStringLiteral("qsTr('src', '', NaN)"));
            QQuickJSValue tr2 = qEng.evaluate(QStringLiteral("qsTranslate('ctx', 'src', '', 1e100)"));
            QQuickJSValue tr3 = qEng.evaluate(QStringLiteral("qsTrId('id', NaN)"));
            TEST_RESULT("qsTr family: non-integral n coerces via ToInt32",
                        tr1.toString() == QStringLiteral("src")
                            && tr2.toString() == QStringLiteral("src")
                            && tr3.toString() == QStringLiteral("id"),
                        QStringLiteral("tr1='%1' tr2='%2' tr3='%3'")
                            .arg(tr1.toString()).arg(tr2.toString()).arg(tr3.toString()));

            delete robj;
        }
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

    // 15b. Typed fast-path roundtrips: property/method conversions for the
    // types the metacall sites convert directly (D-42). Dual-engine: V4
    // matches for the shared domain; the BigInt case below is engine-specific
    // (V4 rounds qint64 beyond 2^53 to a Number, we keep BigInt precision).
    {
        QJSEngine qtEng;
        QQuickJSEngine qEng;
        TestObject* qtObj = new TestObject();
        TestObject* qObj = new TestObject();
        QJSEngine::setObjectOwnership(qtObj, QJSEngine::CppOwnership);
        qtEng.globalObject().setProperty(QStringLiteral("testObj"), qtEng.newQObject(qtObj));
        qEng.globalObject().setProperty(QStringLiteral("testObj"), qEng.newQObject(qObj));
        auto parity = [&qtEng, &qEng](const char* name, const char* js) {
            QJSValue qtRes = qtEng.evaluate(QString::fromUtf8(js));
            QQuickJSValue qRes = qEng.evaluate(QString::fromUtf8(js));
            TEST_RESULT(name, !qRes.isError() && !qtRes.isError() && qtRes.toString() == qRes.toString(),
                        QStringLiteral("QJSEngine='%1', QuickJS='%2'").arg(qtRes.toString(), qRes.toString()));
        };
        parity("FastPath: float property write+read", "testObj.floatProp = 2.5; String(testObj.floatProp)");
        parity("FastPath: float property exact value", "testObj.floatProp = 1.25; testObj.floatProp === 1.25");
        parity("FastPath: uint property write+read", "testObj.uintProp = 3000000000; testObj.uintProp === 3000000000");
        parity("FastPath: qint64 property within double", "testObj.bigProp = 42; testObj.bigProp === 42");
        parity("FastPath: double coercion of qint64 matches (rounds)", "testObj.bigProp = 3.9; String(testObj.bigProp)");
        parity("FastPath: float method arg+return", "testObj.echoFloat(1.25) === 1.25");
        parity("FastPath: qint64 method arg+return", "testObj.echoBigInt(41) + 1 === 42");
        parity("FastPath: uint method arg+return", "testObj.echoUint(3000000000) === 3000000000");
        parity("FastPath: QDate property is a Date", "Object.prototype.toString.call(testObj.dateProp) === '[object Date]'");
        parity("FastPath: QTime property is a Date (V4 1971-04-01 anchor)", "Object.prototype.toString.call(testObj.timeProp) === '[object Date]'");
        parity("FastPath: QTime Date local getters", "var t = testObj.timeProp; t.getFullYear() + '-' + (t.getMonth()+1) + '-' + t.getDate() + ' ' + t.getHours() + ':' + t.getMinutes() + ':' + t.getSeconds() + '.' + t.getMilliseconds()");
        parity("FastPath: QTime Date epoch", "String(testObj.timeProp.getTime())");
        parity("FastPath: QTime method arg+return round-trip", "var t = testObj.echoTime(testObj.timeProp); t instanceof Date && t.getTime() === testObj.timeProp.getTime() ? 'ok' : 'bad'");
        parity("FastPath: string into QTime property parses", "testObj.timeProp = '09:05:07'; var t = testObj.timeProp; t.getHours() + ':' + t.getMinutes() + ':' + t.getSeconds()");
        parity("FastPath: Date into QTime arg takes local time-of-day", "var d = new Date(2020, 4, 17, 13, 45); var t = testObj.echoTime(d); t.getHours() + ':' + t.getMinutes()");
        parity("FastPath: number into QTime arg rejected", "try { testObj.echoTime(12345); 'no-throw' } catch (e) { e instanceof TypeError ? 'type-error' : 'other' }");
        // QDate -> Date anchors the UTC midnight of the date, and Date ->
        // QDate takes the local date (both probed against live Qt 6.12 in
        // UTC and Europe/Moscow). The write below goes through the
        // Date -> QDate local-date conversion, so dateProp holds
        // QDate(2020-05-17) in both engines before the epoch is read back;
        // the round trip then reports 'bad' in negative-offset zones on
        // BOTH engines - an inherited V4 wart, pinned as-is.
        parity("FastPath: QDate Date epoch is UTC midnight (V4 parity)", "testObj.dateProp = new Date(2020, 4, 17, 12, 0); String(testObj.dateProp.getTime())");
        parity("FastPath: QDate Date toISOString", "testObj.dateProp.toISOString()");
        parity("FastPath: QDate Date UTC getters", "var d = testObj.dateProp; d.getUTCFullYear() + '-' + (d.getUTCMonth()+1) + '-' + d.getUTCDate() + ' ' + d.getUTCHours() + ':' + d.getUTCMinutes() + ':' + d.getUTCSeconds()");
        parity("FastPath: QDate method arg+return round-trip", "var t = testObj.echoDate(testObj.dateProp); t instanceof Date && t.getTime() === testObj.dateProp.getTime() ? 'ok' : 'bad'");
        parity("FastPath: Date into QDate arg takes the local date", "var d = new Date(2020, 4, 17, 13, 45); var r = testObj.echoDate(d); r.getUTCFullYear() + '-' + (r.getUTCMonth()+1) + '-' + r.getUTCDate()");
        // The QTBUG-92466 edge (probed against live Qt 6.12 in
        // America/New_York): a date-only ISO string is the UTC midnight,
        // whose local wall time falls on the previous day in a
        // negative-offset zone - the UTC date wins, on both engines. A
        // NaN Date passes/reads as the invalid target value silently,
        // without a TypeError, in every date/time slot - also V4 parity,
        // probed in both zones.
        parity("FastPath: date-only ISO string into QDate arg keeps the date (QTBUG-92466)", "var t = testObj.echoDate(new Date('2020-05-17')); t.getTime() === new Date('2020-05-17').getTime() ? 'ok' : 'bad'");
        parity("FastPath: NaN Date into QDate arg is a silent invalid date (V4 parity)", "var t = testObj.echoDate(new Date(NaN)); isNaN(t.getTime()) ? 'invalid' : 'valid'");
        parity("FastPath: NaN Date into QDate property writes silently (V4 parity)", "testObj.dateProp = new Date(NaN); isNaN(testObj.dateProp.getTime()) ? 'invalid' : 'valid'");
        parity("FastPath: NaN Date into QTime arg is a silent invalid time (V4 parity)", "var t = testObj.echoTime(new Date(NaN)); isNaN(t.getTime()) ? 'invalid' : 'valid'");
        parity("FastPath: NaN Date into QDateTime arg is a silent invalid datetime (V4 parity)", "var t = testObj.echoDateTime(new Date(NaN)); isNaN(t.getTime()) ? 'invalid' : 'valid'");
        parity("FastPath: string -> float keeps generic coercion", "testObj.floatProp = '2.5'; testObj.floatProp === 2.5");
        QQuickJSValue bigRes = qEng.evaluate(QStringLiteral("testObj.bigProp = 9007199254740993n; testObj.bigProp === 9007199254740993n"));
        TEST_RESULT("FastPath: qint64 property keeps BigInt precision (engine-specific)",
                    bigRes.toBool(), QStringLiteral("res=%1").arg(bigRes.toString()));
        delete qtObj;
        delete qObj;
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

        // Stack-guard regression threads. These are crash-capable on unpatched
        // code (a misclamped guard line turns runaway recursion into SIGSEGV),
        // which is exactly why they live HERE: a crash must not mask the safe
        // tests' results printed above.
        {
            StackProbeThread thread;
            thread.setStackSize(512 * 1024);
            thread.start();
            thread.wait();
            TEST_RESULT("Limit: stack guard fires on a small thread stack",
                        thread.result == StackProbeThread::GuardFired,
                        QStringLiteral("small-stack thread outcome=%1").arg(int(thread.result)));
        }

        // Starved stack, the underflow regression: the whole thread stack
        // (128 KiB) is below the safety margin, so the anchor sits INSIDE
        // the margin - the exact case where the old clamp arithmetic
        // underflowed and installed an unreachable guard line (hard process
        // crash on the first runaway recursion). The engine must keep every
        // script entry a catchable error (or a normal value) and the process
        // alive. "1+1" is only guaranteed to be refused when the OS really
        // delivered the full starved window: a benign environment variance
        // (thread placement, sanitizer thread bootstrap) can leave the thread
        // a slightly larger usable window than requested, in which case the
        // trivial entry legitimately runs under a properly clamped budget.
        // Pre-fix code never gets this far: the underflowed budget kills the
        // whole process at the first entry (SIGSEGV), which is exactly what
        // this regression test pins.
        {
            StarvedStackThread thread;
            thread.setStackSize(128 * 1024);
            thread.start();
            thread.wait();
            TEST_RESULT("Limit: starved-stack entry refused, no underflow",
                        thread.result == StarvedStackThread::Refused
                            && thread.trivial != StarvedStackThread::NotRun,
                        QStringLiteral("starved thread outcome=%1 trivial=%2 - the process "
                                       "survived; pre-fix code died here with SIGSEGV")
                            .arg(int(thread.result))
                            .arg(int(thread.trivial)));
            // The registerModule hole (same starved anchor): registration is
            // refused (false) with a pending RangeError, and the follow-up
            // importModule() surfaces that pending error instead of loading.
            // Both halves are only guaranteed on the window that also
            // refused the trivial entry. In the benign-variance case (the
            // trivial entry ran; a Windows/ASan thread bootstrap can leave
            // the requested 128 KiB thread a larger usable window) the
            // registration must succeed, and importModule() must not return
            // a stack refusal: it resolves no registered bare name (Qt 6.12
            // parity, see "RegModule: invisible to global object and
            // importModule"), so the documented outcome is the plain
            // "could not load module filename" error, and a "Maximum call
            // stack size exceeded" here would be a false fire in a window
            // the guard itself measured as usable.
            TEST_RESULT("Limit: starved registerModule refused, import surfaces the refusal",
                        thread.trivial == StarvedStackThread::Refused
                            ? (!thread.registered
                               && thread.imported == StarvedStackThread::Refused)
                            : (thread.registered
                               && thread.imported != StarvedStackThread::Refused),
                        QStringLiteral("registered=%1 imported=%2 trivial=%3")
                            .arg(thread.registered)
                            .arg(int(thread.imported))
                            .arg(int(thread.trivial)));

            // Same starved anchor, pending-exception interplay: a C++-thrown
            // error preempts the refusal and is CONSUMED by it (pure C path),
            // never left dangling behind the starved entries.
            TEST_RESULT("Limit: starved evaluate consumes a pending exception",
                        thread.pendingConsumed && !thread.pendingWasRangeError,
                        QStringLiteral("consumed=%1 wasRangeError=%2")
                            .arg(thread.pendingConsumed)
                            .arg(thread.pendingWasRangeError));
        }

        // Re-entrant signal emission with a disconnect in the outer frame:
        // the receiver's execution state must be a DEPTH counter, not a
        // flag - the nested emission's exit must not clear the "executing"
        // state while the outer callback frame is still on the stack, or
        // the disconnect deletes the receiver under the live ExecGuard
        // (heap-use-after-free on unpatched code).
        {
            qInfo().noquote() << QStringLiteral(">>> Re-entrant signal + disconnect (use-after-free on unpatched code)");
            QQuickJSEngine qEng;
            TestObject* obj = new TestObject();
            qEng.globalObject().setProperty("testObj", qEng.newQObject(obj));
            QQuickJSValue r = qEng.evaluate(QStringLiteral(
                "var calls = 0;"
                "function cb(v) {"
                "    calls++;"
                "    if (calls === 1) {"
                "        testObj.value = 42;"                      // nested emission of the same signal
                "        testObj.valueChanged.disconnect(cb);"      // the outer frame is still executing
                "    }"
                "}"
                "testObj.valueChanged.connect(cb);"
                "testObj.value = 1;"
                "calls;"));
            TEST_RESULT("Signal: re-entrant emission + disconnect in the outer frame",
                        r.toInt() == 2 && obj->getValue() == 42,
                        QStringLiteral("calls=%1 value=%2 - the receiver died under a live frame")
                            .arg(r.toInt()).arg(obj->getValue()));
            delete obj;
        }

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

    // 19. QJSEngine Parity Stage 2: extensions, iterator, conversion behavior, sticky interrupt
    {
        // --- QQuickJSValueIterator ---
        {
            QQuickJSEngine qEng;
            QQuickJSValue obj = qEng.evaluate("({a: 1, b: 'x'})");

            QStringList names;
            QVariantHash values;
            QQuickJSValueIterator it(obj);
            TEST_RESULT("Iterator: hasNext before the first property", it.hasNext(), "hasNext false on a fresh iterator");
            while (it.hasNext()) {
                it.next();
                names.append(it.name());
                values.insert(it.name(), it.value().toVariant());
            }
            TEST_RESULT("Iterator: own properties enumerated",
                        names.size() == 2 && names.contains(QStringLiteral("a")) && names.contains(QStringLiteral("b"))
                            && values.value(QStringLiteral("a")).toInt() == 1
                            && values.value(QStringLiteral("b")).toString() == QLatin1String("x"),
                        QStringLiteral("names=%1").arg(names.join(u',')));
            TEST_RESULT("Iterator: exhausted after the last property",
                        !it.hasNext() && !it.next() && it.name() == QLatin1String("b")
                            && it.value().toString() == QLatin1String("x"),
                        "iterator exhausted state differs from the last-jumped-over semantics");

            // Live parity: the same object iterated through QJSValueIterator
            {
                QJSEngine qtEng;
                QJSValue qtObj = qtEng.evaluate("({a: 1, b: 'x'})");
                QStringList qtNames;
                QJSValueIterator qtIt(qtObj);
                while (qtIt.hasNext()) { qtIt.next(); qtNames.append(qtIt.name()); }
                QStringList sortedNames = names, sortedQtNames = qtNames;
                sortedNames.sort();
                sortedQtNames.sort();
                TEST_RESULT("Iterator: plain-object name parity with QJSValueIterator",
                            sortedNames == sortedQtNames,
                            QStringLiteral("ours=%1 Qt=%2").arg(sortedNames.join(u','), sortedQtNames.join(u',')));
            }

            // Arrays: own indices plus "length", symbol keys skipped (probe-verified Qt behavior)
            {
                QQuickJSValue arr = qEng.evaluate("[10, 20]");
                QStringList arrNames;
                QQuickJSValueIterator it2(arr);
                while (it2.hasNext()) { it2.next(); arrNames.append(it2.name()); }

                QJSEngine qtEng;
                QJSValue qtArr = qtEng.evaluate("[10, 20]");
                QStringList qtArrNames;
                QJSValueIterator qtIt2(qtArr);
                while (qtIt2.hasNext()) { qtIt2.next(); qtArrNames.append(qtIt2.name()); }
                QStringList sortedArrNames = arrNames, sortedQtArrNames = qtArrNames;
                sortedArrNames.sort();
                sortedQtArrNames.sort();
                TEST_RESULT("Iterator: array names parity with QJSValueIterator (length included)",
                            sortedArrNames == sortedQtArrNames && arrNames.contains(QStringLiteral("length")),
                            QStringLiteral("ours=%1 Qt=%2").arg(sortedArrNames.join(u','), sortedQtArrNames.join(u',')));

                it2 = arr;   // operator= restarts the iteration
                bool got0 = false, got1 = false;
                while (it2.hasNext()) {
                    it2.next();
                    if (it2.name() == QLatin1String("0")) got0 = it2.value().toInt() == 10;
                    if (it2.name() == QLatin1String("1")) got1 = it2.value().toInt() == 20;
                }
                TEST_RESULT("Iterator: operator= restarts and reads element values",
                            got0 && got1, "array element values not read after operator=");
            }

            // Prototype chain not followed; symbol-keyed properties skipped
            {
                QQuickJSValue protoObj = qEng.evaluate(
                    "(function(){ var o = {own: 1}; o[Symbol('s')] = 2; return o; })()");
                QStringList protoNames;
                QQuickJSValueIterator it3(protoObj);
                while (it3.hasNext()) { it3.next(); protoNames.append(it3.name()); }
                TEST_RESULT("Iterator: own properties only, symbols skipped",
                            protoNames == QStringList{QStringLiteral("own")},
                            QStringLiteral("names=%1").arg(protoNames.join(u',')));
            }

            // value() is lazy: enumeration runs no getters, value() does
            {
                QQuickJSValue getterObj = qEng.evaluate(
                    "({ get g() { globalThis.getterRuns = (globalThis.getterRuns||0)+1; return 7; },"
                    "   get boom() { throw new Error('getter'); } })");
                qEng.evaluate("globalThis.getterRuns = 0");
                QQuickJSValueIterator it4(getterObj);
                while (it4.hasNext()) { it4.next(); }   // name sweep only
                TEST_RESULT("Iterator: enumeration runs no getters",
                            qEng.globalObject().property(QStringLiteral("getterRuns")).toInt() == 0,
                            "enumeration triggered a getter");

                it4 = getterObj;
                int runsAfterValue = 0;
                while (it4.hasNext()) {
                    it4.next();
                    if (it4.name() == QLatin1String("g")) {
                        it4.value();
                        runsAfterValue = qEng.globalObject().property(QStringLiteral("getterRuns")).toInt();
                    }
                }
                TEST_RESULT("Iterator: value() runs the getter lazily",
                            runsAfterValue == 1, "value() did not run the getter exactly once");

                it4 = getterObj;
                bool threwInvalid = false;
                while (it4.hasNext()) {
                    it4.next();
                    if (it4.name() == QLatin1String("boom")) threwInvalid = !it4.value().isValid();
                }
                TEST_RESULT("Iterator: throwing getter yields an invalid value",
                            threwInvalid, "a throwing getter did not yield an invalid value");
            }

            // Non-object and invalid values yield exhausted iterators
            {
                QQuickJSValueIterator prim(qEng.evaluate("42"));
                QQuickJSValueIterator invalid{QQuickJSValue()};
                TEST_RESULT("Iterator: primitive and invalid values are exhausted",
                            !prim.hasNext() && !prim.next() && prim.name().isEmpty()
                                && !invalid.hasNext() && !invalid.next() && !invalid.value().isValid(),
                            "a non-object value produced a working iterator");
            }

            // Cross-engine assignment: the enumeration array belongs to the
            // runtime that allocated it; operator= must release it through the
            // previous engine before rebinding (a cross-runtime free would
            // corrupt the heap; ASan would catch it when it was wrong).
            {
                QQuickJSEngine engA;
                QQuickJSEngine engB;
                QQuickJSValueIterator it(engA.evaluate("({a: 1, b: 2})"));
                TEST_RESULT("Iterator: cross-engine assignment setup",
                            it.hasNext() && it.next() && it.name() == QLatin1String("a"),
                            "initial enumeration failed");
                it = engB.evaluate("({x: 10, y: 20, z: 30})");
                QStringList names;
                while (it.hasNext()) { it.next(); names << it.name(); }
                TEST_RESULT("Iterator: assignment across engines rebinds safely",
                            names == QStringList({QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")}),
                            QStringLiteral("names=%1").arg(names.join(QLatin1String(","))));
            }

            // Non-enumerable own string properties stay enumerated, matching
            // QJSValueIterator (V4) - JS_GPN_STRING_MASK without ENUM_ONLY.
            {
                const QString script = QStringLiteral(
                    "(function(){ var o = {a: 1};"
                    "Object.defineProperty(o, 'ne', {value: 2, enumerable: false});"
                    "return o; })()");
                QQuickJSEngine qEng2;
                QJSEngine qtEng2;
                QQuickJSValueIterator oursIt(qEng2.evaluate(script));
                QJSValueIterator theirsIt(qtEng2.evaluate(script));
                QStringList ours, theirs;
                while (oursIt.hasNext()) { oursIt.next(); ours << oursIt.name(); }
                while (theirsIt.hasNext()) { theirsIt.next(); theirs << theirsIt.name(); }
                TEST_RESULT("Iterator: non-enumerable own properties parity with QJSValueIterator",
                            ours == theirs,
                            QStringLiteral("ours=[%1] QJSEngine=[%2]").arg(ours.join(','), theirs.join(',')));
            }
        }

        // --- toVariant(ObjectConversionBehavior) ---
        {
            QQuickJSEngine qEng;
            QVariant fnR = qEng.evaluate("(function(){ return 1; })").toVariant(QQuickJSValue::RetainJSObjects);
            TEST_RESULT("toVariant: RetainJSObjects wraps a function",
                        fnR.userType() == qMetaTypeId<QQuickJSValue>() && fnR.value<QQuickJSValue>().isCallable(),
                        "function not retained");
            QVariant plainR = qEng.evaluate("({a: 1})").toVariant(QQuickJSValue::RetainJSObjects);
            TEST_RESULT("toVariant: RetainJSObjects wraps a plain object",
                        plainR.userType() == qMetaTypeId<QQuickJSValue>()
                            && plainR.value<QQuickJSValue>().property(QStringLiteral("a")).toInt() == 1,
                        "plain object not retained");
            QVariant arrR = qEng.evaluate("[1, 2, 3]").toVariant(QQuickJSValue::RetainJSObjects);
            TEST_RESULT("toVariant: RetainJSObjects wraps an array",
                        arrR.userType() == qMetaTypeId<QQuickJSValue>() && arrR.value<QQuickJSValue>().isArray(),
                        "array not retained");
            QVariant dateR = qEng.evaluate("new Date(1700000000000)").toVariant(QQuickJSValue::RetainJSObjects);
            TEST_RESULT("toVariant: RetainJSObjects keeps the native Date conversion",
                        dateR.userType() == QMetaType::QDateTime
                            && dateR.toDateTime().toMSecsSinceEpoch() == 1700000000000LL,
                        "Date conversion lost in retain mode");
            QVariant symR = qEng.newSymbol(QStringLiteral("mysym")).toVariant(QQuickJSValue::RetainJSObjects);
            TEST_RESULT("toVariant: RetainJSObjects wraps a symbol",
                        symR.userType() == qMetaTypeId<QQuickJSValue>() && symR.value<QQuickJSValue>().isSymbol(),
                        "symbol not retained");
            QVariant mapR = qEng.evaluate("new Map()").toVariant(QQuickJSValue::RetainJSObjects);
            TEST_RESULT("toVariant: RetainJSObjects wraps exotic objects",
                        mapR.userType() == qMetaTypeId<QQuickJSValue>(), "exotic object not retained");

            // Live parity: QJSEngine also retains plain objects in retain mode
            {
                QJSEngine qtEng;
                QVariant qtPlain = qtEng.evaluate("({a: 1})").toVariant(QJSValue::RetainJSObjects);
                QVariant qtFn = qtEng.evaluate("(function(){})").toVariant(QJSValue::RetainJSObjects);
                TEST_RESULT("toVariant: RetainJSObjects parity with QJSEngine",
                            qtPlain.userType() == qMetaTypeId<QJSValue>()
                                && qtFn.userType() == qMetaTypeId<QJSValue>()
                                && plainR.userType() == qMetaTypeId<QQuickJSValue>()
                                && fnR.userType() == qMetaTypeId<QQuickJSValue>(),
                            QStringLiteral("Qt plain=%1 fn=%2, ours plain=%3 fn=%4")
                                .arg(qtPlain.typeName(), qtFn.typeName(), plainR.typeName(), fnR.typeName()));
            }
        }

        // --- installExtensions(extensions, object) ---
        {
            QQuickJSEngine qEng;
            QQuickJSValue target = qEng.newObject();
            qEng.installExtensions(QQuickJSEngine::ConsoleExtension | QQuickJSEngine::TranslationExtension
                                       | QQuickJSEngine::GarbageCollectionExtension, target);
            TEST_RESULT("installExtensions: console installed on the target object",
                        target.property(QStringLiteral("console")).isObject()
                            && target.property(QStringLiteral("console")).property(QStringLiteral("log")).isCallable(),
                        "console missing on the target");
            TEST_RESULT("installExtensions: print installed on the target object",
                        target.property(QStringLiteral("print")).isCallable(), "print missing on the target");
            TEST_RESULT("installExtensions: translation functions installed on the target",
                        target.property(QStringLiteral("qsTr")).isCallable()
                            && target.property(QStringLiteral("qsTranslate")).isCallable()
                            && target.property(QStringLiteral("qsTrId")).isCallable(),
                        "translation functions missing on the target");
            TEST_RESULT("installExtensions: gc installed on the target",
                        target.property(QStringLiteral("gc")).isCallable(), "gc missing on the target");
            TEST_RESULT("installExtensions: global object untouched by a custom target",
                        qEng.globalObject().property(QStringLiteral("console")).isUndefined()
                            && qEng.globalObject().property(QStringLiteral("qsTr")).isUndefined(),
                        "extensions leaked onto the global object");
            TEST_RESULT("installExtensions: String.prototype.arg installed engine-wide",
                        qEng.evaluate("'%1'.arg(7)").toString() == QLatin1String("7"),
                        "String.prototype.arg missing");

            // A non-object target falls back to the global object (QJSEngine parity)
            {
                QQuickJSEngine qEng2;
                qEng2.installExtensions(QQuickJSEngine::ConsoleExtension, qEng2.evaluate("42"));
                TEST_RESULT("installExtensions: non-object target falls back to the global object",
                            qEng2.globalObject().property(QStringLiteral("console")).isObject()
                                && qEng2.globalObject().property(QStringLiteral("print")).isCallable(),
                            "console not installed on the global object");
            }

            // A value owned by another engine is rejected
            {
                QQuickJSEngine other;
                QQuickJSEngine qEng3;
                QQuickJSValue foreign = other.newObject();
                WarningCaptureGuard capture;   // the rejection warning must be emitted
                qEng3.installExtensions(QQuickJSEngine::ConsoleExtension, foreign);
                TEST_RESULT("installExtensions: foreign-engine value rejected",
                            foreign.property(QStringLiteral("console")).isUndefined()
                                && !qEng3.globalObject().property(QStringLiteral("console")).isCallable()
                                && g_capturedWarnings.size() == 1,
                            QStringLiteral("warnings=%1").arg(g_capturedWarnings.join(u',')));
            }

            // Live parity with QJSEngine
            {
                QJSEngine qtEng;
                QJSValue qtTarget = qtEng.newObject();
                qtEng.installExtensions(QJSEngine::ConsoleExtension, qtTarget);
                TEST_RESULT("installExtensions: custom-target parity with QJSEngine",
                            qtTarget.property(QStringLiteral("console")).isObject()
                                && qtTarget.property(QStringLiteral("print")).isCallable()
                                && qtEng.globalObject().property(QStringLiteral("console")).isUndefined(),
                            "QJSEngine custom-target behavior differs");
            }
        }

        // --- Console extension ---
        {
            QQuickJSEngine qEng;
            qEng.installExtensions(QQuickJSEngine::ConsoleExtension);
            TEST_RESULT("Console: full method surface",
                        qEng.evaluate("typeof console.assert").toString() == QLatin1String("function")
                            && qEng.evaluate("typeof console.count").toString() == QLatin1String("function")
                            && qEng.evaluate("typeof console.time").toString() == QLatin1String("function")
                            && qEng.evaluate("typeof console.timeEnd").toString() == QLatin1String("function")
                            && qEng.evaluate("typeof console.trace").toString() == QLatin1String("function")
                            && qEng.evaluate("typeof console.exception").toString() == QLatin1String("function")
                            && qEng.evaluate("typeof print").toString() == QLatin1String("function"),
                        "console method missing");

            {
                QJSEngine qtEng;
                qtEng.installExtensions(QJSEngine::ConsoleExtension);
                TEST_RESULT("Console: method-surface parity with QJSEngine",
                            qtEng.evaluate("typeof print").toString() == QLatin1String("function")
                                && qtEng.evaluate("typeof console.assert").toString() == QLatin1String("function")
                                && qtEng.evaluate("typeof console.trace").toString() == QLatin1String("function"),
                            "QJSEngine console surface differs");
            }

            TEST_RESULT("Console: print behaves like console.log",
                        qEng.evaluate("print('hello')").isUndefined() && !qEng.evaluate("print('hello')").isError(),
                        "print failed");
            TEST_RESULT("Console: assert with a truthy expression returns undefined",
                        qEng.evaluate("console.assert(true, 'never printed')").isUndefined(),
                        "truthy assert failed");
            TEST_RESULT("Console: assert with a falsy expression returns undefined (prints)",
                        qEng.evaluate("console.assert(false, 'failure detail')").isUndefined(),
                        "falsy assert failed");
            TEST_RESULT("Console: assert without arguments throws",
                        qEng.evaluate("console.assert()").isError(),
                        "missing-argument assert did not throw");
            TEST_RESULT("Console: time without a label throws",
                        qEng.evaluate("console.time()").isError(), "console.time() did not throw");
            TEST_RESULT("Console: timeEnd without a label throws",
                        qEng.evaluate("console.timeEnd()").isError(), "console.timeEnd() did not throw");
            TEST_RESULT("Console: time/timeEnd round-trip and an unknown label are silent",
                        qEng.evaluate("console.time('t'); console.timeEnd('t'); console.timeEnd('unknown');").isUndefined(),
                        "time/timeEnd round-trip failed");
            TEST_RESULT("Console: trace with arguments throws",
                        qEng.evaluate("console.trace(1)").isError(), "console.trace(1) did not throw");
            TEST_RESULT("Console: exception without arguments throws",
                        qEng.evaluate("console.exception()").isError(), "console.exception() did not throw");
            TEST_RESULT("Console: exception with arguments returns undefined",
                        qEng.evaluate("console.exception(new Error('e'))").isUndefined(),
                        "console.exception failed");
            TEST_RESULT("Console: count returns undefined",
                        qEng.evaluate("console.count('c'); console.count('c');").isUndefined(),
                        "console.count failed");
            TEST_RESULT("Console: symbol arguments do not throw",
                        qEng.evaluate("console.log(Symbol('s')); console.count(Symbol('k'))").isUndefined(),
                        "a symbol argument broke console output");

            // console.* caller stacks are built on the internal Error class:
            // replacing globalThis.Error must not corrupt console output.
            bool consoleTampered = false;
            {
                QQuickJSEngine qEng3;
                qEng3.installExtensions(QQuickJSEngine::ConsoleExtension);
                MessageCaptureGuard capture;
                qEng3.evaluate("globalThis.Error = function(){ return {stack: 'HACKED'}; }; console.trace();");
                for (const QString& m : std::as_const(g_capturedMessages)) {
                    if (m.contains(QLatin1String("HACKED"))) consoleTampered = true;
                }
            }
            TEST_RESULT("Console: caller stack immune to globalThis.Error tampering",
                        !consoleTampered, "a replaced global Error leaked into console caller stacks");

            // Output formats follow V4's console: timeEnd prints "label: Nms",
            // count prints "label: N" (keyed by the printed label - a documented
            // divergence from V4's per-call-site counters).
            {
                QQuickJSEngine qEng2;
                qEng2.installExtensions(QQuickJSEngine::ConsoleExtension);
                MessageCaptureGuard capture;
                qEng2.evaluate("console.time('t1'); console.timeEnd('t1'); console.count('c1'); console.count('c1');");
                bool timeFormatOk = false;
                for (const QString& m : std::as_const(g_capturedMessages)) {
                    if (m.startsWith(QStringLiteral("[JS console] t1: ")) && m.endsWith(QStringLiteral("ms"))) timeFormatOk = true;
                }
                TEST_RESULT("Console: timeEnd output format",
                            timeFormatOk, QStringLiteral("captured=%1").arg(g_capturedMessages.join(u' ')));
                TEST_RESULT("Console: count output format (label-keyed)",
                            g_capturedMessages.contains(QStringLiteral("[JS console] c1: 1"))
                                && g_capturedMessages.contains(QStringLiteral("[JS console] c1: 2")),
                            QStringLiteral("captured=%1").arg(g_capturedMessages.join(u' ')));
            }
        }

        // --- Translation extension ---
        {
            QQuickJSEngine qEng;
            qEng.installExtensions(QQuickJSEngine::TranslationExtension);
            {
                QJSEngine qtEng;
                qtEng.installExtensions(QJSEngine::TranslationExtension);
                TEST_RESULT("Translation: function-surface parity with QJSEngine",
                            qtEng.evaluate("typeof qsTranslate").toString() == QLatin1String("function")
                                && qtEng.evaluate("typeof QT_TRANSLATE_NOOP").toString() == QLatin1String("function")
                                && qtEng.evaluate("typeof qsTrId").toString() == QLatin1String("function")
                                && qtEng.evaluate("typeof QT_TRID_NOOP").toString() == QLatin1String("function"),
                            "QJSEngine translation surface differs");
                TEST_RESULT("Translation: our function surface",
                            qEng.evaluate("typeof qsTranslate").toString() == QLatin1String("function")
                                && qEng.evaluate("typeof QT_TRANSLATE_NOOP").toString() == QLatin1String("function")
                                && qEng.evaluate("typeof QT_TR_NOOP").toString() == QLatin1String("function")
                                && qEng.evaluate("typeof qsTrId").toString() == QLatin1String("function")
                                && qEng.evaluate("typeof QT_TRID_NOOP").toString() == QLatin1String("function"),
                            "translation function missing");
                TEST_RESULT("Translation: qsTranslate parity",
                            qEng.evaluate("qsTranslate('ctx', 'source')").toString() == QLatin1String("source")
                                && qtEng.evaluate("qsTranslate('ctx', 'source')").toString() == QLatin1String("source"),
                            "qsTranslate diverged from QJSEngine");
                TEST_RESULT("Translation: qsTranslate with disambiguation and n",
                            qEng.evaluate("qsTranslate('ctx', '%n files', 'dis', 5)").toString() == QLatin1String("5 files")
                                && qtEng.evaluate("qsTranslate('ctx', '%n files', 'dis', 5)").toString() == QLatin1String("5 files"),
                            "qsTranslate extended form diverged from QJSEngine");
                TEST_RESULT("Translation: qsTrId parity (no translator: the id is returned)",
                            qEng.evaluate("qsTrId('my.id')").toString() == QLatin1String("my.id")
                                && qtEng.evaluate("qsTrId('my.id')").toString() == QLatin1String("my.id"),
                            "qsTrId diverged from QJSEngine");
                TEST_RESULT("Translation: NOOP parity",
                            qEng.evaluate("QT_TR_NOOP('x')").toString() == QLatin1String("x")
                                && qEng.evaluate("QT_TRANSLATE_NOOP('c', 's')").toString() == QLatin1String("s")
                                && qEng.evaluate("QT_TRID_NOOP('i')").toString() == QLatin1String("i")
                                && qtEng.evaluate("QT_TR_NOOP('x')").toString() == QLatin1String("x")
                                && qtEng.evaluate("QT_TRANSLATE_NOOP('c', 's')").toString() == QLatin1String("s")
                                && qtEng.evaluate("QT_TRID_NOOP('i')").toString() == QLatin1String("i"),
                            "NOOP behavior diverged from QJSEngine");
                TEST_RESULT("Translation: NOOP with too few arguments is undefined",
                            qEng.evaluate("QT_TR_NOOP()").isUndefined()
                                && qEng.evaluate("QT_TRANSLATE_NOOP('c')").isUndefined()
                                && qEng.evaluate("QT_TRID_NOOP()").isUndefined()
                                && qtEng.evaluate("QT_TR_NOOP()").isUndefined(),
                            "NOOP arity behavior diverged from QJSEngine");
                TEST_RESULT("Translation: String.prototype.arg parity",
                            qEng.evaluate("'%1 %2'.arg(5)").toString() == qtEng.evaluate("'%1 %2'.arg(5)").toString()
                                && qEng.evaluate("'%1'.arg(1.5)").toString() == qtEng.evaluate("'%1'.arg(1.5)").toString()
                                && qEng.evaluate("'%1'.arg(true)").toString() == qtEng.evaluate("'%1'.arg(true)").toString()
                                && qEng.evaluate("'%1'.arg('str')").toString() == qtEng.evaluate("'%1'.arg('str')").toString()
                                && qEng.evaluate("'%1 %2'.arg(5)").toString() == QLatin1String("5 %2"),
                            "String.prototype.arg diverged from QJSEngine");
            }
            TEST_RESULT("Translation: qsTr extended signature",
                        qEng.evaluate("qsTr('s', 'comment', 5)").toString() == QLatin1String("s"),
                        "qsTr extended form failed");
            TEST_RESULT("Translation: qsTr argument validation",
                        qEng.evaluate("qsTr()").isError()
                            && qEng.evaluate("qsTr(42)").isError()
                            && qEng.evaluate("qsTr('s', 42)").isError()
                            && qEng.evaluate("qsTr('s', 'c', 'x')").isError(),
                        "qsTr argument validation failed");
            TEST_RESULT("Translation: qsTranslate argument validation",
                        qEng.evaluate("qsTranslate('c')").isError()
                            && qEng.evaluate("qsTranslate('c', 's', 5)").isError()
                            && qEng.evaluate("qsTranslate(42, 's')").isError(),
                        "qsTranslate argument validation failed");
            TEST_RESULT("Translation: qsTranslate() missing-arguments message",
                        qEng.evaluate("qsTranslate('c')").toString().contains(QStringLiteral("requires at least two")),
                        "wrong missing-arguments message");
            TEST_RESULT("Translation: qsTrId argument validation",
                        qEng.evaluate("qsTrId()").isError()
                            && qEng.evaluate("qsTrId(42)").isError()
                            && qEng.evaluate("qsTrId('id', 'x')").isError(),
                        "qsTrId argument validation failed");
            TEST_RESULT("Translation: qsTrId wrong-type is a TypeError",
                        qEng.evaluate("qsTrId(42)").errorType() == QQuickJSValue::TypeError,
                        "qsTrId type validation is not a TypeError");
            TEST_RESULT("Translation: String.arg argument validation",
                        qEng.evaluate("'x'.arg()").isError() && qEng.evaluate("'x'.arg(1, 2)").isError(),
                        "String.arg argument validation failed");
        }

        // --- evaluate(..., exceptionStackTrace) ---
        {
            QQuickJSEngine qEng;
            QStringList stack;
            QQuickJSValue res = qEng.evaluate(
                "function f(){\n  throw new Error('boom');\n}\nf();", QStringLiteral("sc.js"), 1, &stack);
            TEST_RESULT("StackTrace: populated for an uncaught Error",
                        res.isError() && !stack.isEmpty(),
                        QStringLiteral("stack=%1").arg(stack.join(u' ')));
            bool frameOk = false;
            for (const QString& f : std::as_const(stack)) {
                if (f.startsWith(QStringLiteral("f:2:")) && f.endsWith(QStringLiteral(":sc.js"))) frameOk = true;
            }
            TEST_RESULT("StackTrace: frame format function:line:column:file",
                        frameOk, QStringLiteral("stack=%1").arg(stack.join(u' ')));

            QStringList clean;
            TEST_RESULT("StackTrace: empty without an exception",
                        qEng.evaluate("1 + 1", QString(), 1, &clean).toInt() == 2 && clean.isEmpty(),
                        "non-empty stack without an exception");

            // Nested evaluate() called from a Q_INVOKABLE (itself called from
            // the outer script): the inner error stack keeps the OUTER script
            // frames, matching live QJSEngine (V4 includes them — probe in
            // tools/probe-qt-apiaudit.cpp). Regression guard against adding
            // JS_EVAL_FLAG_BACKTRACE_BARRIER, which would strip the outer frames.
            {
                ThrowProbeObject probe;
                probe.qjsEngine = &qEng;
                qEng.globalObject().setProperty(QStringLiteral("obj"), qEng.newQObject(&probe));
                QQuickJSValue r = qEng.evaluate(
                    QStringLiteral("function outer() { return obj.runInnerEval(); }\nouter();"),
                    QStringLiteral("outer.js"));
                const bool hasInner = probe.innerStack.isEmpty()
                    ? false
                    : std::any_of(probe.innerStack.cbegin(), probe.innerStack.cend(),
                                  [](const QString& f) { return f.endsWith(QLatin1String("inner.js")); });
                const bool hasOuter = probe.innerStack.isEmpty()
                    ? false
                    : std::any_of(probe.innerStack.cbegin(), probe.innerStack.cend(),
                                  [](const QString& f) { return f.endsWith(QLatin1String("outer.js")); });
                TEST_RESULT("StackTrace: nested evaluate keeps outer frames",
                            !r.isError() && hasInner && hasOuter,
                            QStringLiteral("stack=[%1]").arg(probe.innerStack.join(u' ')));
            }

            {
                QJSEngine qtEng;
                QStringList qtStack;
                QJSValue qtRes = qtEng.evaluate(
                    "function f(){\n  throw new Error('boom');\n}\nf();", QStringLiteral("sc.js"), 1, &qtStack);
                bool qtFrame = false;
                for (const QString& f : std::as_const(qtStack)) {
                    if (f.contains(QStringLiteral(":2:"))) qtFrame = true;
                }
                TEST_RESULT("StackTrace: parity with QJSEngine (throwing frame at line 2)",
                            qtRes.isError() && !qtStack.isEmpty() && qtFrame && frameOk,
                            QStringLiteral("Qt=%1 ours=%2").arg(qtStack.join(u' '), stack.join(u' ')));
            }

            QStringList plainThrow;
            qEng.evaluate("throw 'str'", QString(), 1, &plainThrow);
            TEST_RESULT("StackTrace: non-Error throw yields an empty list",
                        plainThrow.isEmpty(), QStringLiteral("stack=%1").arg(plainThrow.join(u' ')));

            QStringList caught;
            qEng.evaluate("try { throw new Error('x') } catch(e) { }", QString(), 1, &caught);
            TEST_RESULT("StackTrace: caught exception yields an empty list",
                        caught.isEmpty(), QStringLiteral("stack=%1").arg(caught.join(u' ')));
        }

        // --- isSymbol()/isPromise() ---
        {
            QQuickJSEngine qEng;
            TEST_RESULT("API: isPromise",
                        qEng.evaluate("Promise.resolve(1)").isPromise()
                            && qEng.evaluate("(async function(){})()").isPromise()
                            && !qEng.evaluate("({})").isPromise()
                            && !qEng.evaluate("(function(){})").isPromise(),
                        "isPromise failed");
            TEST_RESULT("API: isSymbol",
                        qEng.newSymbol(QStringLiteral("d")).isSymbol()
                            && qEng.evaluate("Symbol('x')").isSymbol()
                            && !qEng.evaluate("'s'").isSymbol()
                            && !qEng.evaluate("42").isSymbol()
                            && !qEng.evaluate("({})").isSymbol(),
                        "isSymbol failed");
        }

        // --- Sticky interrupt: live parity with QJSEngine ---
        {
            QJSEngine qtEng;
            qtEng.setInterrupted(true);
            QJSValue qtRes = qtEng.evaluate("1 + 1");
            qtEng.setInterrupted(false);
            TEST_RESULT("Interrupt: sticky semantics parity with QJSEngine",
                        qtRes.isError() && qtEng.evaluate("1 + 1").toInt() == 2,
                        QStringLiteral("Qt evaluate result: %1").arg(qtRes.toString()));
        }
    }

    // 20. QJSEngine Parity Stage 3: throwError/hasError/catchError
    // Semantics pinned against live Qt 6.12 (scripts/probe-qt-throw.cpp):
    // a pending error preempts evaluate()/call() without executing the code,
    // evaluate() and call() clear hasError(), catchError() returns undefined
    // when nothing is pending, and exceptions of signal handlers are
    // reported without aborting the emission.
    {
        // --- direct dual-engine semantics (C++ level) ---
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            qtEng.throwError(QStringLiteral("plain boom"));
            qEng.throwError(QStringLiteral("plain boom"));
            TEST_RESULT("Throw: hasError after throwError",
                        qtEng.hasError() && qEng.hasError(), "hasError false right after throwError");

            QJSValue qtC1 = qtEng.catchError();
            QQuickJSValue qC1 = qEng.catchError();
            TEST_RESULT("Throw: catchError returns the thrown error",
                        qtC1.isError() && qC1.isError()
                            && qtC1.toString() == QLatin1String("Error: plain boom")
                            && qC1.toString() == QLatin1String("Error: plain boom")
                            && qtC1.errorType() == QJSValue::GenericError
                            && qC1.errorType() == QQuickJSValue::GenericError,
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtC1.toString(), qC1.toString()));
            TEST_RESULT("Throw: hasError false after catchError",
                        !qtEng.hasError() && !qEng.hasError(), "catchError did not clear the pending error");

            QJSValue qtC2 = qtEng.catchError();
            QQuickJSValue qC2 = qEng.catchError();
            TEST_RESULT("Throw: catchError without pending is undefined",
                        qtC2.isUndefined() && qC2.isUndefined(), "catchError with nothing pending must be undefined");
        }

        // typed errors: class preserved through throwError/catchError
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            qtEng.throwError(QJSValue::RangeError, QStringLiteral("range boom"));
            qEng.throwError(QQuickJSValue::RangeError, QStringLiteral("range boom"));
            QJSValue qtR = qtEng.catchError();
            QQuickJSValue qR = qEng.catchError();
            TEST_RESULT("Throw: RangeError class preserved",
                        qtR.toString() == QLatin1String("RangeError: range boom")
                            && qR.toString() == QLatin1String("RangeError: range boom")
                            && qtR.errorType() == QJSValue::RangeError && qR.errorType() == QQuickJSValue::RangeError,
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtR.toString(), qR.toString()));

            qtEng.throwError(QJSValue::EvalError, QStringLiteral("eval boom"));
            qEng.throwError(QQuickJSValue::EvalError, QStringLiteral("eval boom"));
            QJSValue qtE = qtEng.catchError();
            QQuickJSValue qE = qEng.catchError();
            TEST_RESULT("Throw: EvalError class preserved",
                        qtE.errorType() == QJSValue::EvalError && qE.errorType() == QQuickJSValue::EvalError
                            && qtE.toString() == QLatin1String("EvalError: eval boom")
                            && qE.toString() == QLatin1String("EvalError: eval boom"),
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtE.toString(), qE.toString()));

            // default (empty) message: toString keeps the class prefix only
            qtEng.throwError(QJSValue::RangeError);
            qEng.throwError(QQuickJSValue::RangeError);
            QJSValue qtEmpty = qtEng.catchError();
            QQuickJSValue qEmpty = qEng.catchError();
            TEST_RESULT("Throw: empty message keeps the error class",
                        qtEmpty.toString() == QLatin1String("RangeError") && qEmpty.toString() == QLatin1String("RangeError"),
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtEmpty.toString(), qEmpty.toString()));
        }

        // a pending error preempts evaluate(): the script is not executed
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            qtEng.throwError(QStringLiteral("pending boom"));
            qEng.throwError(QStringLiteral("pending boom"));
            QJSValue qtR = qtEng.evaluate(QStringLiteral("globalThis.ran = 1; 1 + 1"));
            QQuickJSValue qR = qEng.evaluate(QStringLiteral("globalThis.ran = 1; 1 + 1"));
            TEST_RESULT("Throw: pending error preempts evaluate",
                        qtR.isError() && qR.isError()
                            && qtR.toString() == QLatin1String("Error: pending boom")
                            && qR.toString() == QLatin1String("Error: pending boom"),
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtR.toString(), qR.toString()));
            TEST_RESULT("Throw: preempted evaluate did not run the script",
                        qtEng.globalObject().property(QStringLiteral("ran")).isUndefined()
                            && qEng.globalObject().property(QStringLiteral("ran")).isUndefined(),
                        "script side effect visible after a preempted evaluate");
            TEST_RESULT("Throw: hasError false after preempted evaluate",
                        !qtEng.hasError() && !qEng.hasError(), "preempted evaluate left the error pending");
        }

        // (r13, D-62): the pending-exception REPORT itself runs user JS (a hostile
        // "stack" getter, toString overrides, a C++ slot behind
        // uncaughtException()). It must run under the entry bookkeeping: a
        // re-entrant evaluate() from inside the report is a NESTED entry -
        // it may not drain the microtask queue (run-to-completion belongs
        // to the outer tick) and may not re-arm the guard/quota machinery
        // as a false top level. Pre-fix, the re-entrant entry saw
        // m_evalDepth == 0 and drained the queue during the report.
        {
            QQuickJSEngine qEng;
            TestObject* obj = new TestObject();
            obj->setJsEngine(&qEng);
            qEng.globalObject().setProperty(QStringLiteral("testObj"), qEng.newQObject(obj));
            const QQuickJSValue hostile = qEng.evaluate(QStringLiteral(
                "var microLog = [];"
                "var reenterResult = 'not-run';"
                "var hostile = new Error('pending boom');"
                "Object.defineProperty(hostile, 'stack', {"
                "    get: function() {"
                "        reenterResult = String(testObj.reenterEval("
                "            'Promise.resolve().then(function(){ microLog.push(\"micro\"); }); 21+21'));"
                "        return '    at fake (f.js:1:2)';"
                "    }"
                "});"
                "hostile;"));
            qEng.throwError(hostile);
            QStringList stack;
            QQuickJSValue r = qEng.evaluate(QStringLiteral("'not executed'"),
                                            QStringLiteral("preempted.js"), 1, &stack);
            TEST_RESULT("Reentry: pending-exception report survives a hostile stack getter",
                        r.isError() && r.toString() == QLatin1String("Error: pending boom"),
                        QStringLiteral("got='%1'").arg(r.toString()));
            // the fake frame returned by the hostile getter flowed through
            // the stack parser (func:line:col:file)
            TEST_RESULT("Reentry: hostile stack getter output parsed into the trace",
                        stack.size() == 1 && stack.first() == QLatin1String("fake:1:2:f.js"),
                        QStringLiteral("got=%1").arg(stack.join(QLatin1Char('|'))));
            // The script body of this very evaluate reads microLog BEFORE
            // its own end-of-tick drain: a nested (non-draining) re-entrant
            // entry leaves it empty here; a false top level would have run
            // the microtask during the report already. This must be the
            // FIRST evaluate after the preempted one - any intermediate
            // top-level entry would drain the queue itself.
            TEST_RESULT("Reentry: nested evaluate during the report does not drain microtasks",
                        qEng.evaluate(QStringLiteral("microLog.length")).toInt() == 0,
                        QStringLiteral("got=%1 - the queue was drained by a false top level")
                            .arg(qEng.evaluate(QStringLiteral("microLog.length")).toInt()));
            TEST_RESULT("Reentry: the stack getter's nested evaluate ran and returned",
                        qEng.evaluate(QStringLiteral("reenterResult")).toString() == QLatin1String("42"),
                        QStringLiteral("got='%1'").arg(qEng.evaluate(QStringLiteral("reenterResult")).toString()));
            TEST_RESULT("Reentry: the microtask runs on the next top-level entry",
                        qEng.evaluate(QStringLiteral("microLog.join(',')")).toString() == QLatin1String("micro"),
                        QStringLiteral("got='%1'").arg(qEng.evaluate(QStringLiteral("microLog.join(',')")).toString()));
            TEST_RESULT("Reentry: engine consistent after the hostile report",
                        qEng.evaluate(QStringLiteral("1+1")).toInt() == 2 && !qEng.hasError(),
                        "engine damaged");
            delete obj;
        }

        // (r13, D-62): QQuickJSEngine::setProperty() on a script-installed global
        // accessor runs the setter - user JS. It needs the entry bookkeeping
        // (QQuickJSValue::setProperty has had it all along): the setter's
        // nested engine calls are nested entries, and its JS runs under a
        // FRESH quota instead of inheriting an expired timeout deadline.
        {
            QQuickJSEngine qEng;
            TestObject* obj = new TestObject();
            obj->setJsEngine(&qEng);
            qEng.globalObject().setProperty(QStringLiteral("testObj"), qEng.newQObject(obj));
            const QQuickJSValue setterSetup = qEng.evaluate(QStringLiteral(
                "var setterLog = [];"
                "Object.defineProperty(globalThis, 'gHostile', {"
                "    set: function(v) { setterLog.push(v);"
                "        String(testObj.reenterEval('Promise.resolve().then(function(){ setterLog.push(\"micro\"); }); 5')); }"
                "});"));
            TEST_RESULT("Reentry: setter fixture installed",
                        !setterSetup.isError(),
                        QStringLiteral("setup error: %1").arg(setterSetup.toString()));
            qEng.setProperty(QStringLiteral("gHostile"), QVariant(7));
            // Drain check FIRST: the script body of this very evaluate reads
            // setterLog before its own end-of-tick drain; any intermediate
            // top-level entry would drain the queue itself.
            TEST_RESULT("Reentry: setter's nested evaluate does not drain microtasks",
                        qEng.evaluate(QStringLiteral("setterLog.length")).toInt() == 1,
                        QStringLiteral("got=%1 - the queue was drained by a false top level")
                            .arg(qEng.evaluate(QStringLiteral("setterLog.length")).toInt()));
            TEST_RESULT("Reentry: engine setProperty runs a script-installed global setter",
                        qEng.evaluate(QStringLiteral("setterLog[0]")).toInt() == 7,
                        QStringLiteral("got='%1' - the setter did not run")
                            .arg(qEng.evaluate(QStringLiteral("String(setterLog[0])")).toString()));
            TEST_RESULT("Reentry: setter microtask runs on the next top-level entry",
                        qEng.evaluate(QStringLiteral("setterLog.join(',')")).toString() == QLatin1String("7,micro"),
                        QStringLiteral("got='%1'").arg(qEng.evaluate(QStringLiteral("setterLog.join(',')")).toString()));
            // A throwing setter stays a silent no-op of the void API (the
            // pending exception is cleared), like any failed write.
            qEng.evaluate(QStringLiteral(
                "Object.defineProperty(globalThis, 'gThrow', {"
                "    set: function(v) { throw new Error('setter boom'); }"
                "});"));
            qEng.setProperty(QStringLiteral("gThrow"), QVariant(1));
            TEST_RESULT("Reentry: throwing setter is a silent no-op, engine consistent",
                        !qEng.hasError() && qEng.evaluate(QStringLiteral("1+1")).toInt() == 2,
                        "a throwing setter damaged the engine");
            delete obj;
        }

        // (r13, D-62, quota half of the setProperty fix): the setter's JS runs under
        // a fresh timeout deadline. Pre-fix, setProperty entered the engine
        // without a scope: with the previous top-level entry's deadline long
        // expired, the setter's first interrupt check aborted it with a
        // non-catchable "Interrupted" (the same stale-deadline class H-2
        // fixed for newErrorObject in r11).
        {
            QQuickJSEngine qEng;
            TestObject* obj = new TestObject();
            obj->setJsEngine(&qEng);
            qEng.globalObject().setProperty(QStringLiteral("testObj"), qEng.newQObject(obj));
            qEng.evaluate(QStringLiteral(
                "var marker = 'none';"
                "Object.defineProperty(globalThis, 'gSlow', {"
                "    set: function(v) { var t0 = Date.now();"
                "        while (Date.now() - t0 < 40) {} marker = 'set-done'; }"
                "});"));
            qEng.setExecutionTimeoutMs(250);
            qEng.evaluate(QStringLiteral("1+1"));   // deadline := now + 250ms
            QThread::msleep(400);                   // expires with no JS running
            qEng.setProperty(QStringLiteral("gSlow"), QVariant(1));   // ~40ms of setter JS
            TEST_RESULT("Reentry: setter runs under a fresh quota, not a stale deadline",
                        qEng.evaluate(QStringLiteral("marker")).toString() == QLatin1String("set-done"),
                        QStringLiteral("got='%1' - the setter inherited an expired timeout")
                            .arg(qEng.evaluate(QStringLiteral("marker")).toString()));
            qEng.setExecutionTimeoutMs(0);
            delete obj;
        }

        // a pending error preempts call(): the function is not invoked
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            QJSValue qtFn = qtEng.evaluate(QStringLiteral("(function(a){ globalThis.calledWith = a; return a * 2; })"));
            QQuickJSValue qFn = qEng.evaluate(QStringLiteral("(function(a){ globalThis.calledWith = a; return a * 2; })"));
            qtEng.throwError(QStringLiteral("pending call"));
            qEng.throwError(QStringLiteral("pending call"));
            QJSValue qtR = qtFn.call(QJSValueList() << 21);
            QQuickJSValue qR = qFn.call(QVariantList() << 21);
            TEST_RESULT("Throw: pending error preempts call",
                        qtR.isError() && qR.isError()
                            && qtR.toString() == QLatin1String("Error: pending call")
                            && qR.toString() == QLatin1String("Error: pending call"),
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtR.toString(), qR.toString()));
            TEST_RESULT("Throw: preempted call did not invoke the function",
                        qtEng.globalObject().property(QStringLiteral("calledWith")).isUndefined()
                            && qEng.globalObject().property(QStringLiteral("calledWith")).isUndefined(),
                        "function ran despite the pending error");
        }

        // evaluate() clears hasError() for its own script exceptions
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            qtEng.evaluate(QStringLiteral("throw 'script error'"));
            qEng.evaluate(QStringLiteral("throw 'script error'"));
            TEST_RESULT("Throw: hasError false after failing evaluate",
                        !qtEng.hasError() && !qEng.hasError(),
                        "evaluate() did not clear hasError() for its own exception");
        }

        // catchError() clears the error: subsequent evaluate() runs normally
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            qtEng.throwError(QStringLiteral("transient"));
            qEng.throwError(QStringLiteral("transient"));
            qtEng.catchError();
            qEng.catchError();
            QJSValue qtR = qtEng.evaluate(QStringLiteral("2 + 3"));
            QQuickJSValue qR = qEng.evaluate(QStringLiteral("2 + 3"));
            TEST_RESULT("Throw: evaluate runs normally after catchError",
                        qtR.toString() == QLatin1String("5") && qR.toString() == QLatin1String("5")
                            && !qtEng.hasError() && !qEng.hasError(),
                        QStringLiteral("Qt='%1' ours='%2'").arg(qtR.toString(), qR.toString()));
        }

        // throwError(value): custom properties survive the catch
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            QJSValue qtErr = qtEng.newErrorObject(QJSValue::TypeError, QStringLiteral("typed boom"));
            qtErr.setProperty(QStringLiteral("code"), 42);
            qtEng.throwError(qtErr);
            QQuickJSValue qErr = qEng.newErrorObject(QQuickJSValue::TypeError, QStringLiteral("typed boom"));
            qErr.setProperty(QStringLiteral("code"), 42);
            qEng.throwError(qErr);
            QJSValue qtC = qtEng.catchError();
            QQuickJSValue qC = qEng.catchError();
            TEST_RESULT("Throw: custom properties survive throwError(value)",
                        qtC.property(QStringLiteral("code")).toInt() == 42 && qC.property(QStringLiteral("code")).toInt() == 42
                            && qtC.errorType() == QJSValue::TypeError && qC.errorType() == QQuickJSValue::TypeError,
                        QStringLiteral("Qt code=%1 ours code=%2")
                            .arg(qtC.property(QStringLiteral("code")).toInt())
                            .arg(qC.property(QStringLiteral("code")).toInt()));
        }

        // newErrorObject(NoError) yields undefined, throwError(NoError) is a no-op
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            TEST_RESULT("Throw: newErrorObject(NoError) is undefined",
                        qtEng.newErrorObject(QJSValue::NoError, QStringLiteral("m")).isUndefined()
                            && qEng.newErrorObject(QQuickJSValue::NoError, QStringLiteral("m")).isUndefined(),
                        "newErrorObject(NoError) must be undefined");
            qtEng.throwError(QJSValue::NoError, QStringLiteral("ignored"));
            qEng.throwError(QQuickJSValue::NoError, QStringLiteral("ignored"));
            TEST_RESULT("Throw: throwError(NoError) is a no-op",
                        !qtEng.hasError() && !qEng.hasError(),
                        "throwError(NoError) must not leave a pending error");
        }

        // --- JS-visible delivery through native call boundaries ---
        compareThrowEngines("Throw: slot error caught in JS",
                            "(function(){ try { obj.fail(); return 'no'; } catch(e) { return 'caught:' + e.message; } })()");
        compareThrowEngines("Throw: slot error uncaught",
                            "obj.fail()");
        compareThrowEngines("Throw: typed slot error uncaught",
                            "obj.failTyped()");
        compareThrowEngines("Throw: value slot error uncaught",
                            "obj.failValue()");
        compareThrowEngines("Throw: getter error caught in JS",
                            "(function(){ try { return 'got:' + obj.throwingProp; } catch(e) { return 'getter-caught:' + e.message; } })()");
        compareThrowEngines("Throw: setter error caught in JS (strict)",
                            "(function(){ 'use strict'; try { obj.throwingProp = 5; return 'set-ok'; } catch(e) { return 'setter-caught:' + e.message; } })()");
        compareThrowEngines("Throw: setter error caught in JS (sloppy)",
                            "(function(){ try { obj.throwingProp = 6; return 'set-ok'; } catch(e) { return 'setter-caught:' + e.message; } })()");
        compareThrowEngines("Throw: default value throws undefined",
                            "(function(){ try { obj.throwDefault(); return 'no'; } catch(e) { return 'caught-default:' + String(e); } })()");
        compareThrowEngines("Throw: signal handler exception does not abort emission",
                            "(function(){ obj.ping.connect(function(){ throw 'handler boom'; }); obj.emitPing(); return 'emit-ok'; })()");

        // hasError() is already true inside the slot that threw
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            ThrowProbeObject qtObj, qObj;
            qtObj.qtEngine = &qtEng;
            qObj.qjsEngine = &qEng;
            qtEng.globalObject().setProperty(QStringLiteral("obj"), qtEng.newQObject(&qtObj));
            qEng.globalObject().setProperty(QStringLiteral("obj"), qEng.newQObject(&qObj));
            QJSValue qtR = qtEng.evaluate(QStringLiteral("(function(){ try { obj.failAndRecord(); return 'no'; } catch(e) { return 'ok:' + e.message; } })()"));
            QQuickJSValue qR = qEng.evaluate(QStringLiteral("(function(){ try { obj.failAndRecord(); return 'no'; } catch(e) { return 'ok:' + e.message; } })()"));
            TEST_RESULT("Throw: hasError true inside the throwing slot",
                        qtObj.sawError && qObj.sawError && !qtEng.hasError() && !qEng.hasError()
                            && qtR.toString() == QLatin1String("ok:record boom") && qR.toString() == QLatin1String("ok:record boom"),
                        QStringLiteral("Qt saw=%1 ours saw=%2").arg(qtObj.sawError).arg(qObj.sawError));
        }

        // --- QQuickJSEngine-specific delivery details ---
        {
            // The preempted evaluate reports the error through uncaughtException
            QQuickJSEngine qEng;
            int reports = 0;
            QString msg;
            QObject::connect(&qEng, &QQuickJSEngine::uncaughtException, [&](const QString& m, int) { reports++; msg = m; });
            qEng.throwError(QStringLiteral("pending reported"));
            QQuickJSValue r = qEng.evaluate(QStringLiteral("1 + 1"));
            TEST_RESULT("Throw: preempted evaluate reports via uncaughtException",
                        r.isError() && reports == 1 && msg.contains(QLatin1String("pending reported")),
                        QStringLiteral("reports=%1 msg=%2").arg(reports).arg(msg));

            // A value from another engine is rejected with a warning
            QQuickJSEngine otherEngine;
            QQuickJSValue foreign = otherEngine.newErrorObject(QQuickJSValue::TypeError, QStringLiteral("foreign"));
            {
                WarningCaptureGuard guard;
                qEng.throwError(foreign);
                TEST_RESULT("Throw: cross-engine value rejected with warning",
                            !qEng.hasError() && !g_capturedWarnings.isEmpty()
                                && g_capturedWarnings.first().contains(QLatin1String("another engine")),
                            QStringLiteral("hasError=%1 warnings=%2").arg(qEng.hasError()).arg(g_capturedWarnings.size()));
            }
        }
    }

    // 21. QJSEngine Parity Stage 4: ES modules (importModule / registerModule)
    {
        QTemporaryDir moduleTmp;
        QDir moduleDir(moduleTmp.path());
        const QString modulePath = moduleDir.path();

        auto writeModule = [&moduleDir](const QString& name, const QString& code) {
            QFile f(moduleDir.filePath(name));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(code.toUtf8());
            }
        };

        writeModule(QStringLiteral("mod.mjs"),
            "export const x = 42;\n"
            "export function f(){ return 7; }\n"
            "export default \"defval\";\n");
        writeModule(QStringLiteral("counter.mjs"),
            "export let n = 0;\n"
            "export function bump(){ n++; return n; }\n");
        writeModule(QStringLiteral("calls.mjs"),
            "import { f } from \"./mod.mjs\";\n"
            "export const seven = f();\n");
        writeModule(QStringLiteral("bump1.mjs"),
            "import { bump } from \"./counter.mjs\";\n"
            "export const first = bump();\n");
        writeModule(QStringLiteral("bump2.mjs"),
            "import { bump } from \"./counter.mjs\";\n"
            "export const second = bump();\n");
        writeModule(QStringLiteral("meta.mjs"),
            "export const hasUrl = 'url' in import.meta;\n"
            "export const keyCount = Object.keys(import.meta).length;\n");
        writeModule(QStringLiteral("synbad.mjs"), "export const x = ;\n");
        writeModule(QStringLiteral("throws.mjs"), "throw new TypeError('module boom');\n");
        writeModule(QStringLiteral("missingdep.mjs"),
            "import { q } from \"./gone.mjs\";\n"
            "export const q2 = 1;\n");
        writeModule(QStringLiteral("circA.mjs"),
            "import { getB } from \"./circB.mjs\";\n"
            "export const a = 1;\n"
            "export const bViaB = getB();\n");
        writeModule(QStringLiteral("circB.mjs"),
            "import { a } from \"./circA.mjs\";\n"
            "export function getB(){ return 11; }\n"
            "export function readA(){ return a; }\n");

        // Both engines import @p entry; the namespace property @p prop is
        // compared as a string. A null prop compares the import result itself
        // (failure flags and error class; the Qt failure form varies between
        // builds — normalized via moduleFailureClass()).
        auto importParity = [&moduleDir](const char* name, const QString& entry, const char* prop) {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            QJSValue qtRes = qtEng.importModule(entry);
            QQuickJSValue qRes = qEng.importModule(entry);
            const bool qtErr = qtRes.isError();
            const bool qErr = qRes.isError();
            if (qtErr || qErr) {
                // QuickJS must return a real error; Qt may report the failure
                // as an Error value or as a plain stringified value, so both
                // sides are normalized through moduleFailureClass().
                const int qtClass = moduleFailureClass(qtRes);
                const int qClass = static_cast<int>(qRes.errorType());
                TEST_RESULT(name,
                            qErr && qClass != static_cast<int>(QJSValue::NoError) && qtClass == qClass,
                            QStringLiteral("QJSEngine err=%1 (%2/%3), QuickJS err=%4 (%5/%6)")
                                .arg(qtErr).arg(qtRes.toString()).arg(qtClass)
                                .arg(qErr).arg(qRes.toString()).arg(qClass));
            } else {
                const QString qtV = prop ? qtRes.property(QLatin1String(prop)).toString()
                                         : qtRes.toString();
                const QString qV = prop ? qRes.property(QLatin1String(prop)).toString()
                                        : qRes.toString();
                TEST_RESULT(name, qtV == qV,
                            QStringLiteral("QJSEngine='%1', QuickJS='%2'").arg(qtV).arg(qV));
            }
        };
        const QString modPath = moduleDir.filePath(QStringLiteral("mod.mjs"));
        const QString counterPath = moduleDir.filePath(QStringLiteral("counter.mjs"));

        importParity("Module: namespace exports value", modPath, "x");
        importParity("Module: default export property", modPath, "default");
        importParity("Module: imported function call", moduleDir.filePath(QStringLiteral("calls.mjs")), "seven");
        importParity("Module: nested import shares instance",
                     moduleDir.filePath(QStringLiteral("bump1.mjs")), "first");
        importParity("Module: nested import second caller",
                     moduleDir.filePath(QStringLiteral("bump2.mjs")), "second");
        importParity("Module: syntax error is SyntaxError",
                     moduleDir.filePath(QStringLiteral("synbad.mjs")), nullptr);
        importParity("Module: top-level throw propagates",
                     moduleDir.filePath(QStringLiteral("throws.mjs")), nullptr);
        importParity("Module: missing file is SyntaxError error class",
                     moduleDir.filePath(QStringLiteral("nope.mjs")), nullptr);
        importParity("Module: missing dependency is SyntaxError error class",
                     moduleDir.filePath(QStringLiteral("missingdep.mjs")), nullptr);

        // Singleton through the C++ entry: two importModule() calls and one
        // cross-module import share one instance; different spellings of the
        // same file (relative vs absolute) normalize to one cache key.
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            QJSValue qtNs = qtEng.importModule(counterPath);
            QJSValue qtNs2 = qtEng.importModule(moduleDir.filePath(QStringLiteral("./counter.mjs")));
            QQuickJSValue qNs = qEng.importModule(counterPath);
            QQuickJSValue qNs2 = qEng.importModule(moduleDir.filePath(QStringLiteral("./counter.mjs")));
            const QString qt1 = qtNs.property(QStringLiteral("bump")).call().toString();
            const QString qt2 = qtNs2.property(QStringLiteral("bump")).call().toString();
            const QString q1 = qNs.property(QStringLiteral("bump")).call().toString();
            const QString q2 = qNs2.property(QStringLiteral("bump")).call().toString();
            TEST_RESULT("Module: C++ singleton + path normalization",
                        qt1 == q1 && qt2 == q2 && qt2.toInt() == 2 && q2.toInt() == 2,
                        QStringLiteral("Qt %1/%2, QuickJS %3/%4").arg(qt1).arg(qt2).arg(q1).arg(q2));
        }

        // A module that threw at top level stays cached: the re-import returns
        // its namespace without re-running the code.
        {
            const QString throwsPath = moduleDir.filePath(QStringLiteral("throws.mjs"));
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            (void)qtEng.importModule(throwsPath);
            (void)qEng.importModule(throwsPath);
            const bool qtAgain = qtEng.importModule(throwsPath).isError();
            const bool qAgain = qEng.importModule(throwsPath).isError();
            TEST_RESULT("Module: failed module is not re-imported", !qtAgain && !qAgain,
                        QStringLiteral("Qt err=%1, QuickJS err=%2").arg(qtAgain).arg(qAgain));
        }

        // import declarations are not valid classic scripts on either engine.
        compareEngines("Module: import statement is script syntax error",
                       "import { x } from \"./mod.mjs\";");

        // qrc modules: the same ":/..." path on both engines.
        importParity("Module: qrc resource import",
                     QStringLiteral(":/qjsmodules/modules/embedded.mjs"), "x");

        // ---- registerModule ----
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            QJSValue qtVal = qtEng.evaluate(QStringLiteral("({ a: 1, b: 'x' })"));
            QQuickJSValue qVal = qEng.evaluate(QStringLiteral("({ a: 1, b: 'x' })"));
            const bool qtOk = qtEng.registerModule(QStringLiteral("regmod.mjs"), qtVal);
            const bool qOk = qEng.registerModule(QStringLiteral("regmod.mjs"), qVal);
            writeModule(QStringLiteral("usereg.mjs"),
                "import v, { a, b } from \"regmod.mjs\";\n"
                "export const ok = (a === 1 && b === 'x' && v.a === 1);\n");
            const QString usePath = moduleDir.filePath(QStringLiteral("usereg.mjs"));
            QJSValue qtNs = qtEng.importModule(usePath);
            QQuickJSValue qNs = qEng.importModule(usePath);
            TEST_RESULT("RegModule: named + default exports",
                        qtOk && qOk && !qtNs.isError() && !qNs.isError()
                            && qtNs.property(QStringLiteral("ok")).toBool()
                            && qNs.property(QStringLiteral("ok")).toBool(),
                        QStringLiteral("register %1/%2, ok %3/%4")
                            .arg(qtOk).arg(qOk)
                            .arg(qtNs.property(QStringLiteral("ok")).toBool())
                            .arg(qNs.property(QStringLiteral("ok")).toBool()));
        }

        // An own enumerable "__proto__" property of the registered value is a
        // plain named export: the snapshot must define it as an own property,
        // not install it as the snapshot's prototype (a plain JS_SetProperty
        // on "__proto__" does exactly that, leaving the export declared but
        // never filled).
        {
            QQuickJSEngine qEng;
            QQuickJSValue val = qEng.evaluate(QString::fromUtf8(
                "(function(){ var o = { b: 2 }; "
                "Object.defineProperty(o, '__proto__', "
                "{ value: 42, enumerable: true, writable: true, configurable: true }); "
                "return o; })()"));
            const bool registered = qEng.registerModule(QStringLiteral("protomod.mjs"), val);
            writeModule(QStringLiteral("useproto.mjs"),
                "import * as m from \"protomod.mjs\";\n"
                "export const protoOk = (m.__proto__ === 42 && m.b === 2 && m.default.b === 2);\n");
            QQuickJSValue ns = qEng.importModule(moduleDir.filePath(QStringLiteral("useproto.mjs")));
            TEST_RESULT("RegModule: own '__proto__' property exports as a plain value",
                        registered && !ns.isError() && ns.property(QStringLiteral("protoOk")).toBool(),
                        QStringLiteral("registered=%1 protoOk=%2 - the export was lost to the prototype")
                            .arg(registered)
                            .arg(ns.property(QStringLiteral("protoOk")).toBool()));
        }

        // Registered modules are matched by the bare specifier from any
        // importer location (verified against live Qt 6.12).
        {
            moduleDir.mkdir(QStringLiteral("sub"));
            writeModule(QStringLiteral("sub/usereg.mjs"),
                "import v, { a } from \"regmod.mjs\";\n"
                "export const ok = (a === 1 && v.a === 1);\n");
            const QString subPath = moduleDir.filePath(QStringLiteral("sub/usereg.mjs"));
            importParity("RegModule: bare name from subdir", subPath, "ok");
        }

        // Non-object values: the default export is the value itself.
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            (void)qtEng.registerModule(QStringLiteral("numreg.mjs"), qtEng.toScriptValue(42));
            (void)qEng.registerModule(QStringLiteral("numreg.mjs"), qEng.toScriptValue(42));
            writeModule(QStringLiteral("usenum.mjs"),
                "import n from \"numreg.mjs\";\n"
                "export const N = n;\n");
            importParity("RegModule: number default export",
                         moduleDir.filePath(QStringLiteral("usenum.mjs")), "N");
            writeModule(QStringLiteral("usenumbad.mjs"),
                "import { x } from \"numreg.mjs\";\n"
                "export const X = x;\n");
            importParity("RegModule: named import from number errors",
                         moduleDir.filePath(QStringLiteral("usenumbad.mjs")), nullptr);
        }

        // Snapshot: values (and getter results) are read at registration time.
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            QJSValue qtVal = qtEng.evaluate(QStringLiteral("({ v: 1 })"));
            QQuickJSValue qVal = qEng.evaluate(QStringLiteral("({ v: 1 })"));
            (void)qtEng.registerModule(QStringLiteral("snapmod.mjs"), qtVal);
            (void)qEng.registerModule(QStringLiteral("snapmod.mjs"), qVal);
            qtVal.setProperty(QStringLiteral("v"), 99);
            qVal.setProperty(QStringLiteral("v"), 99);
            writeModule(QStringLiteral("usesnap.mjs"),
                "import { v } from \"snapmod.mjs\";\n"
                "export const V = v;\n");
            importParity("RegModule: snapshot at registration",
                         moduleDir.filePath(QStringLiteral("usesnap.mjs")), "V");
        }

        // Non-enumerable properties are not exported as named exports, but
        // remain reachable through the default export.
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            const char* src = "(function(){ var o = { e: 1 }; "
                              "Object.defineProperty(o, 'hidden', { value: 5, enumerable: false }); "
                              "return o; })()";
            QJSValue qtVal = qtEng.evaluate(QString::fromUtf8(src));
            QQuickJSValue qVal = qEng.evaluate(QString::fromUtf8(src));
            (void)qtEng.registerModule(QStringLiteral("hiddenmod.mjs"), qtVal);
            (void)qEng.registerModule(QStringLiteral("hiddenmod.mjs"), qVal);
            writeModule(QStringLiteral("usehidden.mjs"),
                "import h, { e } from \"hiddenmod.mjs\";\n"
                "export const E = e;\n"
                "export const hasHidden = 'hidden' in h;\n");
            importParity("RegModule: non-enumerable via default only",
                         moduleDir.filePath(QStringLiteral("usehidden.mjs")), "E");
            {
                QJSEngine qtEng2;
                QQuickJSEngine qEng2;
                QJSValue qtVal2 = qtEng2.evaluate(QString::fromUtf8(src));
                QQuickJSValue qVal2 = qEng2.evaluate(QString::fromUtf8(src));
                (void)qtEng2.registerModule(QStringLiteral("hiddenmod.mjs"), qtVal2);
                (void)qEng2.registerModule(QStringLiteral("hiddenmod.mjs"), qVal2);
                QJSValue qtNs = qtEng2.importModule(moduleDir.filePath(QStringLiteral("usehidden.mjs")));
                QQuickJSValue qNs = qEng2.importModule(moduleDir.filePath(QStringLiteral("usehidden.mjs")));
                TEST_RESULT("RegModule: non-enumerable reachable via default",
                            qtNs.property(QStringLiteral("hasHidden")).toBool()
                                && qNs.property(QStringLiteral("hasHidden")).toBool(),
                            QStringLiteral("Qt %1, QuickJS %2")
                                .arg(qtNs.property(QStringLiteral("hasHidden")).toBool())
                                .arg(qNs.property(QStringLiteral("hasHidden")).toBool()));
            }
        }

        // Re-registration with the same name: the new value wins for new
        // imports (verified against live Qt 6.12).
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            (void)qtEng.registerModule(QStringLiteral("dupmod.mjs"), qtEng.evaluate(QStringLiteral("({ who: 'first' })")));
            (void)qEng.registerModule(QStringLiteral("dupmod.mjs"), qEng.evaluate(QStringLiteral("({ who: 'first' })")));
            (void)qtEng.registerModule(QStringLiteral("dupmod.mjs"), qtEng.evaluate(QStringLiteral("({ who: 'second' })")));
            (void)qEng.registerModule(QStringLiteral("dupmod.mjs"), qEng.evaluate(QStringLiteral("({ who: 'second' })")));
            writeModule(QStringLiteral("usedup.mjs"),
                "import d from \"dupmod.mjs\";\n"
                "export const W = d.who;\n");
            importParity("RegModule: re-registration replaces",
                         moduleDir.filePath(QStringLiteral("usedup.mjs")), "W");
        }

        // A "default" property on the value is ignored: the default export is
        // the value itself, like QJSEngine (code-review regression test).
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            const char* src = "({ a: 1, dflt: 9, default: 'ownprop' })";
            (void)qtEng.registerModule(QStringLiteral("defmod.mjs"), qtEng.evaluate(QString::fromUtf8(src)));
            (void)qEng.registerModule(QStringLiteral("defmod.mjs"), qEng.evaluate(QString::fromUtf8(src)));
            writeModule(QStringLiteral("usedef.mjs"),
                "import v, { a } from \"defmod.mjs\";\n"
                "export const ok = (v.a === 1 && v.dflt === 9 && v.default === 'ownprop');\n");
            importParity("RegModule: value 'default' property ignored",
                         moduleDir.filePath(QStringLiteral("usedef.mjs")), "ok");
        }

        // A throwing getter fails the registration and leaves the exception
        // pending; the next import surfaces it (live Qt 6.12 behavior).
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            const bool qtOk = qtEng.registerModule(QStringLiteral("getmod.mjs"),
                qtEng.evaluate(QStringLiteral("({ get g(){ throw new TypeError('getter-boom'); }, p: 5 })")));
            const bool qOk = qEng.registerModule(QStringLiteral("getmod.mjs"),
                qEng.evaluate(QStringLiteral("({ get g(){ throw new TypeError('getter-boom'); }, p: 5 })")));
            writeModule(QStringLiteral("useget.mjs"),
                "import { p } from \"getmod.mjs\";\n"
                "export const P = p;\n");
            QJSValue qtRes = qtEng.importModule(moduleDir.filePath(QStringLiteral("useget.mjs")));
            QQuickJSValue qRes = qEng.importModule(moduleDir.filePath(QStringLiteral("useget.mjs")));
            TEST_RESULT("RegModule: throwing getter fails registration",
                        !qtOk && !qOk && qtRes.isError() && qRes.isError()
                            && qtRes.errorType() == QJSValue::TypeError
                            && qRes.errorType() == QQuickJSValue::TypeError
                            && qtRes.toString() == qRes.toString(),
                        QStringLiteral("register %1/%2, import '%3' vs '%4'")
                            .arg(qtOk).arg(qOk).arg(qtRes.toString()).arg(qRes.toString()));
        }

        // Registered name vs a same-named real file: the bare specifier uses
        // the registration, the relative specifier uses the file.
        {
            writeModule(QStringLiteral("coexist.mjs"), "export const src = 'from-file';\n");
            writeModule(QStringLiteral("usecoexist.mjs"),
                "import { src } from \"coexist.mjs\";\n"
                "export const S = src;\n");
            writeModule(QStringLiteral("usecoexistrel.mjs"),
                "import { src } from \"./coexist.mjs\";\n"
                "export const S = src;\n");
            {
                QJSEngine qtEng;
                QQuickJSEngine qEng;
                (void)qtEng.registerModule(QStringLiteral("coexist.mjs"),
                    qtEng.evaluate(QStringLiteral("({ src: 'from-register' })")));
                (void)qEng.registerModule(QStringLiteral("coexist.mjs"),
                    qEng.evaluate(QStringLiteral("({ src: 'from-register' })")));
                QJSValue qtNs = qtEng.importModule(moduleDir.filePath(QStringLiteral("usecoexist.mjs")));
                QQuickJSValue qNs = qEng.importModule(moduleDir.filePath(QStringLiteral("usecoexist.mjs")));
                TEST_RESULT("RegModule: bare name beats same-named file",
                            qtNs.property(QStringLiteral("S")).toString() == QLatin1String("from-register")
                                && qNs.property(QStringLiteral("S")).toString() == QLatin1String("from-register"),
                            QStringLiteral("Qt '%1', QuickJS '%2'")
                                .arg(qtNs.property(QStringLiteral("S")).toString())
                                .arg(qNs.property(QStringLiteral("S")).toString()));
            }
            importParity("RegModule: relative path beats registration",
                         moduleDir.filePath(QStringLiteral("usecoexistrel.mjs")), "S");
        }

        // Registered modules are not visible through the global object and
        // not importable through importModule() by their registered name.
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            (void)qtEng.registerModule(QStringLiteral("globmod.mjs"), qtEng.evaluate(QStringLiteral("({ g: 1 })")));
            (void)qEng.registerModule(QStringLiteral("globmod.mjs"), qEng.evaluate(QStringLiteral("({ g: 1 })")));
            const bool qtHidden = qtEng.globalObject().property(QStringLiteral("globmod.mjs")).isUndefined();
            const bool qHidden = qEng.globalObject().property(QStringLiteral("globmod.mjs")).isUndefined();
            const bool qtByImport = qtEng.importModule(QStringLiteral("globmod.mjs")).isError();
            const bool qByImport = qEng.importModule(QStringLiteral("globmod.mjs")).isError();
            TEST_RESULT("RegModule: invisible to global object and importModule",
                        qtHidden && qHidden && qtByImport && qByImport,
                        QStringLiteral("hidden %1/%2, import error %3/%4")
                            .arg(qtHidden).arg(qHidden).arg(qtByImport).arg(qByImport));
        }

        // Relative specifiers never resolve to registered modules.
        {
            QJSEngine qtEng;
            QQuickJSEngine qEng;
            (void)qtEng.registerModule(QStringLiteral("relreg.mjs"), qtEng.evaluate(QStringLiteral("({ r: 8 })")));
            (void)qEng.registerModule(QStringLiteral("relreg.mjs"), qEng.evaluate(QStringLiteral("({ r: 8 })")));
            writeModule(QStringLiteral("userelreg.mjs"),
                "import { r } from \"./relreg.mjs\";\n"
                "export const R = r;\n");
            importParity("RegModule: relative specifier bypasses registry",
                         moduleDir.filePath(QStringLiteral("userelreg.mjs")), nullptr);
        }

        // ---- QuickJSEngine-specific module behavior ----
        {
            // Code-review regression: a dependency loaded during a failed
            // import chain (its importer failed at another missing import)
            // sits in the cache compiled but not instantiated; importing it
            // afterwards must evaluate and return its namespace.
            {
                writeModule(QStringLiteral("chainA.mjs"),
                    "import { x } from \"./chainB.mjs\";\n"
                    "import { q } from \"./gone.mjs\";\n"
                    "export const r = x;\n");
                writeModule(QStringLiteral("chainB.mjs"), "export const x = 7;\n");
                QQuickJSEngine qEng;
                QQuickJSValue chainAResult = qEng.importModule(moduleDir.filePath(QStringLiteral("chainA.mjs")));
                QQuickJSValue late = qEng.importModule(moduleDir.filePath(QStringLiteral("chainB.mjs")));
                TEST_RESULT("Module: dependency of failed chain importable",
                            chainAResult.isError() && !late.isError()
                                && late.property(QStringLiteral("x")).toInt() == 7,
                            QStringLiteral("chainA: %1, chainB: %2")
                                .arg(chainAResult.toString()).arg(late.toString()));
            }

            // Code-review regression: an expired execution-timeout deadline
            // must not interrupt the getter reads of a later registration.
            {
                QQuickJSEngine qEng;
                qEng.setExecutionTimeoutMs(1);
                (void)qEng.evaluate(QStringLiteral(
                    "var t = Date.now(); while (Date.now() - t < 30);"));
                const bool ok = qEng.registerModule(QStringLiteral("deadlinemod.mjs"),
                    qEng.evaluate(QStringLiteral("({ get v() { return 11; } })")));
                writeModule(QStringLiteral("usedeadline.mjs"),
                    "import { v } from \"deadlinemod.mjs\";\n"
                    "export const V = v;\n");
                QQuickJSValue ns = qEng.importModule(moduleDir.filePath(QStringLiteral("usedeadline.mjs")));
                TEST_RESULT("RegModule: expired deadline does not interrupt snapshot",
                            ok && !ns.isError() && ns.property(QStringLiteral("V")).toInt() == 11,
                            QStringLiteral("register=%1 V=%2")
                                .arg(ok).arg(ns.property(QStringLiteral("V")).toInt()));
            }

            // Error details: the message names the missing file, and the
            // engine state is clean afterwards (hasError() false).
            QQuickJSEngine qEng;
            QQuickJSValue r = qEng.importModule(moduleDir.filePath(QStringLiteral("gone.mjs")));
            TEST_RESULT("Module: missing file message names the file",
                        r.isError() && r.toString().contains(QLatin1String("gone.mjs"))
                            && !qEng.hasError(),
                        QStringLiteral("result: %1, hasError=%2").arg(r.toString()).arg(qEng.hasError()));

            // A sticky interrupt flag yields an "Interrupted" error object.
            QQuickJSEngine qEng2;
            qEng2.setInterrupted(true);
            QQuickJSValue r2 = qEng2.importModule(modPath);
            TEST_RESULT("Module: interrupted import yields Interrupted",
                        r2.isError() && r2.toString().contains(QLatin1String("Interrupted")),
                        QStringLiteral("result: %1").arg(r2.toString()));

            // Values from another engine are rejected with a warning.
            QQuickJSEngine qEng3;
            QQuickJSEngine otherEngine;
            {
                WarningCaptureGuard guard;
                const bool rejected = !qEng3.registerModule(QStringLiteral("foreign.mjs"),
                                                            otherEngine.newObject());
                TEST_RESULT("RegModule: cross-engine value rejected",
                            rejected && !g_capturedWarnings.isEmpty()
                                && g_capturedWarnings.first().contains(QLatin1String("another engine")),
                            QStringLiteral("rejected=%1 warnings=%2")
                                .arg(rejected).arg(g_capturedWarnings.size()));
            }
            // Uninitialized values are rejected the same way.
            {
                WarningCaptureGuard guard;
                const bool rejected = !qEng3.registerModule(QStringLiteral("invalid.mjs"), QQuickJSValue());
                TEST_RESULT("RegModule: invalid value rejected",
                            rejected && !g_capturedWarnings.isEmpty(),
                            QStringLiteral("rejected=%1 warnings=%2")
                                .arg(rejected).arg(g_capturedWarnings.size()));
            }

            // Circular imports follow the ECMAScript specification (V4 fails
            // to load circular modules at all - documented divergence).
            QQuickJSEngine qEng4;
            QQuickJSValue circA = qEng4.importModule(moduleDir.filePath(QStringLiteral("circA.mjs")));
            QQuickJSValue circB = qEng4.importModule(moduleDir.filePath(QStringLiteral("circB.mjs")));
            TEST_RESULT("Module: circular imports (spec semantics, beyond Qt)",
                        !circA.isError() && !circB.isError()
                            && circA.property(QStringLiteral("a")).toInt() == 1
                            && circA.property(QStringLiteral("bViaB")).toInt() == 11
                            && circB.property(QStringLiteral("readA")).call().toInt() == 1,
                        QStringLiteral("a=%1 bViaB=%2 readA()=%3")
                            .arg(circA.property(QStringLiteral("a")).toInt())
                            .arg(circA.property(QStringLiteral("bViaB")).toInt())
                            .arg(circB.property(QStringLiteral("readA")).call().toInt()));

            // import.meta: quickjs-ng implements the ES2020 object; live
            // QJSEngine 6.12 rejects any import.meta use with a SyntaxError
            // (documented divergence: our behavior follows the specification).
            QQuickJSValue meta = qEng4.importModule(moduleDir.filePath(QStringLiteral("meta.mjs")));
            TEST_RESULT("Module: import.meta object (spec semantics, beyond Qt)",
                        !meta.isError()
                            && !meta.property(QStringLiteral("hasUrl")).toBool()
                            && meta.property(QStringLiteral("keyCount")).toInt() == 0,
                        QStringLiteral("hasUrl=%1 keyCount=%2")
                            .arg(meta.property(QStringLiteral("hasUrl")).toBool())
                            .arg(meta.property(QStringLiteral("keyCount")).toInt()));

            // Top-level await (unsupported by QJSEngine 6.12; quickjs-ng
            // implements the ES2022 behavior) resolves through the microtask
            // queue that importModule() pumps by default.
            writeModule(QStringLiteral("tla.mjs"),
                "export const x = await Promise.resolve(42);\n");
            QQuickJSValue tla = qEng4.importModule(moduleDir.filePath(QStringLiteral("tla.mjs")));
            TEST_RESULT("Module: top-level await (spec semantics, beyond Qt)",
                        !tla.isError() && tla.property(QStringLiteral("x")).toInt() == 42,
                        QStringLiteral("result: %1").arg(tla.toString()));

            // A top-level-await rejection is handed to the C++ importer and
            // must not surface a second time through uncaughtException: the
            // buffered rejection of the module evaluation promise is
            // retracted by importModule().
            writeModule(QStringLiteral("tlarej.mjs"),
                "export const boom = await Promise.reject(new Error('TLA rejection boom'));\n");
            {
                int uncaught = 0;
                const auto conn = QObject::connect(&qEng4, &QQuickJSEngine::uncaughtException,
                                                   [&uncaught](const QString&, int) { ++uncaught; });
                const QQuickJSValue rej = qEng4.importModule(moduleDir.filePath(QStringLiteral("tlarej.mjs")));
                TEST_RESULT("Module: TLA rejection reaches C++ without a duplicate uncaughtException",
                            rej.isError() && rej.toString().contains(QLatin1String("TLA rejection boom")) && uncaught == 0,
                            QStringLiteral("isError=%1 text=%2 uncaught=%3")
                                .arg(rej.isError()).arg(rej.toString()).arg(uncaught));
                QObject::disconnect(conn);
            }

            // Dynamic import() works inside evaluate() scripts (a SyntaxError
            // on QJSEngine - documented beyond-parity extra); both file
            // modules and registered modules can be requested.
            (void)qEng4.registerModule(QStringLiteral("regdyn.mjs"),
                                       qEng4.evaluate(QStringLiteral("({ d: 'dyn' })")));
            const QString dynScript = QStringLiteral(
                "globalThis.res = 'pending';\n"
                "globalThis.res2 = 'pending';\n"
                "import('%1').then(function(m){ globalThis.res = m.x; });\n"
                "import('regdyn.mjs').then(function(m){ globalThis.res2 = m.d; });\n").arg(modPath);
            (void)qEng4.evaluate(dynScript);
            TEST_RESULT("Module: dynamic import() in scripts (beyond Qt)",
                        qEng4.globalObject().property(QStringLiteral("res")).toInt() == 42
                            && qEng4.globalObject().property(QStringLiteral("res2")).toString() == QLatin1String("dyn"),
                        QStringLiteral("res=%1 res2=%2")
                            .arg(qEng4.globalObject().property(QStringLiteral("res")).toString())
                            .arg(qEng4.globalObject().property(QStringLiteral("res2")).toString()));

            // Registered modules without a .mjs suffix are importable too
            // (QJSEngine rejects the import - documented divergence).
            QQuickJSEngine qEng5;
            (void)qEng5.registerModule(QStringLiteral("plainmod"), qEng5.evaluate(QStringLiteral("({ c: 3 })")));
            writeModule(QStringLiteral("useplain.mjs"),
                "import { c } from \"plainmod\";\n"
                "export const C = c;\n");
            QQuickJSValue plain = qEng5.importModule(moduleDir.filePath(QStringLiteral("useplain.mjs")));
            TEST_RESULT("RegModule: non-mjs name importable (beyond Qt)",
                        !plain.isError() && plain.property(QStringLiteral("C")).toInt() == 3,
                        QStringLiteral("result: %1").arg(plain.toString()));
        }

        // Relative fileName resolution against the current working directory.
        {
            const QString oldCwd = QDir::currentPath();
            QDir::setCurrent(modulePath);
            auto cwdGuard = qScopeGuard([oldCwd]() { QDir::setCurrent(oldCwd); });
            importParity("Module: relative fileName against CWD",
                         QStringLiteral("counter.mjs"), "n");
        }
    }

    // 22. Final Engine Stats
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

    // Module files for the importModule() qrc tests (see qjsmodules.qrc)
    Q_INIT_RESOURCE(qjsmodules);

    const bool runDangerous = !a.arguments().contains(QStringLiteral("--skip-dangerous"));

    // Stress mode: set at build time via the SERIAL_TEST_RUNS option
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
