// QJSEngine (Qt V4) vs QQuickJSEngine (quickjs-ng) benchmark.
//
// Methodology (validated on x86 desktop / x86 office / ARM64 1GB / ARMv7 512MB):
//  - fresh engines per test: bounds peak RSS, isolates tests
//  - interleaved samples (A,B / B,A): thermal drift hits both engines equally
//  - one untimed warm-up per test: JIT compilation stays out of measurements
//  - time-calibrated tests: per-engine iteration count targeting ~400 ms samples
//  - bridge method calls are hoisted ("const m = obj.method" before the loop):
//    QJSEngine allocates a Heap::QObjectMethod (~97 B) on every method property
//    lookup, never collected by automatic GC (OOM on low-RAM boards); hoisting
//    removes it and speeds calls up ~1.9x. Same pattern for both engines.
//  - per-sample memory peaks: RSS sampling thread (5 ms) around each sample
//  - median of N runs; spread = (max-min)/median

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QDebug>
#include <QObject>
#include <QSysInfo>
#include <QVector>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QHash>
#include <QList>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QProcessEnvironment>
#include <QDateTime>
#include <functional>
#include <cmath>
#include <algorithm>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>

#ifndef SKIP_QT_BENCHMARK
#include <QJSEngine>
#include <QJSValue>
#endif

#ifdef __linux__
#include <malloc.h>
#include <fstream>
#include <string>
#endif

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#define BENCH_STRINGIFY2(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY2(x)
#if defined(__clang__)
#define BENCH_COMPILER "Clang " BENCH_STRINGIFY(__clang_major__) "." BENCH_STRINGIFY(__clang_minor__)
#elif defined(__GNUC__)
#define BENCH_COMPILER "GCC " BENCH_STRINGIFY(__GNUC__) "." BENCH_STRINGIFY(__GNUC_MINOR__)
#elif defined(_MSC_VER)
#define BENCH_COMPILER "MSVC " BENCH_STRINGIFY(_MSC_VER)
#else
#define BENCH_COMPILER "unknown compiler"
#endif

#include "QuickJSEngine.h"

// =================================================================================
// Configuration (CLI-overridable)
// =================================================================================

static int g_runs = 5;             // timed samples per test
static bool g_verbose = false;     // per-sample debug rows in the CSV
static bool g_runQt = true;
static bool g_runQuick = true;

static int g_scriptErrorsQt = 0;
static int g_scriptErrorsQuick = 0;

static const double kCalibTargetMs = 400.0;   // target sample duration for calibrated tests

// =================================================================================
// Memory helpers
// =================================================================================

static size_t currentRSSKB()
{
#ifdef __linux__
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            size_t pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) return std::stoul(line.substr(pos));
        }
    }
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / 1024;
    return 0;
#else
    return 0;
#endif
}

static void trimMemory()
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

static void quietPeriod(int ms = 150)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Samples process RSS every 5 ms while running; reports the maximum seen.
// Gives true per-sample peaks (RSS snapshots and process-lifetime HWM cannot).
class MemoryMonitor
{
public:
    MemoryMonitor() = default;
    ~MemoryMonitor() { stop(); }
    MemoryMonitor(const MemoryMonitor&) = delete;
    MemoryMonitor& operator=(const MemoryMonitor&) = delete;

