// leakgate: quick regression detector for engine-memory leaks.
// Runs N cycles of a representative monitoring workload, takes a
// post-GC memory snapshot every k cycles, and reports the linear
// regression slope of JS memory vs cycle count.
//
// Verdict:
//   PASS  - slope within noise (<= 32 bytes/cycle, stable across segments)
//   LEAK  - monotone growth: slope > 32 bytes/cycle, consistent in all
//           segments; the fastest-growing counter hints at the cause
//           (atoms = unique names generated, objects = retained refs...).
//   ERROR - workload scripts failed to execute; the slope is meaningless
//           and the gate must not report green.
//
// Usage: leakgate [cycles] [logEvery] [--dump-leaks]
//   cycles       - total workload cycles (default 10000)
//   logEvery     - snapshot every k cycles (default 50)
//   --dump-leaks - engine leak detector (debug): JS_DUMP_ATOM_LEAKS |
//                  JS_DUMP_LEAKS | JS_ABORT_ON_LEAKS via JS_SetDumpFlags.
//                  The engine dumps leaked atoms/objects/strings in
//                  JS_FreeRuntime and aborts the process on leaks.
//
// On a direct run (no --dump-leaks) the process automatically re-runs
// itself with the same cycles/logEvery plus --dump-leaks and forwards the
// child output (auto dump-leaks stage, see runAutoDumpStage). An explicit
// --dump-leaks run never chains again. Exit code 0 requires every executed
// stage to be green. See printUsage() for the full command line reference.

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "QuickJSEngine.h"

// Exit codes (kept stable for scripts and CI):
//   0 - PASS (and the auto dump-leaks stage, when it ran, also passed)
//   1 - LEAK verdict, ERROR verdict (script errors) or a red dump-leaks stage
//   2 - usage error (unknown or invalid argument)

// Environment knobs:
//   LEAKGATE_AUTO_DUMP       - "0"/"off"/"false" disables the automatic
//                              dump-leaks stage; "1"/"on"/"true" enables it
//                              even under ctest; unset = automatic (enabled
//                              for direct runs, disabled under ctest).
//   LEAKGATE_DUMP_TIMEOUT_MS - overrides the computed dump child timeout.
//   LEAKGATE_DUMP_CHILD      - internal: set in the child environment as a
//                              second recursion guard.
static const char kEnvAutoDump[] = "LEAKGATE_AUTO_DUMP";
static const char kEnvDumpTimeoutMs[] = "LEAKGATE_DUMP_TIMEOUT_MS";
static const char kEnvDumpChild[] = "LEAKGATE_DUMP_CHILD";
// ctest exports this for every test process (verified with CTest 4.4.5,
// normal and -Q modes); presence detects harness runs.
static const char kEnvCtest[] = "CTEST_INTERACTIVE_DEBUG_MODE";

// ---------------------------------------------------------------------
// Workload object: the surface a monitoring script typically touches
// ---------------------------------------------------------------------

class MonitorObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(int counter READ counter WRITE setCounter NOTIFY counterChanged)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
public:
    explicit MonitorObject(QObject* parent = nullptr)
        : QObject(parent), m_child(nullptr)
    {
        // Child only for the root: guards the recursive constructor.
        if (!parent) m_child = new MonitorObject(this);
    }

    int counter() const { return m_counter; }
    void setCounter(int v) { if (m_counter != v) { m_counter = v; emit counterChanged(v); } }
    QString title() const { return m_title; }
    void setTitle(const QString& t) { m_title = t; emit titleChanged(); }

    Q_INVOKABLE int add(int a, int b) { return a + b; }
    Q_INVOKABLE QVariant process(const QVariantMap& data) {
        // Typical host transform: read fields, compute, return a map back.
        QVariantMap out;
        out.insert(QStringLiteral("sum"), data.value(QStringLiteral("a")).toInt()
                                              + data.value(QStringLiteral("b")).toInt());
        out.insert(QStringLiteral("label"), data.value(QStringLiteral("label")).toString()
                                                + QStringLiteral("_обработано"));
        return out;
    }
    Q_INVOKABLE MonitorObject* child() const { return m_child; }