    void start()
    {
        if (m_running.exchange(true))
            return;
        m_maxKB.store(0);
        m_stop.store(false);
        m_thread = std::thread([this] {
            while (!m_stop.load(std::memory_order_relaxed)) {
                const size_t kb = currentRSSKB();
                size_t prev = m_maxKB.load(std::memory_order_relaxed);
                while (kb > prev && !m_maxKB.compare_exchange_weak(prev, kb)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }

    size_t stop()
    {
        if (!m_running.exchange(false))
            return m_maxKB.load();
        m_stop.store(true);
        if (m_thread.joinable())
            m_thread.join();
        return m_maxKB.load();
    }

private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_maxKB{0};
};

// =================================================================================
// Test objects
// =================================================================================

class TestObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(QString rusName READ rusName WRITE setRusName NOTIFY rusNameChanged)
public:
    explicit TestObject(QObject* parent = nullptr) : QObject(parent), m_value(0), m_child(nullptr) {
        if (!parent) m_child = new TestObject(this);
    }
    int value() const { return m_value; }
    void setValue(int val) { if (m_value != val) { m_value = val; emit valueChanged(val); } }
    const QString& rusName() const { return m_rusName; }
    void setRusName(const QString& val) { m_rusName = val; emit rusNameChanged(); }

    Q_INVOKABLE int add(int a, int b) { return a + b; }
    Q_INVOKABLE QVariant echoVariant(QVariant v) { return v; }
    Q_INVOKABLE QObject* getChild() const { return m_child; }
    // Prebuilt lists: measure the C++ -> JS conversion only, no per-call
    // list construction (return-by-value is a cheap refcount bump).
    Q_INVOKABLE QVariantList getList32() const { return m_list32; }
    Q_INVOKABLE QStringList getStrList16() const { return m_strList16; }

signals:
    void valueChanged(int newValue);
    void rusNameChanged();

private:
    int m_value = 0;
    QString m_rusName = "Привет, мир! Это тестовая строка на русском языке.";
    TestObject* m_child;
    QVariantList m_list32 = QVariantList{
        0, 1, QStringLiteral("two"), 3.5, true, 5, QStringLiteral("six"), 7,
        8, 9, QStringLiteral("ten"), 11.5, false, 13, QStringLiteral("fourteen"), 15,
        16, 17, QStringLiteral("eighteen"), 19.5, true, 21, QStringLiteral("twenty-two"), 23,
        24, 25, QStringLiteral("twenty-six"), 27.5, false, 29, QStringLiteral("thirty"), 31 };
    QStringList m_strList16 = {
        QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma"), QStringLiteral("delta"),
        QStringLiteral("epsilon"), QStringLiteral("zeta"), QStringLiteral("eta"), QStringLiteral("theta"),
        QStringLiteral("iota"), QStringLiteral("kappa"), QStringLiteral("lambda"), QStringLiteral("mu"),
        QStringLiteral("nu"), QStringLiteral("xi"), QStringLiteral("omicron"), QStringLiteral("pi") };
};

class HeavyTestObject : public QObject {
    Q_OBJECT
public:
    explicit HeavyTestObject(QObject* parent = nullptr) : QObject(parent) {}

#define HEAVY_PROP(i) \
    Q_PROPERTY(int prop##i READ getProp##i WRITE setProp##i) \
        int m_prop##i = 0; \
        int getProp##i() const { return m_prop##i; } \
        void setProp##i(int v) { m_prop##i = v; }

    HEAVY_PROP(0) HEAVY_PROP(1) HEAVY_PROP(2) HEAVY_PROP(3) HEAVY_PROP(4)
        HEAVY_PROP(5) HEAVY_PROP(6) HEAVY_PROP(7) HEAVY_PROP(8) HEAVY_PROP(9)
        HEAVY_PROP(10) HEAVY_PROP(11) HEAVY_PROP(12) HEAVY_PROP(13) HEAVY_PROP(14)
        HEAVY_PROP(15) HEAVY_PROP(16) HEAVY_PROP(17) HEAVY_PROP(18) HEAVY_PROP(19)
        HEAVY_PROP(20) HEAVY_PROP(21) HEAVY_PROP(22) HEAVY_PROP(23) HEAVY_PROP(24)
        HEAVY_PROP(25) HEAVY_PROP(26) HEAVY_PROP(27) HEAVY_PROP(28) HEAVY_PROP(29)
        HEAVY_PROP(30) HEAVY_PROP(31) HEAVY_PROP(32) HEAVY_PROP(33) HEAVY_PROP(34)
        HEAVY_PROP(35) HEAVY_PROP(36) HEAVY_PROP(37) HEAVY_PROP(38) HEAVY_PROP(39)

#define HEAVY_METHOD(i) \
        Q_INVOKABLE int method##i(int v) { return v + i; }

    HEAVY_METHOD(0) HEAVY_METHOD(1) HEAVY_METHOD(2) HEAVY_METHOD(3) HEAVY_METHOD(4)
        HEAVY_METHOD(5) HEAVY_METHOD(6) HEAVY_METHOD(7) HEAVY_METHOD(8) HEAVY_METHOD(9)
        HEAVY_METHOD(10) HEAVY_METHOD(11) HEAVY_METHOD(12) HEAVY_METHOD(13) HEAVY_METHOD(14)
        HEAVY_METHOD(15) HEAVY_METHOD(16) HEAVY_METHOD(17) HEAVY_METHOD(18) HEAVY_METHOD(19)
        HEAVY_METHOD(20) HEAVY_METHOD(21) HEAVY_METHOD(22) HEAVY_METHOD(23) HEAVY_METHOD(24)
        HEAVY_METHOD(25) HEAVY_METHOD(26) HEAVY_METHOD(27) HEAVY_METHOD(28) HEAVY_METHOD(29)
        HEAVY_METHOD(30) HEAVY_METHOD(31) HEAVY_METHOD(32) HEAVY_METHOD(33) HEAVY_METHOD(34)
        HEAVY_METHOD(35) HEAVY_METHOD(36) HEAVY_METHOD(37) HEAVY_METHOD(38) HEAVY_METHOD(39)
};

// =================================================================================
// Results
// =================================================================================

struct BenchResult {
    QString name;
    int weight = 0;
    bool calibrated = false;
    int iterations = 0;
    std::vector<double> samples;        // ms
    std::vector<double> samplePeaksMB;  // process RSS max during each sample
    double medianMs = 0;
    double spreadPct = 0;
    double peakMB = 0;                  // max over samples
    double retentionMB = 0;             // RSS delta after engines destroyed (process level)
    bool valid = false;
    double nsPerOp() const { return (valid && iterations > 0) ? medianMs * 1e6 / iterations : 0; }
};

class ResultsCollector {
public:
    void add(const QString& name, std::vector<double> samples, std::vector<double> peaks,
             int iterations, int weight, bool calibrated)
    {
        BenchResult r;
        r.name = name;
        r.weight = weight;
        r.calibrated = calibrated;
        r.iterations = iterations;
        r.samples = std::move(samples);
        r.samplePeaksMB = std::move(peaks);
        if (r.samples.empty()) { m_results.append(r); return; }
        std::vector<double> s = r.samples;
        std::sort(s.begin(), s.end());
        const size_t n = s.size();
        r.medianMs = (n % 2 == 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0;
        const double mn = s.front(), mx = s.back();
        r.medianMs = (r.medianMs > 0) ? r.medianMs : 1e-9;
        r.spreadPct = (mx - mn) / r.medianMs * 100.0;
        for (double p : r.samplePeaksMB) r.peakMB = std::max(r.peakMB, p);
        r.valid = true;
        m_results.append(r);
    }
    const QVector<BenchResult>& results() const { return m_results; }
    BenchResult& last() { return m_results.last(); }
private:
    QVector<BenchResult> m_results;
};

static QString fmtPerOp(double ns)
{
    if (ns <= 0) return QStringLiteral("N/A");
    if (ns < 1000.0) return QString::number(ns, 'f', 1) + QStringLiteral(" ns");
    if (ns < 1e6) return QString::number(ns / 1e3, 'f', 2) + QStringLiteral(" us");
    if (ns < 1e9) return QString::number(ns / 1e6, 'f', 2) + QStringLiteral(" ms");
    return QString::number(ns / 1e9, 'f', 2) + QStringLiteral(" s");
}

static QString joinDoubles(const std::vector<double>& v)
{
    QString out;
    for (double x : v) out += (out.isEmpty() ? QString() : QStringLiteral("|")) + QString::number(x, 'f', 1);
    return out;
}

// =================================================================================
// Engine adapters
// =================================================================================

struct BenchEngine {
    virtual ~BenchEngine() = default;
    virtual bool evaluate(const QString& script) = 0;
    virtual void registerGlobal(const QString& name, QObject* obj) = 0;
    virtual void wrap(QObject* obj) = 0;
    virtual void prepareCall() = 0;
    virtual double callOnce() = 0;
    virtual void collectGarbage() = 0;
};

#ifndef SKIP_QT_BENCHMARK
class QtBenchEngine : public BenchEngine {
public:
    QJSEngine engine;
    bool evaluate(const QString& script) override {
        QJSValue r = engine.evaluate(script);
        if (r.isError()) {
            ++g_scriptErrorsQt;
            qDebug().noquote() << "  >>> Qt SCRIPT ERROR:" << r.toString();
            return false;
        }
        return true;
    }
    void registerGlobal(const QString& name, QObject* obj) override {
        engine.globalObject().setProperty(name, engine.newQObject(obj));
        QJSEngine::setObjectOwnership(obj, QJSEngine::CppOwnership);
    }
    void wrap(QObject* obj) override { engine.newQObject(obj); }
    void prepareCall() override {
        m_fn = engine.evaluate(QStringLiteral("(function(a, b) { return a + b; })"));
        m_args = QJSValueList() << 1 << 2;
    }
    double callOnce() override { return m_fn.call(m_args).toNumber(); }
    void collectGarbage() override { engine.collectGarbage(); }
private:
    QJSValue m_fn;
    QJSValueList m_args;
};
#endif

class QuickBenchEngine : public BenchEngine {
public:
    QQuickJSEngine engine;
    QuickBenchEngine() {
        engine.setMemoryLimit(0);
        engine.setMaxStackSize(0);
        engine.setExecutionTimeoutMs(0);
    }
    // Before member destruction, so the args release JSValues on a live engine.
    ~QuickBenchEngine() override { delete m_callArgs; }
    bool evaluate(const QString& script) override {
        QQuickJSValue r = engine.evaluate(script);
        if (r.isError()) {
            ++g_scriptErrorsQuick;
            qDebug().noquote() << "  >>> QuickJS SCRIPT ERROR:" << r.toString();
            return false;
        }
        return true;
    }
    void registerGlobal(const QString& name, QObject* obj) override {
        engine.globalObject().setProperty(name, engine.newQObject(obj));
    }
    void wrap(QObject* obj) override { engine.newQObject(obj); }
    void prepareCall() override {
        m_fn = engine.evaluate(QStringLiteral("(function(a, b) { return a + b; })"));
        delete m_callArgs;
        QQuickJSValue arg1(&engine, engine.toJSValue(QVariant(1)), true);
        QQuickJSValue arg2(&engine, engine.toJSValue(QVariant(2)), true);
        m_callArgs = new QList<QQuickJSValue>{arg1, arg2};
    }
    double callOnce() override { return m_fn.call(*m_callArgs).toDouble(); }
    void collectGarbage() override { engine.collectGarbage(); }
private:
    QQuickJSValue m_fn;
    QList<QQuickJSValue>* m_callArgs = nullptr;
};

// =================================================================================
// Bench cases
// =================================================================================

struct BenchCase {
    const char* name;
    int weight;
    int defaultN;
    bool calibrated;                                  // per-engine n targeting ~400 ms
    std::function<void(BenchEngine&, int n)> body;
    std::function<void(BenchEngine&)> setup;          // per-sample setup (may be null)
    std::function<void(BenchEngine&)> cleanup;        // per-sample cleanup (may be null)
};

// Per-engine iteration count for ~targetMs per sample; two probe runs double
// as warm-up. Floor of defaultN/10 keeps samples long enough for stable timing.
static int calibrateOps(BenchEngine& eng, const BenchCase& tc, double targetMs)
{
    const int probe = std::max(1, tc.defaultN / 100);
    tc.body(eng, probe);
    QElapsedTimer t;
    t.start();
    tc.body(eng, probe);
    const double ms = t.nsecsElapsed() / 1e6;
    if (ms <= 0.01) return tc.defaultN;
    const int scaled = static_cast<int>(std::min(probe * targetMs / ms, 20000000.0));
    const int floorN = std::max(probe, tc.defaultN / 10);
    return std::max(floorN, scaled);
}

// One timed sample with a memory monitor; returns process RSS peak in KB.
static size_t sampleOnce(BenchEngine* active, BenchEngine* idle,
                         const BenchCase& tc, int n, std::vector<double>& samples)
{
    if (!active) return 0;
    if (idle && tc.cleanup) tc.cleanup(*idle);
    if (tc.setup) tc.setup(*active);
    quietPeriod();
    MemoryMonitor monitor;
    monitor.start();
    QElapsedTimer t;
    t.start();
    tc.body(*active, n);
    samples.push_back(t.nsecsElapsed() / 1e6);
    return monitor.stop();
}

static void printEngineLine(const char* label, const BenchResult& r)
{
    if (!r.valid) {
        qDebug().noquote() << QString("  %1: N/A").arg(label, -8);
        return;
    }
    qDebug().noquote() << QString("  %1: %2/op  (%3 ms @ n=%4, spread %5%, peak %6 MB)  [%7]")
                              .arg(label, -8).arg(fmtPerOp(r.nsPerOp()), -10).arg(r.medianMs, 0, 'f', 2)
                              .arg(r.iterations).arg(r.spreadPct, 0, 'f', 1)
                              .arg(r.peakMB, 0, 'f', 1).arg(joinDoubles(r.samples));
}

static void runCase(const BenchCase& tc,
                    BenchEngine* a, ResultsCollector& colA, const char* labelA,
                    BenchEngine* b, ResultsCollector* colB, const char* labelB)
{
    int nA = tc.calibrated && a ? calibrateOps(*a, tc, kCalibTargetMs) : tc.defaultN;
    int nB = tc.calibrated && b ? calibrateOps(*b, tc, kCalibTargetMs) : tc.defaultN;

    // Untimed warm-up for non-calibrated tests (calibrated ones warm up in probes).
    if (!tc.calibrated) {
        if (a) {
            if (tc.setup) tc.setup(*a);
            tc.body(*a, nA);
            if (tc.cleanup) tc.cleanup(*a);
        }
        if (b) {
            if (tc.setup) tc.setup(*b);
            tc.body(*b, nB);
            if (tc.cleanup) tc.cleanup(*b);
        }
    }

    std::vector<double> sA, sB, pA, pB;
    for (int run = 0; run < g_runs; ++run) {
        if (run % 2 == 0) {
            pA.push_back(sampleOnce(a, b, tc, nA, sA) / 1024.0);
            pB.push_back(sampleOnce(b, a, tc, nB, sB) / 1024.0);
        } else {
            pB.push_back(sampleOnce(b, a, tc, nB, sB) / 1024.0);
            pA.push_back(sampleOnce(a, b, tc, nA, sA) / 1024.0);
        }
    }

    if (tc.cleanup) {
        if (a) tc.cleanup(*a);
        if (b) tc.cleanup(*b);
    }

    const QString name = QString::fromLatin1(tc.name);
    if (a) colA.add(name, std::move(sA), std::move(pA), nA, tc.weight, tc.calibrated);
    if (b && colB) colB->add(name, std::move(sB), std::move(pB), nB, tc.weight, tc.calibrated);

    qDebug().noquote() << "";
    qDebug().noquote() << tc.name << (tc.calibrated ? "[calibrated]" : "");
    if (a) printEngineLine(labelA, colA.last());
    if (b && colB) printEngineLine(labelB, colB->last());
}

static QVector<BenchCase> buildCases(TestObject& obj)
{
    QVector<BenchCase> cases;

    // Sloppy-mode implicit globals: QJSEngine may not define `globalThis`.
    // Every setup disconnects all known callbacks first - no accumulation between samples.
    const QString sigCleanup = QStringLiteral(
        "try { obj.valueChanged.disconnect(__sigA); } catch(e) {}"
        "try { obj.valueChanged.disconnect(__sigB); } catch(e) {}"
        "try { obj.valueChanged.disconnect(__sigC); } catch(e) {}"
        "try { obj.valueChanged.disconnect(__sigD); } catch(e) {}");
    auto sigCleanFn = [sigCleanup](BenchEngine& e) { e.evaluate(sigCleanup); };
    auto gcHook = [](BenchEngine& e) { e.collectGarbage(); };

    auto add = [&](const char* name, int weight, int defaultN, bool calibrated,
                   std::function<void(BenchEngine&, int)> body,
                   std::function<void(BenchEngine&)> setup,
                   std::function<void(BenchEngine&)> cleanup)
    {
        BenchCase tc;
        tc.name = name;
        tc.weight = weight;
        tc.defaultN = defaultN;
        tc.calibrated = calibrated;
        tc.body = std::move(body);
        tc.setup = std::move(setup);
        tc.cleanup = std::move(cleanup);
        cases.append(tc);
    };

    // ---- Pure JS ----
    add("Math loop", 15, 10000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let s=0; for(let i=0;i<%1;i++){ s+=i*2-1; } return s; } f();").arg(n));
        }, nullptr, nullptr);

    add("Function call overhead", 5, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ return 1; } function g(){ let s=0; for(let i=0;i<%1;i++){ s+=f(); } return s; } g();").arg(n));
        }, nullptr, nullptr);

    add("Fibonacci (fib 30)", 5, 1, false,
        [](BenchEngine& e, int) {
            e.evaluate(QStringLiteral("function fib(n){ if(n<=1) return n; return fib(n-1)+fib(n-2); } fib(10); fib(30);"));
        }, nullptr, nullptr);

    add("Object creation", 10, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let o; for(let i=0;i<%1;i++){ o = {a:i, b:i+1}; } return o; } f();").arg(n));
        }, gcHook, gcHook);

    add("Array access", 5, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let a=[]; for(let i=0;i<%1;i++) a.push(i); let s=0; for(let i=0;i<%2;i++) s+=a[i]; return s; } f();").arg(n).arg(n));
        }, gcHook, gcHook);

    // ---- Bridge (hoisted method calls) ----
    add("C++ -> JS call", 15, 1000000, false,
        [](BenchEngine& e, int n) {
            for (int i = 0; i < n; ++i) e.callOnce();
        },
        [](BenchEngine& e) { e.prepareCall(); },
        nullptr);

    add("JS -> C++ method", 15, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.add; let s=0; for(let i=0;i<%1;i++){ s+=m(i, i+1); } return s; } f();").arg(n));
        }, nullptr, nullptr);

    add("Wrapper cache (newQObject)", 5, 5000000, false,
        [&obj](BenchEngine& e, int n) {
            for (int i = 0; i < n; ++i) e.wrap(&obj);
        }, nullptr, nullptr);

    add("Identity (getChild)", 5, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.getChild; let r; for(let i=0;i<%1;i++){ r=m(); } return r; } f();").arg(n));
        }, nullptr, nullptr);

    // ---- Signals ----
    add("Signal emit, no listeners", 2, 5000000, false,
        [&obj](BenchEngine&, int n) {
            for (int i = 0; i < n; ++i) obj.setValue(i);
        }, sigCleanFn, sigCleanFn);

    add("Signal emit, 1 JS callback", 5, 1000000, false,
        [&obj](BenchEngine&, int n) {
            for (int i = 0; i < n; ++i) obj.setValue(i);
        },
        [sigCleanup](BenchEngine& e) {
            e.evaluate(sigCleanup + QStringLiteral("__sigA = function(v){}; obj.valueChanged.connect(__sigA);"));
        },
        sigCleanFn);

    add("Signal emit, 3 JS callbacks", 5, 1000000, false,
        [&obj](BenchEngine&, int n) {
            for (int i = 0; i < n; ++i) obj.setValue(i);
        },
        [sigCleanup](BenchEngine& e) {
            e.evaluate(sigCleanup + QStringLiteral(
                           "__sigB = function(v){}; __sigC = function(v){}; __sigD = function(v){};"
                           "obj.valueChanged.connect(__sigB);"
                           "obj.valueChanged.connect(__sigC);"
                           "obj.valueChanged.connect(__sigD);"));
        },
        sigCleanFn);

    // 90000 stays below the wrapper's 100k microtask drain limit per evaluate().
    add("Promise microtasks", 5, 90000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ for(let i=0;i<%1;i++){ Promise.resolve().then(v=>v); } } f();").arg(n));
        }, gcHook, gcHook);

    // ---- QVariant bridge (calibrated: heavy per-call allocation on both engines) ----
    add("QVariant: Map", 5, 400000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.echoVariant; let r; for(let i=0;i<%1;i++){ r=m({a:1, b:2}); } return r; } f();").arg(n));
        }, gcHook, gcHook);

    add("QVariant: Map long keys", 5, 400000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.echoVariant; let r; for(let i=0;i<%1;i++){ r=m({userName:1, displayName:2}); } return r; } f();").arg(n));
        }, gcHook, gcHook);

    add("QVariant: String", 10, 1000000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.echoVariant; let r; for(let i=0;i<%1;i++){ r=m('test string'); } return r; } f();").arg(n));
        }, gcHook, gcHook);

    // Round-trip via echoVariant: JS literal -> QVariantList -> JS.
    // Both directions per call (as in QVariant: Map).
    add("QVariant: List", 5, 400000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.echoVariant; let r; for(let i=0;i<%1;i++){ r=m([1,2,3,4,5,6,7,8]); } return r; } f();").arg(n));
        }, gcHook, gcHook);

    // Isolated C++ -> JS direction (listToJSArray / stringListToJSArray):
    // the invokable returns a prebuilt list, conversion is the call's only work.
    add("QVariant: List 32 C++ -> JS", 5, 400000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.getList32; let r; for(let i=0;i<%1;i++){ r=m(); } return r; } f();").arg(n));
        }, gcHook, gcHook);

    add("QVariant: StringList 16 C++ -> JS", 5, 400000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.getStrList16; let r; for(let i=0;i<%1;i++){ r=m(); } return r; } f();").arg(n));
        }, gcHook, gcHook);

    add("Method cache", 10, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = obj.add; let s=0; for(let i=0;i<%1;i++){ s+=m(0, 1); } return s; } f();").arg(n));
        }, nullptr, nullptr);

    add("Property cache", 10, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let s=0; for(let i=0;i<%1;i++){ s+=obj.value; } return s; } f();").arg(n));
        }, nullptr, nullptr);

    add("Negative cache", 5, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let s; for(let i=0;i<%1;i++){ s=obj.nonExistentProp; } return s; } f();").arg(n));
        }, nullptr, nullptr);

    // Qt recompiles on every evaluate() (no eval cache) - calibration bounds it.
    add("Eval cache (same scripts)", 5, 100000, true,
        [](BenchEngine& e, int n) {
            for (int i = 0; i < n; ++i) e.evaluate(QStringLiteral("var x = 1 + 1; obj.value;"));
        }, nullptr, nullptr);

    add("Wrapper churn (create/delete)", 5, 100000, false,
        [](BenchEngine& e, int n) {
            for (int i = 0; i < n; ++i) {
                TestObject* o = new TestObject();
                e.wrap(o);
                delete o;
            }
            e.collectGarbage();
        }, nullptr, nullptr);

    add("UTF property", 5, 100000, true,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let s=''; for(let i=0;i<%1;i++){ s=obj.rusName; } return s; } f();").arg(n));
        }, nullptr, nullptr);

    // ---- Heavy object ----
    add("Heavy object (property reads)", 10, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ let s=0; for(let i=0;i<%1;i++){ s+=heavyObj.prop39; } return s; } f();").arg(n));
        }, nullptr, nullptr);

    add("Heavy object (method calls)", 10, 1000000, false,
        [](BenchEngine& e, int n) {
            e.evaluate(QStringLiteral("function f(){ const m = heavyObj.method39; let s=0; for(let i=0;i<%1;i++){ s+=m(i); } return s; } f();").arg(n));
        }, nullptr, nullptr);

    return cases;
}

// =================================================================================
// Suite: fresh engines per test
// =================================================================================

static void runSuite(ResultsCollector& qtCol, ResultsCollector& quickCol)
{
    qDebug().noquote() << "========================================";
    qDebug().noquote() << "Benchmark: QJSEngine (Qt V4) vs QQuickJSEngine (quickjs-ng)";
    qDebug().noquote() << "Fresh engines per test; interleaved samples (A,B / B,A).";
    qDebug().noquote() << "========================================";

    TestObject obj;
    HeavyTestObject heavy;
    const QVector<BenchCase> cases = buildCases(obj);

    for (const BenchCase& tc : cases) {
        const double beforeMB = currentRSSKB() / 1024.0;

#ifndef SKIP_QT_BENCHMARK
        std::unique_ptr<QtBenchEngine> qtEng;
        if (g_runQt) {
            qtEng = std::make_unique<QtBenchEngine>();
            qtEng->registerGlobal(QStringLiteral("obj"), &obj);
            qtEng->registerGlobal(QStringLiteral("heavyObj"), &heavy);
        }
#endif
        std::unique_ptr<QuickBenchEngine> qjsEng;
        if (g_runQuick) {
            qjsEng = std::make_unique<QuickBenchEngine>();
            qjsEng->registerGlobal(QStringLiteral("obj"), &obj);
            qjsEng->registerGlobal(QStringLiteral("heavyObj"), &heavy);
        }

#ifndef SKIP_QT_BENCHMARK
        if (qtEng && qjsEng)
            runCase(tc, qtEng.get(), qtCol, "Qt V4", qjsEng.get(), &quickCol, "QuickJS");
        else if (qtEng)
            runCase(tc, qtEng.get(), qtCol, "Qt V4", nullptr, nullptr, nullptr);
        else if (qjsEng)
            runCase(tc, qjsEng.get(), quickCol, "QuickJS", nullptr, nullptr, nullptr);
#else
        if (qjsEng)
            runCase(tc, qjsEng.get(), quickCol, "QuickJS", nullptr, nullptr, nullptr);
#endif

        // Engines die here; measure process-level retention.
        trimMemory();
        quietPeriod(300);
        const double retentionMB = currentRSSKB() / 1024.0 - beforeMB;
#ifndef SKIP_QT_BENCHMARK
        if (g_runQt && !qtCol.results().isEmpty() && qtCol.last().name == QString::fromLatin1(tc.name))
            qtCol.last().retentionMB = retentionMB;
#endif
        if (g_runQuick && !quickCol.results().isEmpty() && quickCol.last().name == QString::fromLatin1(tc.name))
            quickCol.last().retentionMB = retentionMB;
        qDebug().noquote() << QString("    [mem] retention after test: %1 MB").arg(retentionMB, 0, 'f', 1);
    }
}