signals:
    void counterChanged(int);
    void titleChanged();

private:
    int m_counter = 0;
    QString m_title = QStringLiteral("сенсор №%1");
    MonitorObject* m_child;
};

#include "main.moc"

// ---------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------

struct Snapshot {
    int cycle = 0;
    double jsMemKB = 0;      // memory_used_size: the engine's own accounting
    double mallocKB = 0;     // malloc_size: bytes allocated by the engine's allocator
    double atomsKB = 0, strsKB = 0, objsKB = 0, propsKB = 0, shapesKB = 0;
    int64_t atomCount = 0, objCount = 0, strCount = 0, propCount = 0;
    double rssKB = 0;        // process RSS: C++-side growth the JS snapshot cannot see
};

// Process RSS from /proc (the same monitor 02_speed samples around its
// benchmarks). The JS memory usage covers only the engine's heap: C++-side
// containers (console label tables, meta-object caches, engine-side hashes)
// grow invisible to it, so the report carries the process view too. 0 on
// platforms without /proc - the verdict never depends on it.
static double currentRSSKB()
{
#ifdef Q_OS_LINUX
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            const size_t kb = line.find_first_of("0123456789");
            if (kb != std::string::npos) return double(std::strtoull(line.c_str() + kb, nullptr, 10));
        }
    }
#endif
    return 0;
}

static double snapshotField(const JSMemoryUsage& u, int64_t JSMemoryUsage::*field)
{
    return double(u.*field) / 1024.0;
}

static Snapshot takeSnapshot(QQuickJSEngine& eng, int cycle)
{
    eng.collectGarbage();                 // measure retention, not the GC sawtooth
    JSMemoryUsage u = eng.memoryUsage();
    Snapshot s;
    s.cycle = cycle;
    s.jsMemKB  = snapshotField(u, &JSMemoryUsage::memory_used_size);
    s.mallocKB = snapshotField(u, &JSMemoryUsage::malloc_size);
    s.atomsKB  = snapshotField(u, &JSMemoryUsage::atom_size);
    s.strsKB   = snapshotField(u, &JSMemoryUsage::str_size);
    s.objsKB   = snapshotField(u, &JSMemoryUsage::obj_size);
    s.propsKB  = snapshotField(u, &JSMemoryUsage::prop_size);
    s.shapesKB = snapshotField(u, &JSMemoryUsage::shape_size);
    s.atomCount = u.atom_count;
    s.objCount = u.obj_count;
    s.strCount = u.str_count;
    s.propCount = u.prop_count;
    s.rssKB = currentRSSKB();
    return s;
}

// Least-squares slope of y over x.
static double slope(const std::vector<double>& x, const std::vector<double>& y)
{
    const size_t n = x.size();
    if (n < 2) return 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        sx += x[i]; sy += y[i];
        sxx += x[i] * x[i];
        sxy += x[i] * y[i];
    }
    const double denom = n * sxx - sx * sx;
    if (denom == 0) return 0;
    return (n * sxy - sx * sy) / denom;
}

static void reportSlope(const char* name, const std::vector<Snapshot>& snaps,
                        double Snapshot::*field, int segFrom, int segTo)
{
    std::vector<double> x, y;
    for (int i = segFrom; i < segTo; ++i) {
        x.push_back(snaps[i].cycle);
        y.push_back(snaps[i].*field);
    }
    qDebug().noquote() << QString("    %1: %2 KB/cycle")
                              .arg(name, -14).arg(slope(x, y), 0, 'f', 6);
}

// ---------------------------------------------------------------------
// Workload cycle: a finished unit of monitoring work. Nothing it creates
// must remain reachable after the cycle ends (except the persistent
// infrastructure object registered once).
// ---------------------------------------------------------------------
static int g_scriptErrors = 0;

// Module infrastructure for the import step: canonical path of the file
// module (empty if the temp dir failed); the registered module
// "leakmod" is created in main(). Both engine-side module caches
// (m_fileModules / m_registeredModules) are keyed by fixed distinct names
// and filled once - they are cycle infrastructure like the `mon` object.
static QString g_fileModulePath;

// Escape a path for embedding inside a single-quoted JS string literal.
static QString jsStringLiteral(const QString& s)
{
    QString out;
    out.reserve(s.size() + 2);
    for (const QChar c : s) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('\''))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

static void runCycle(QQuickJSEngine& eng, int cycle)
{
    auto ev = [&eng](const QString& s) {
        if (eng.evaluate(s).isError()) ++g_scriptErrors;
    };

    // 1) Event-driven script with a promise chain (typical async handler).
    ev(QStringLiteral(
           "mon.counter = %1;"
           "Promise.resolve(%1).then(v => v * 2).then(v => { mon.counter = v; });")
           .arg(cycle % 97));

    // 2) Signal connect + emit + disconnect (receiver churn like reloadable rules).
    ev(QStringLiteral(
           "var h = function(c) { mon.title = 'событие_' + c; };"
           "mon.counterChanged.connect(h);"
           "mon.counter = %1;"
           "mon.counterChanged.disconnect(h);" // NOTE: comment this line out to see what a leak looks like
           ).arg(cycle % 89));

    // 3) Bridge call with a QVariantMap round-trip (host data processing).
    ev(QStringLiteral(
           "(function(){ const p = mon.process;"
           "var r = p({a: %1, b: %2, label: 'замер_%3'});"
           "mon.title = r.label; })();")
           .arg(cycle).arg(cycle * 3).arg(cycle % 13));

    // 4) Property access + child identity (wrapper cache paths).
    ev(QStringLiteral(
           "(function(){ const c = mon.child;"
           "if (c.counter !== mon.counter - %1) { c.counter = mon.counter; } })();")
           .arg(cycle % 7));

    // 5) Unicode string work (wide strings, slicing).
    ev(QStringLiteral(
           "var s = 'показания_сенсора_№%1_温度_測定';"
           "s = s.substring(0, s.length - 2) + '_финал';"
           "mon.title = s;")
           .arg(cycle));

    // 6) A caught error (exception churn: throw/catch each cycle).
    ev(QStringLiteral(
        "try { null.x; } catch(e) { mon.title = String(e).slice(0, 30); }"));

    // 7) Dynamic property on a throwaway object (dynamic-prop cache paths).
    ev(QStringLiteral(
           "var tmp = {dyn_%1: %1, other: 'значение'};"
           "tmp['dyn_' + (%1 + 1)] = %1 * 2;")
           .arg(cycle % 31));

    // 8) JSON round-trip (parse + stringify).
    ev(QStringLiteral(
           "var j = JSON.parse('{\"idx\": %1, \"tags\": [\"ru\", \"data\"]}');"
           "mon.title = JSON.stringify(j);")
           .arg(cycle % 53));

    // 9) Dynamic module import (the loader path that was previously outside
    // the workload's coverage: specifier normalization, singleton cache
    // lookup, namespace retrieval, and the import promise + .then draining
    // with the job queue). Alternates between the file module (absolute
    // specifier -> m_fileModules cache) and the registered module (bare
    // specifier -> m_registeredModules); every per-cycle allocation on this
    // path must be collectible after the cycle ends.
    {
        const bool useFile = (cycle % 2 == 0) && !g_fileModulePath.isEmpty();
        const QString spec = useFile ? g_fileModulePath : QStringLiteral("leakmod");
        ev(QStringLiteral("import('%1').then(m => { mon.title = m.tag; });")
               .arg(jsStringLiteral(spec)));
    }
}

// ---------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------

struct Options {
    int cycles = 10000;
    int logEvery = 50;
    bool dumpLeaks = false;
    bool help = false;
};