// =================================================================================
// Memory comparison phase (solo engine, monitored peaks)
// =================================================================================

struct EngineMemRecord {
    QString engine;
    double initMB = 0;
    double peakMB = 0;       // monitored during 1M-object allocation
    double afterGcMB = 0;
    double afterDestroyMB = 0;
};
static QVector<EngineMemRecord> g_memoryRecords;

template <typename EngineT>
static void runMemoryBenchmark(const char* engineLabel)
{
    qDebug().noquote() << "";
    qDebug().noquote() << "========================================";
    qDebug().noquote() << "Memory:" << engineLabel << "(solo, monitored peak)";
    qDebug().noquote() << "========================================";

    EngineMemRecord rec;
    rec.engine = QString::fromLatin1(engineLabel);

    trimMemory();
    quietPeriod(200);
    {
        EngineT eng;
        eng.evaluate(QStringLiteral("var a = 1+1;"));
        rec.initMB = currentRSSKB() / 1024.0;

        MemoryMonitor monitor;
        monitor.start();
        eng.evaluate(QStringLiteral("var arr = []; for(var i=0;i<1000000;i++){ arr.push({x:i, y:'str'}); }"));
        rec.peakMB = monitor.stop() / 1024.0;

        eng.evaluate(QStringLiteral("arr = null; for(var i=0;i<10000;i++){ var tmp = {a:i}; }"));
        eng.collectGarbage();
        rec.afterGcMB = currentRSSKB() / 1024.0;
    }
    trimMemory();
    quietPeriod(300);
    rec.afterDestroyMB = currentRSSKB() / 1024.0;

    g_memoryRecords.append(rec);
    qDebug().noquote() << QString("  init %1 MB | peak(1M objects) %2 MB | after GC %3 MB | after destroy %4 MB")
                              .arg(rec.initMB, 0, 'f', 2).arg(rec.peakMB, 0, 'f', 2)
                              .arg(rec.afterGcMB, 0, 'f', 2).arg(rec.afterDestroyMB, 0, 'f', 2);
}