static void printUsage()
{
    qDebug().noquote() << "Usage: leakgate [cycles] [logEvery] [--dump-leaks]";
    qDebug().noquote() << "";
    qDebug().noquote() << "Quick regression detector for engine-memory leaks: runs";
    qDebug().noquote() << "the monitoring workload, snapshots JS memory every";
    qDebug().noquote() << "logEvery cycles and reports the memory-vs-cycle slope.";
    qDebug().noquote() << "";
    qDebug().noquote() << "Parameters:";
    qDebug().noquote() << "  cycles          total workload cycles (default 10000, >= 1)";
    qDebug().noquote() << "  logEvery        snapshot every k cycles (default 50, >= 1;";
    qDebug().noquote() << "                  the slope needs at least 2 snapshots)";
    qDebug().noquote() << "  --dump-leaks    dump-leaks run only: the engine dumps leaked";
    qDebug().noquote() << "                  atoms/objects/strings in JS_FreeRuntime and";
    qDebug().noquote() << "                  aborts on leaks; never chains again";
    qDebug().noquote() << "  -h, --help      print this help";
    qDebug().noquote() << "";
    qDebug().noquote() << "A normal run automatically re-runs itself with --dump-leaks";
    qDebug().noquote() << "and the same cycles/logEvery (auto dump-leaks stage), unless";
    qDebug().noquote() << "launched by ctest or disabled via LEAKGATE_AUTO_DUMP.";
    qDebug().noquote() << "";
    qDebug().noquote() << "Environment:";
    qDebug().noquote() << "  LEAKGATE_AUTO_DUMP        0/off = never auto-chain, 1/on = chain";
    qDebug().noquote() << "                           even under ctest, unset = automatic";
    qDebug().noquote() << "  LEAKGATE_DUMP_TIMEOUT_MS  dump child timeout override in ms";
    qDebug().noquote() << "";
    qDebug().noquote() << "Exit codes: 0 = PASS, 1 = LEAK/ERROR or failed dump run,";
    qDebug().noquote() << "            2 = usage error";
    qDebug().noquote() << "";
    qDebug().noquote() << "Examples:";
    qDebug().noquote() << "  ./leakgate                        default run + auto dump run";
    qDebug().noquote() << "  ./leakgate 2000 50                custom run + auto dump run";
    qDebug().noquote() << "  ./leakgate 2000 50 --dump-leaks   dump-leaks run only";
}

// Strict parsing: any unknown option or invalid value is a usage error.
static bool parseArgs(int argc, char* argv[], Options* opt, QString* error)
{
    QList<QByteArray> positional;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--dump-leaks") {
            opt->dumpLeaks = true;
        } else if (arg == "-h" || arg == "--help") {
            opt->help = true;
            return true;
        } else if (arg.startsWith('-')) {
            *error = QString("unknown option '%1'").arg(QString::fromUtf8(arg));
            return false;
        } else {
            positional.append(arg);
        }
    }
    if (positional.size() > 2) {
        *error = QString("too many positional arguments (%1, expected at most 2)")
                     .arg(positional.size());
        return false;
    }
    bool ok = false;
    if (!positional.isEmpty()) {
        const int v = positional.at(0).toInt(&ok);
        if (!ok || v < 1) {
            *error = QString("invalid cycles '%1' (must be an integer >= 1)")
                         .arg(QString::fromUtf8(positional.at(0)));
            return false;
        }
        opt->cycles = v;
    }
    if (positional.size() > 1) {
        const int v = positional.at(1).toInt(&ok);
        if (!ok || v < 1) {
            *error = QString("invalid logEvery '%1' (must be an integer >= 1)")
                         .arg(QString::fromUtf8(positional.at(1)));
            return false;
        }
        opt->logEvery = v;
    }
    return true;
}

// ---------------------------------------------------------------------
// Auto dump-leaks stage
// ---------------------------------------------------------------------

// Runs the automatic dump-leaks stage: spawns this executable with the same
// cycles/logEvery plus --dump-leaks and forwards the child output. Returns
// true when the child finished normally with exit code 0. Infrastructure
// failures (unknown self path, spawn failure) only warn and keep the main
// verdict: they say nothing about leaks. A timeout or a red child exit
// fails the stage.
static bool runAutoDumpStage(const Options& opt, qint64 mainElapsedMs)
{
    const QString self = QCoreApplication::applicationFilePath();
    if (self.isEmpty() || !QFileInfo(self).isExecutable()) {
        qWarning().noquote() << "Auto dump-leaks run: cannot determine own executable"
                                " path, skipping (main verdict stands)";
        return true;
    }

    qDebug().noquote() << "";
    qDebug().noquote() << QStringLiteral("=== Auto dump-leaks run: %1 %2 %3 --dump-leaks ===")
                              .arg(self, QString::number(opt.cycles),
                                   QString::number(opt.logEvery));
    fflush(nullptr); // flush our streams before the child starts writing

    QProcess child;
    child.setProgram(self);
    child.setArguments({ QString::number(opt.cycles), QString::number(opt.logEvery),
                         QStringLiteral("--dump-leaks") });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QLatin1String(kEnvDumpChild), QStringLiteral("1")); // recursion guard
    child.setProcessEnvironment(env);
    child.setProcessChannelMode(QProcess::MergedChannels);

    child.start();
    if (!child.waitForStarted(5000)) {
        qWarning().noquote() << QStringLiteral("Auto dump-leaks run: failed to start (%1),"
                                               " skipping (main verdict stands)")
                                    .arg(child.errorString());
        return true;
    }

    // The child repeats the main workload plus the teardown dump: allow a
    // generous multiple of the main-run time with a floor for slow machines.
    qint64 timeoutMs = 60000 + 10 * mainElapsedMs;
    bool parsed = false;
    const qint64 overrideMs = QProcessEnvironment::systemEnvironment()
                                  .value(QLatin1String(kEnvDumpTimeoutMs)).toLongLong(&parsed);
    if (parsed && overrideMs > 0) timeoutMs = overrideMs;

    bool timedOut = false;
    QElapsedTimer stageClock;
    stageClock.start();
    while (child.state() == QProcess::Running) {
        const qint64 left = timeoutMs - stageClock.elapsed();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        // Cap each wait: the deadline is re-checked every lap, and the
        // per-call value stays in the int range even for huge timeouts.
        if (child.waitForReadyRead(left > 10000 ? 10000 : int(left))) {
            const QByteArray chunk = child.readAllStandardOutput();
            if (!chunk.isEmpty())
                fwrite(chunk.constData(), 1, size_t(chunk.size()), stderr);
        }
    }
    if (timedOut) {
        child.kill();
    }
    child.waitForFinished(5000); // reap the child and collect its exit status
    const QByteArray tail = child.readAllStandardOutput();
    if (!tail.isEmpty())
        fwrite(tail.constData(), 1, size_t(tail.size()), stderr);

    bool stageOk = false;
    if (timedOut) {
        qWarning().noquote() << QString("Auto dump-leaks run: timed out after %1 ms,"
                                        " killed - treating as failure").arg(timeoutMs);
    } else if (child.exitStatus() != QProcess::NormalExit) {
        qWarning().noquote() << "Auto dump-leaks run: child crashed - treating as failure";
    } else if (child.exitCode() != 0) {
        qWarning().noquote() << QString("Auto dump-leaks run: child exit code %1 -"
                                        " treating as failure").arg(child.exitCode());
    } else {
        stageOk = true;
    }
    qDebug().noquote() << "";
    qDebug().noquote() << QString("=== Auto dump-leaks run: %1 ===")
                              .arg(stageOk ? "PASS" : "FAILED");
    return stageOk;
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