// =================================================================================
// Environment info
// =================================================================================

struct EnvironmentInfo {
    QString system, cpu, os, qtVersion, buildType, compiler, enginesMode, timestamp, parentProcess;
};

static QString detectParentProcess()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* var : {"QT_CREATOR", "QT_QTESTING", "XPC_SERVICE_NAME"}) {
        if (!env.value(QString::fromLatin1(var)).isEmpty())
            return QString::fromLatin1(var);
    }
#ifdef _WIN32
    if (IsDebuggerPresent()) return QStringLiteral("debugger attached");
#elif defined(__linux__)
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("TracerPid:", 0) == 0) {
                size_t pos = line.find_first_of("0123456789");
                if (pos != std::string::npos && std::stoul(line.substr(pos)) != 0)
                    return QStringLiteral("debugger attached");
                break;
            }
    }
#endif
    return QStringLiteral("standalone");
}

static EnvironmentInfo collectEnvironmentInfo()
{
    EnvironmentInfo info;
    info.cpu = QSysInfo::currentCpuArchitecture();
#ifdef __linux__
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("model name", 0) == 0) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) { info.cpu = QString::fromStdString(line.substr(pos + 1)).trimmed(); break; }
            }
        }
    }
#elif defined(_WIN32)
    {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char buf[256] = {0};
            DWORD len = sizeof(buf);
            if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr, (LPBYTE)buf, &len) == ERROR_SUCCESS)
                info.cpu = QString::fromLocal8Bit(buf).trimmed();
            RegCloseKey(hKey);
        }
    }