int main(int argc, char* argv[])
{
    QCoreApplication a(argc, argv);

    Options opt;
    QString parseError;
    if (!parseArgs(argc, argv, &opt, &parseError)) {
        qWarning().noquote() << QString("leakgate: %1").arg(parseError);
        printUsage();
        return 2;
    }
    if (opt.help) {
        printUsage();
        return 0;
    }

    const int cycles = opt.cycles;
    const int logEvery = opt.logEvery;
    const bool dumpLeaks = opt.dumpLeaks;

    qDebug().noquote() << "leakgate: quickjs-ng memory-leak regression gate";
    qDebug().noquote() << QString("Build: %1 %2, quickjs-ng %3")
                              .arg(__DATE__).arg(__TIME__).arg(QQuickJSEngine::version());
    qDebug().noquote() << QString("Cycles: %1, snapshot every %2").arg(cycles).arg(logEvery);

    QQuickJSEngine eng;
    if (dumpLeaks) {
        eng.setDumpFlags(uint64_t(JS_DUMP_ATOM_LEAKS) | uint64_t(JS_DUMP_LEAKS)
                          | uint64_t(JS_ABORT_ON_LEAKS));
        qDebug().noquote() << "Dump-leaks mode: JS_DUMP_ATOM_LEAKS | JS_DUMP_LEAKS |"
                              "JS_ABORT_ON_LEAKS (engine dumps and aborts in JS_FreeRuntime on leaks)";
    }
    MonitorObject mon;
    eng.globalObject().setProperty("mon", eng.newQObject(&mon));

    // Module infrastructure for the import step (step 9): a file module in a
    // temp dir + a registered value module. Created once, before the
    // baseline - like `mon`, they are persistent cycle infrastructure, and
    // both engine-side module caches stay flat (fixed names, cache hits
    // after the first import).
    QTemporaryDir moduleDir;
    if (moduleDir.isValid()) {
        QFile moduleFile(moduleDir.filePath(QStringLiteral("cyclemod_file.js")));
        if (moduleFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            moduleFile.write("export const tag = 'filemod';\nexport const n = 16;\n");
            moduleFile.close();
            g_fileModulePath = QFileInfo(moduleFile).canonicalFilePath();
        }
    }
    if (g_fileModulePath.isEmpty()) {
        qWarning().noquote() << "leakgate: temp module file unavailable - the import step "
                                "will exercise the registered-module path only";
    }
    const QQuickJSValue regMod = eng.evaluate(
        QStringLiteral("({ tag: 'regmod', n: 8 })"));
    if (!eng.registerModule(QStringLiteral("leakmod"), regMod)) {
        qWarning().noquote() << "leakgate: registerModule('leakmod') failed";
        return 1;
    }

    // Verify the module infrastructure before the baseline: a silently
    // broken import step would shrink the workload's coverage without
    // failing the gate. Both flavors must resolve and expose .tag.
    {
        const QString fileSpec = jsStringLiteral(g_fileModulePath);
        const QString checkScript = g_fileModulePath.isEmpty()
                ? QStringLiteral("import('leakmod').then(m => { globalThis.modcheck = m.tag; });")
                : QStringLiteral(
                      "globalThis.modcheck = ''; "
                      "import('%1').then(m => { globalThis.modcheck += m.tag; }); "
                      "import('leakmod').then(m => { globalThis.modcheck += m.tag; });")
                      .arg(fileSpec);
        const QQuickJSValue started = eng.evaluate(checkScript);
        const QQuickJSValue tag = eng.evaluate(QStringLiteral("globalThis.modcheck"));
        const QString expected = g_fileModulePath.isEmpty()
                                     ? QStringLiteral("regmod")
                                     : QStringLiteral("filemodregmod");
        if (started.isError() || tag.toString() != expected) {
            qWarning().noquote() << QString("leakgate: module import verification failed "
                                            "(started=%1 modcheck='%2', expected '%3')")
                                        .arg(started.isError() ? QLatin1String("error")
                                                               : QLatin1String("ok"))
                                        .arg(tag.toString(), expected);
            return 1;
        }
        eng.evaluate(QStringLiteral("delete globalThis.modcheck;"));
    }

    // Warm-up: one full cycle + GC, then the baseline snapshot.
    runCycle(eng, 0);
    Snapshot base = takeSnapshot(eng, 0);
    qDebug().noquote() << QString("Baseline (post-GC): JS mem %1 KB, malloc %2 KB, atoms %3, objects %4")
                              .arg(base.jsMemKB, 0, 'f', 1).arg(base.mallocKB, 0, 'f', 1)
                              .arg(base.atomCount).arg(base.objCount);

    std::vector<Snapshot> snaps;
    QElapsedTimer total; total.start();

    for (int c = 1; c <= cycles; ++c) {
        runCycle(eng, c);
        if (c % logEvery == 0)
            snaps.push_back(takeSnapshot(eng, c));
    }
    // cycles < logEvery leaves snaps empty; keep at least one point so the
    // report below stays well-defined (the slope of a single point is 0).
    if (snaps.empty())
        snaps.push_back(takeSnapshot(eng, cycles));
    if (snaps.size() < 2)
        qWarning().noquote() << QString("Only %1 snapshot(s) taken: the slope needs at"
                                        " least 2, increase cycles or lower logEvery")
                                    .arg(int(snaps.size()));

    const qint64 ms = total.elapsed();
    Snapshot last = snaps.back();

    qDebug().noquote() << QString("Done: %1 cycles in %2 ms (%3 ms/cycle)")
                              .arg(cycles).arg(ms).arg(double(ms) / cycles, 0, 'f', 2);
    qDebug().noquote() << QString("Final (post-GC): JS mem %1 KB (base %2 KB, delta %3 KB), atoms %4 (base %5)")
                              .arg(last.jsMemKB, 0, 'f', 1).arg(base.jsMemKB, 0, 'f', 1)
                              .arg(last.jsMemKB - base.jsMemKB, 0, 'f', 1)
                              .arg(last.atomCount).arg(base.atomCount);

    // ---- Regression analysis on the JS memory line ----
    std::vector<double> x, y;
    for (const auto& s : snaps) { x.push_back(s.cycle); y.push_back(s.jsMemKB); }
    const double overall = slope(x, y);                  // KB / cycle
    const double bpc = overall * 1024.0;                 // bytes / cycle

    // Segment slopes: a leak that starts late shows a larger 3rd-segment slope.
    const int n3 = int(snaps.size()) / 3;
    std::vector<double> x1, y1, x2, y2, x3, y3;
    for (int i = 0; i < int(snaps.size()); ++i) {
        if (i < n3) { x1.push_back(snaps[i].cycle); y1.push_back(snaps[i].jsMemKB); }
        else if (i < 2 * n3) { x2.push_back(snaps[i].cycle); y2.push_back(snaps[i].jsMemKB); }
        else { x3.push_back(snaps[i].cycle); y3.push_back(snaps[i].jsMemKB); }
    }

    qDebug().noquote() << "";
    qDebug().noquote() << "JS memory slope by segment (KB/cycle):";
    qDebug().noquote() << QString("    seg1: %1, seg2: %2, seg3: %3")
                              .arg(slope(x1, y1), 0, 'f', 6).arg(slope(x2, y2), 0, 'f', 6)
                              .arg(slope(x3, y3), 0, 'f', 6);

    // Process-RSS slope: informational only. RSS carries allocator arenas and
    // page noise the JS snapshot does not, so a hard gate here would
    // false-positive; a sustained climb is still the visible symptom of
    // C++-side growth (see the comment on Snapshot::rssKB).
    if (last.rssKB > 0) {
        std::vector<double> xr, yr;
        for (const auto& s : snaps) { xr.push_back(s.cycle); yr.push_back(s.rssKB); }
        qDebug().noquote() << QString("Process RSS slope: %1 KB/cycle (base %2 KB, last %3 KB) -"
                                     " informational, not gated")
                                  .arg(slope(xr, yr), 0, 'f', 6)
                                  .arg(base.rssKB, 0, 'f', 1).arg(last.rssKB, 0, 'f', 1);
    }

    if (overall > 0.03125) {   // > 32 bytes/cycle
        // Growing-counter hints: which slice grows fastest?
        struct Named { const char* name; double Snapshot::*field; };
        const Named fields[] = {
                                 {"atoms", &Snapshot::atomsKB}, {"strings", &Snapshot::strsKB},
                                 {"objects", &Snapshot::objsKB}, {"properties", &Snapshot::propsKB},
                                 {"shapes", &Snapshot::shapesKB}, {"malloc", &Snapshot::mallocKB},
                                 };
        qDebug().noquote() << "";
        qDebug().noquote() << "Growth breakdown (overall):";
        for (const auto& f : fields)
            reportSlope(f.name, snaps, f.field, 0, int(snaps.size()));
    }

    if (g_scriptErrors > 0)
        qWarning().noquote() << QString(">>> WARNING: %1 script errors - workload incomplete, results unreliable! <<<")
                                    .arg(g_scriptErrors);

    // A workload with script errors cannot certify the absence of leaks:
    // the gate must not report green when its own workload failed to run.
    const bool slopePass = overall <= 0.03125;   // <= 32 bytes/cycle
    bool pass = slopePass && g_scriptErrors == 0;

    qDebug().noquote() << "";
    if (!slopePass)
        qDebug().noquote() << QString("=== VERDICT: LEAK (%1 bytes/cycle) ===")
                                  .arg(bpc, 0, 'f', 1);
    else if (g_scriptErrors > 0)
        qDebug().noquote() << QString("=== VERDICT: ERROR (%1 bytes/cycle, %2 script errors) ===")
                                  .arg(bpc, 0, 'f', 1).arg(g_scriptErrors);
    else
        qDebug().noquote() << QString("=== VERDICT: PASS (%1 bytes/cycle) ===")
                                  .arg(bpc, 0, 'f', 1);

    // Counter drift as a secondary signal (retained-reachable growth is not a
    // GC leak but still worth seeing: e.g. live objects piling up).
    qDebug().noquote() << QString("Counters: atoms %1, strings %2, objects %3, props %4")
                              .arg(last.atomCount - base.atomCount).arg(last.strCount - base.strCount)
                              .arg(last.objCount - base.objCount).arg(last.propCount - base.propCount);

    // ---- Automatic dump-leaks stage (direct runs only) ----
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString autoDump = env.value(QLatin1String(kEnvAutoDump));
    const bool forcedOff = autoDump == QLatin1String("0") || autoDump == QLatin1String("off")
                           || autoDump == QLatin1String("false");
    const bool forcedOn = autoDump == QLatin1String("1") || autoDump == QLatin1String("on")
                          || autoDump == QLatin1String("true");
    bool chain = false;
    QString skipReason;
    if (dumpLeaks) {
        // Explicit dump-leaks run: dump only, never chain.
    } else if (env.contains(QLatin1String(kEnvDumpChild))) {
        // Recursion guard: this process is the auto dump child (belt and
        // braces; the child's --dump-leaks flag already prevents chaining).
    } else if (forcedOff) {
        skipReason = QStringLiteral("disabled by LEAKGATE_AUTO_DUMP");
    } else if (!forcedOn && env.contains(QLatin1String(kEnvCtest))) {
        skipReason = QStringLiteral("launched by ctest");
    } else {
        chain = true;
    }
    if (chain) {
        if (!runAutoDumpStage(opt, ms))
            pass = false;
    } else if (!skipReason.isEmpty()) {
        qDebug().noquote() << "";
        qDebug().noquote() << QString("Auto dump-leaks run: skipped (%1)").arg(skipReason);
    }

    return pass ? 0 : 1;
}