#endif
    info.os = QSysInfo::prettyProductName();
    info.qtVersion = QString::fromLatin1(qVersion());
#ifdef QT_DEBUG
    info.buildType = QStringLiteral("Debug");
#else
    info.buildType = QStringLiteral("Release");
#endif
    info.compiler = BENCH_COMPILER;
    info.enginesMode = g_runQt && g_runQuick ? QStringLiteral("both")
                       : g_runQt ? QStringLiteral("qt") : QStringLiteral("quickjs");
    info.parentProcess = detectParentProcess();
    info.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    info.system = QString("%1, %2-bit, %3, Qt %4").arg(info.cpu).arg(QSysInfo::WordSize).arg(info.os).arg(info.qtVersion);
    return info;
}

// =================================================================================
// Final score table
// =================================================================================

static void printFinalScore(const EnvironmentInfo& env, ResultsCollector& qtCol, ResultsCollector& quickCol)
{
    qDebug().noquote() << ("\n" + QString(86, '='));
    qDebug().noquote() << env.system;
    qDebug().noquote() << QString(86, '=');

    auto padR = [](QString s, int w) { if (s.length() > w) s = s.left(w); while (s.length() < w) s += ' '; return s; };
    auto padL = [](QString s, int w) { if (s.length() > w) s = s.right(w); while (s.length() < w) s = ' ' + s; return s; };

    // In solo-qt mode quickCol is empty; print the table from qtCol, with the
    // engine in its own column. Otherwise rows come from quickCol.
    const bool soloQt =
#ifndef SKIP_QT_BENCHMARK
        g_runQt && !g_runQuick && quickCol.results().isEmpty();
#else
        false;
#endif

    const QVector<BenchResult>& rowSource = soloQt ? qtCol.results() : quickCol.results();

    qDebug().noquote() << padR("Test", 30) + " | " + padL("Qt V4", 10) + " | " + padL("QuickJS", 10) + " | "
                              + padL("Ratio", 7) + " | " + padL("Weight", 6) + " | " + padR("Winner", 8);
    qDebug().noquote() << QString(86, '-');

    double qtScore = 0, quickScore = 0, totalWeight = 0, logSumQt = 0, logSumQuick = 0;
    int cnt = 0;

#ifndef SKIP_QT_BENCHMARK
    QHash<QString, const BenchResult*> qtMap;
    if (g_runQt)
        for (const auto& r : qtCol.results()) qtMap.insert(r.name, &r);
#endif

    for (const BenchResult& qr : rowSource) {
        if (!qr.valid) continue;

        if (soloQt) {
            // Single-engine row: value in the Qt column.
            totalWeight += qr.weight;
            logSumQt += std::log10(1e9 / qr.nsPerOp());
            cnt++;
            qDebug().noquote() << padR(qr.name, 30) + " | " + padL(fmtPerOp(qr.nsPerOp()), 10) + " | "
                                      + padL("N/A", 10) + " | - | "
                                      + padL(QString::number(qr.weight), 6) + " | " + padR("-", 8);
            continue;
        }

#ifndef SKIP_QT_BENCHMARK
        if (!g_runQt) {
            totalWeight += qr.weight;
            logSumQuick += std::log10(1e9 / qr.nsPerOp());
            cnt++;
            qDebug().noquote() << padR(qr.name, 30) + " | " + padL("N/A", 10) + " | "
                                      + padL(fmtPerOp(qr.nsPerOp()), 10) + " | - | "
                                      + padL(QString::number(qr.weight), 6) + " | " + padR("-", 8);
            continue;
        }
        const BenchResult* tr = qtMap.value(qr.name, nullptr);
        if (!tr || !tr->valid) {
            qDebug().noquote() << padR(qr.name, 30) + " | " + padL("N/A", 10) + " | " + padL("N/A", 10) + " | - | "
                                      + padL(QString::number(qr.weight), 6) + " | " + padR("N/A", 8);
            continue;
        }
        const double qns = qr.nsPerOp(), tns = tr->nsPerOp();
        totalWeight += qr.weight;
        qtScore += qr.weight * (qns / (qns + tns));
        quickScore += qr.weight * (tns / (qns + tns));
        logSumQt += std::log10(1e9 / tns);
        logSumQuick += std::log10(1e9 / qns);
        cnt++;
        const QString winner = (tns < qns) ? QStringLiteral("Qt V4") : QStringLiteral("QuickJS");
        qDebug().noquote() << padR(qr.name, 30) + " | " + padL(fmtPerOp(tns), 10) + " | "
                                  + padL(fmtPerOp(qns), 10) + " | "
                                  + padL(QString::number(qns / tns, 'f', 2) + "x", 7) + " | "
                                  + padL(QString::number(qr.weight), 6) + " | " + padR(winner, 8);
#else
        totalWeight += qr.weight;
        logSumQuick += std::log10(1e9 / qr.nsPerOp());
        cnt++;
        qDebug().noquote() << padR(qr.name, 30) + " | " + padL("N/A", 10) + " | "
                                  + padL(fmtPerOp(qr.nsPerOp()), 10) + " | - | "
                                  + padL(QString::number(qr.weight), 6) + " | " + padR("-", 8);
#endif
    }

    qDebug().noquote() << QString(86, '-');
    qDebug().noquote() << QString("Relative score (out of %1):").arg(totalWeight, 0, 'f', 0);
#ifndef SKIP_QT_BENCHMARK
    if (g_runQt) qDebug().noquote() << QString("  QJSEngine (Qt V4) : %1").arg(qtScore, 0, 'f', 2);
#endif
    if (!soloQt) qDebug().noquote() << QString("  QQuickJSEngine    : %1").arg(quickScore, 0, 'f', 2);
    qDebug().noquote() << "  (relative score requires both engines; single-engine run)";
#ifndef SKIP_QT_BENCHMARK
    if (g_runQt && !soloQt && quickScore > 0) {
        if (quickScore > qtScore)
            qDebug().noquote() << QString("  Winner: QQuickJSEngine (%1% faster)").arg((quickScore / qtScore - 1.0) * 100.0, 0, 'f', 2);
        else
            qDebug().noquote() << QString("  Winner: QJSEngine (%1% faster)").arg((qtScore / quickScore - 1.0) * 100.0, 0, 'f', 2);
    }
#endif
    if (cnt > 0) {
        qDebug().noquote() << "";
        qDebug().noquote() << "Throughput index (geometric mean of ops/sec, higher = faster):";
#ifndef SKIP_QT_BENCHMARK
        if (g_runQt) qDebug().noquote() << QString("  QJSEngine (Qt V4) : %1").arg(pow(10.0, logSumQt / cnt), 0, 'f', 2);
#endif
        if (!soloQt) qDebug().noquote() << QString("  QQuickJSEngine    : %1").arg(pow(10.0, logSumQuick / cnt), 0, 'f', 2);
    }

    if (!g_memoryRecords.isEmpty()) {
        qDebug().noquote() << "";
        qDebug().noquote() << "Memory comparison (1M objects, monitored peaks):";
        for (const auto& r : g_memoryRecords)
            qDebug().noquote() << QString("  %1: init %2 | peak %3 | after GC %4 | after destroy %5 MB")
                                      .arg(r.engine, -26).arg(r.initMB, 0, 'f', 1).arg(r.peakMB, 0, 'f', 1)
                                      .arg(r.afterGcMB, 0, 'f', 1).arg(r.afterDestroyMB, 0, 'f', 1);
    }
}

// =================================================================================
// CSV export
// =================================================================================

static void saveCsv(const QString& path, const EnvironmentInfo& env,
                    ResultsCollector& qtCol, ResultsCollector& quickCol)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning().noquote() << "Cannot write CSV:" << path;
        return;
    }
    QTextStream s(&f);

    s << "# JavaScript engine benchmark results\n";
    s << "# build_stamp;" << QString::fromLatin1(__DATE__) << " " << QString::fromLatin1(__TIME__) << "\n";
    s << "# timestamp;" << env.timestamp << "\n";
    s << "# cpu;" << env.cpu << "\n";
    s << "# os;" << env.os << "\n";
    s << "# qt_version;" << env.qtVersion << "\n";
    s << "# quickjs_version;" << QQuickJSEngine::version() << "\n";
    s << "# build_type;" << env.buildType << "\n";
    s << "# compiler;" << env.compiler << "\n";
    s << "# launched_from;" << env.parentProcess << "\n";
    s << "# engines;" << env.enginesMode << "\n";
    s << "# runs_per_test;" << g_runs << "\n";
    s << "# hoisting;yes (bridge method calls hoisted: const m = obj.method)\n";
    s << "# script_errors;qt=" << g_scriptErrorsQt << ";quickjs=" << g_scriptErrorsQuick << "\n";
    s << "# note;ns_per_op is the comparable metric; calibrated tests pick per-engine n targeting ~400ms (floor defaultN/10); spread=(max-min)/median; per-test peaks are process RSS maxima sampled at 5ms (include the idle engine's resident memory in duel mode); retention is process-level RSS delta after engines destroyed\n";
    s << "# note;retention is process-level RSS delta after engines destroyed; "
         "in solo-qt mode QVariant tests show GC-surviving retention (~50-70 MB) - "
         "separate QJSEngine issue, see investigation doc\n";

    const bool haveTests = !quickCol.results().isEmpty()
#ifndef SKIP_QT_BENCHMARK
                           || (g_runQt && !qtCol.results().isEmpty())
#endif
        ;
    if (haveTests) {
#ifndef SKIP_QT_BENCHMARK
        QHash<QString, const BenchResult*> qtMap;
        if (g_runQt)
            for (const auto& r : qtCol.results()) qtMap.insert(r.name, &r);
#endif
        auto engineCols = [](const BenchResult* r) -> QString {
            if (!r || !r->valid) return QStringLiteral(";;;;;");
            return QString("%1;%2;%3;%4;%5;%6")
                .arg(r->iterations)
                .arg(QString::number(r->nsPerOp(), 'f', 1))
                .arg(QString::number(r->medianMs, 'f', 2))
                .arg(QString::number(r->spreadPct, 'f', 1))
                .arg(QString::number(r->peakMB, 'f', 1))
                .arg(joinDoubles(r->samples));
        };

        s << "\n[tests]\n";
        s << "test;weight;calibrated;"
             "qt_n;qt_ns_per_op;qt_median_ms;qt_spread_pct;qt_peak_mb;qt_samples;"
             "qjs_n;qjs_ns_per_op;qjs_median_ms;qjs_spread_pct;qjs_peak_mb;qjs_samples;"
             "ratio_qjs_over_qt;winner;retention_mb\n";
        for (const BenchResult& qr : quickCol.results()) {
            if (!qr.valid) continue;
            const BenchResult* tr = nullptr;
            double ratio = 0;
            QString winner = QStringLiteral("n/a");
#ifndef SKIP_QT_BENCHMARK
            if (g_runQt) {
                tr = qtMap.value(qr.name, nullptr);
                if (tr && tr->valid) {
                    ratio = qr.nsPerOp() / tr->nsPerOp();
                    winner = (tr->nsPerOp() < qr.nsPerOp()) ? QStringLiteral("qt") : QStringLiteral("quickjs");
                }
            }
#endif
            s << qr.name << ';' << qr.weight << ';' << (qr.calibrated ? 1 : 0) << ';'
              << engineCols(tr) << ';'
              << engineCols(&qr) << ';'
              << QString::number(ratio, 'f', 3) << ';' << winner << ';'
              << QString::number(qr.retentionMB, 'f', 1) << "\n";
        }
        // Emit Qt-only rows when quickjs is disabled.
        // engineCols(nullptr) renders the 6 empty qjs_* fields (see header).
        if (g_runQt && quickCol.results().isEmpty()) {
            for (const auto& r : qtCol.results()) {
                if (!r.valid) continue;
                s << r.name << ';' << r.weight << ';' << (r.calibrated ? 1 : 0) << ';'
                  << engineCols(&r) << ';'
                  << engineCols(nullptr) << ';'
                  << QStringLiteral("0.000;n/a;")
                  << QString::number(r.retentionMB, 'f', 1) << "\n";
            }
        }
    }

    if (!g_memoryRecords.isEmpty()) {
        s << "\n[memory]\n";
        s << "engine;init_mb;peak_1m_objects_mb;after_gc_mb;after_destroy_mb\n";
        for (const auto& r : g_memoryRecords)
            s << r.engine << ';' << QString::number(r.initMB, 'f', 2) << ';'
              << QString::number(r.peakMB, 'f', 2) << ';'
              << QString::number(r.afterGcMB, 'f', 2) << ';'
              << QString::number(r.afterDestroyMB, 'f', 2) << "\n";
    }

    if (g_verbose) {
        s << "\n[details]\n";
        s << "test;engine;n;samples_ms;sample_peaks_mb;peak_mb;retention_mb\n";
#ifndef SKIP_QT_BENCHMARK
        if (g_runQt)
            for (const auto& r : qtCol.results())
                s << r.name << ";qt;" << r.iterations << ';'
                  << joinDoubles(r.samples) << ';' << joinDoubles(r.samplePeaksMB) << ';'
                  << QString::number(r.peakMB, 'f', 1) << ';'
                  << QString::number(r.retentionMB, 'f', 1) << "\n";
#endif
        for (const auto& r : quickCol.results())
            s << r.name << ";quickjs;" << r.iterations << ';'
              << joinDoubles(r.samples) << ';' << joinDoubles(r.samplePeaksMB) << ';'
              << QString::number(r.peakMB, 'f', 1) << ';'
              << QString::number(r.retentionMB, 'f', 1) << "\n";
    }

    qDebug().noquote() << "Results saved to" << QFileInfo(path).absoluteFilePath();
}

// =================================================================================
// main
// =================================================================================

int main(int argc, char* argv[])
{
    QCoreApplication a(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "QJSEngine (Qt V4) vs QQuickJSEngine (quickjs-ng) benchmark.\n"
        "Fresh engines per test, interleaved samples, hoisted bridge calls,\n"
        "per-sample memory peaks via an RSS sampling monitor.");
    parser.addHelpOption();
    QCommandLineOption enginesOpt(QStringLiteral("engines"),
                                  "Engines to benchmark: both | qt | quickjs.", QStringLiteral("mode"), QStringLiteral("both"));
    QCommandLineOption runsOpt(QStringLiteral("runs"),
                               "Timed samples per test; the median is reported (1-21).",
                               QStringLiteral("n"), QStringLiteral("5"));
    QCommandLineOption verboseOpt(QStringLiteral("verbose"),
                                  "Write per-sample debug details (times, peaks) into the CSV.");
    QCommandLineOption noMemoryOpt(QStringLiteral("no-memory"),
                                   "Skip the memory comparison phase.");
    QCommandLineOption memoryOnlyOpt(QStringLiteral("memory-only"),
                                     "Run only the memory phase; writes benchmark_memory.csv.");
    parser.addOption(enginesOpt);
    parser.addOption(runsOpt);
    parser.addOption(verboseOpt);
    parser.addOption(noMemoryOpt);
    parser.addOption(memoryOnlyOpt);
    parser.process(a);

    const QString engines = parser.value(enginesOpt).toLower();
    if (engines == QStringLiteral("qt"))
        g_runQuick = false;
    else if (engines == QStringLiteral("quickjs") || engines == QStringLiteral("qjs"))
        g_runQt = false;
    else if (engines != QStringLiteral("both"))
        qWarning().noquote() << ">>> Unknown --engines value:" << engines
                              << "- expected both | qt | quickjs; running both. <<<";

    bool ok = false;
    const int r = parser.value(runsOpt).toInt(&ok);
    if (ok && r >= 1) g_runs = qBound(1, r, 21);
    g_verbose = parser.isSet(verboseOpt);
    const bool skipMemory = parser.isSet(noMemoryOpt);
    const bool memoryOnly = parser.isSet(memoryOnlyOpt);

#ifdef SKIP_QT_BENCHMARK
    if (!g_runQuick) {
        qWarning().noquote() << ">>> Built with SKIP_QT_BENCHMARK: Qt engine unavailable; running QuickJS only. <<<";
        g_runQuick = true;
    }
    g_runQt = false;
#endif

    const EnvironmentInfo env = collectEnvironmentInfo();

    qDebug().noquote() << "Build stamp:" << QString::fromLatin1(__DATE__) << QString::fromLatin1(__TIME__);
    qDebug().noquote() << "JavaScript engine comparison (performance)...";
    qDebug().noquote() << QString("Methodology: %1 run(s) per test, median reported; spread = (max-min)/median.").arg(g_runs);
    qDebug().noquote() << QString("Build: %1, %2, launched from: %3, engines: %4")
                              .arg(env.buildType, env.compiler, env.parentProcess, env.enginesMode);
#ifdef QT_DEBUG
    qWarning() << ">>> WARNING: DEBUG build! Use release for meaningful numbers. <<<";
#endif

    ResultsCollector qtCol, quickCol;

    if (!memoryOnly) {
        // Global warm-up: fault in pages, load libraries.
        if (g_runQuick) {
            QQuickJSEngine warm;
            warm.setMemoryLimit(0); warm.setMaxStackSize(0); warm.setExecutionTimeoutMs(0);
            warm.evaluate(QStringLiteral("for(let i=0;i<100000;i++){ Math.sqrt(i); }"));
        }
#ifndef SKIP_QT_BENCHMARK
        if (g_runQt) {
            QJSEngine warm;
            warm.evaluate(QStringLiteral("for(let i=0;i<100000;i++){ Math.sqrt(i); }"));
        }
#endif
        trimMemory();

        runSuite(qtCol, quickCol);
        printFinalScore(env, qtCol, quickCol);
    }

    if (!skipMemory) {
#ifndef SKIP_QT_BENCHMARK
        if (g_runQt) runMemoryBenchmark<QtBenchEngine>("QJSEngine (Qt V4)");
#endif
        if (g_runQuick) runMemoryBenchmark<QuickBenchEngine>("QQuickJSEngine (quickjs-ng)");
    }

    const QString csvName = memoryOnly ? QStringLiteral("benchmark_memory.csv")
                                       : QStringLiteral("benchmark_results.csv");
    saveCsv(csvName, env, qtCol, quickCol);

    if (g_scriptErrorsQt > 0 || g_scriptErrorsQuick > 0)
        qWarning().noquote() << QString(">>> WARNING: %1 Qt / %2 QuickJS script errors - affected rows are UNRELIABLE! <<<")
                                    .arg(g_scriptErrorsQt).arg(g_scriptErrorsQuick);

    qDebug().noquote() << "\nDone.";
    return 0;
}

#include "main.moc"