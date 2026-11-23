//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

#include "QuickJSEngine_p.h"
#include "QuickJSConvert.h"
#include "QuickJSQObjectProxy.h"
#include "QuickJSSignalProxy.h"

#include <QCoreApplication>
#include <QDate>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStringList>
#include <QThread>
#include <QTime>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_FREEBSD)
#include <pthread.h>
#include <pthread_np.h>   // pthread_attr_get_np()
#else
// POSIX everywhere else: Linux (glibc/musl), Darwin, other Unixes.
#include <pthread.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>   // _AddressOfReturnAddress()
#endif

namespace {

// Raw machine stack pointer of the calling frame. Unlike
// __builtin_frame_address(0), which sanitizers may rebase into an ASan
// fake-stack frame (detect_stack_use_after_return), a direct register read
// always stays in the real stack address space.
//
// Architecture dispatch uses compiler macros (__x86_64__/__aarch64__), not
// Qt's Q_PROCESSOR_*: the ARM64 spelling differs between toolchains
// (Q_PROCESSOR_ARM_64 in Qt), and a silently dead branch here would fall the
// build back onto the frame-address path - exactly the rebasing-prone source
// this function exists to avoid on the guard's critical path.
uintptr_t currentStackPointer()
{
#if defined(_MSC_VER)
    // Points at the caller's return-address slot, i.e. the TOP of this
    // frame: an overestimate of SP by the current frame's size (hundreds
    // of bytes, a few KiB worst-case in Debug). clampedStackBudget absorbs
    // that inside kSafetyMargin; MSVC has no inline-asm register read that
    // survives optimization this cheaply. clang-cl also lands here (_MSC_VER
    // first), keeping GNU asm out of MSVC-compatible builds.
    return reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
#elif defined(__x86_64__)
    uintptr_t sp;
    asm volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
#elif defined(__aarch64__)
    uintptr_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
#elif defined(__GNUC__) || defined(__clang__)
    // Exotic architecture without an asm path: the frame address is the
    // best available approximation of SP. Under ASan
    // detect_stack_use_after_return clang may rebase it into a fake stack
    // (detected by warnIfFakeStackActive); gcc keeps the real address.
    return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
#else
#error "QQuickJSEngine: unsupported toolchain for the stack guard"
#endif
}

// Raw measurement of the lowest address the calling thread's stack can grow
// to (its reserve bottom), or 0 when unknown. Anchoring the JS stack guard
// inside the real thread bounds keeps runaway recursion from exhausting the
// process stack: quickjs-ng derives its guard as "SP at runtime creation -
// stack_size", and a fixed size that lands below the real bottom (1 MB
// Windows stacks, sanitizer-heavy Debug builds, small worker-thread stacks)
// makes the guard unreachable - recursion then crashes the process instead
// of raising a catchable InternalError.
uintptr_t measureThreadStackFloor()
{
#if defined(Q_OS_WIN)
    // Windows 8+, kernel32: [low, high) of the current thread's stack.
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return uintptr_t(low);
#elif defined(Q_OS_DARWIN)
    // pthread_getattr_np() is a glibc extension and does not exist on
    // Darwin. The portable pair is pthread_get_stackaddr_np() (the ORIGIN,
    // the high end of the downward-growing mapping) and
    // pthread_get_stacksize_np(); the floor is origin - size. For the main
    // thread the size derives from the stack rlimit, so the result is the
    // hard reserve bottom rather than a committed-pages snapshot: the
    // floor cannot move, and the cache's VMA-growth re-measure branch
    // never fires here (harmless). If the direction assumption ever inverts
    // on some exotic Darwin, the garbage guard below catches it (a floor
    // at or above SP) instead of silently mis-budgeting.
    {
        const uintptr_t origin = uintptr_t(pthread_get_stackaddr_np(pthread_self()));
        const size_t size = pthread_get_stacksize_np(pthread_self());
        if (size == 0 || origin <= size)
            return 0;
        return origin - size;
    }
#elif defined(Q_OS_FREEBSD)
    // pthread_np.h: pthread_attr_get_np() + pthread_attr_getstack() report
    // the mapping base (the low end) directly, like glibc.
    {
        pthread_attr_t attr;
        if (pthread_attr_get_np(pthread_self(), &attr) != 0)
            return 0;
        void* base = nullptr;
        size_t size = 0;
        const int rc = pthread_attr_getstack(&attr, &base, &size);
        pthread_attr_destroy(&attr);
        if (rc != 0 || base == nullptr || size == 0)
            return 0;
        return uintptr_t(base);
    }
#elif defined(Q_OS_LINUX) && (defined(__GLIBC__) || defined(__BIONIC__))
    // glibc/bionic only: musl has no pthread_getattr_np() and falls
    // through to the unknown-floor policy (see threadStackFloor()).
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return 0;
    void* base = nullptr;
    size_t size = 0;
    const int rc = pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || base == nullptr || size == 0)
        return 0;
    return uintptr_t(base);
#else
    // Uncovered platform (musl and the like): unknown floor; the
    // conservative unknown-floor clamp in clampedStackBudget() is what
    // keeps the guard survivable there.
    return 0;
#endif
}

// Cached per-thread floor. The measurement is expensive on some hosts
// (Linux/glibc's pthread_getattr_np for the MAIN thread opens and parses
// /proc/self/maps - hundreds of microseconds), and the guard queries it at
// every top-level entry and every conversion node, so it must not be a
// per-call cost. The bounds are constant for the lifetime of ordinary
// threads (POSIX threads map the whole reserve up front; the Windows TEB
// reports fixed limits), so a thread_local cache is safe.
//
// The Linux main thread is the one exception: pthread_getattr_np reports
// the stack VMA grown SO FAR, and that VMA only extends downwards as
// recursion touches new pages. A cached floor therefore goes stale HIGH
// for the deepest entries; the cache re-measures whenever the caller's SP
// is below the cached floor (the VMA must have grown since), so the clamp
// never mistakes "stack grew below the cached floor" for "garbage floor".
// The first (highest) value is otherwise the safe side: clamping against a
// too-high floor only makes the guard fire a little earlier, never below
// the real bottom. The re-measure logic lives in threadStackFloor(),
// defined after the budget constants it reports in its diagnostics.

// Distance kept between the guard line and the real thread stack bottom. The
// JS guard is checked at JS frame entries only, so one whole C frame can slip
// below the line between two checks; fat Debug/ASan frames were measured at
// ~28 KiB on MinGW clang Debug. The throw path must also fit below the line
// when the guard fires. The vendored engine is built optimized and without
// sanitizers even in Debug (QUICKJS_SLIM_VENDOR), so that throw chain is a
// few release-sized frames; the budget below is one slipped fat frame
// (~36 KiB, only reachable on sanitizer-heavy hosts) + the throw chain
// (~72 KiB) with headroom. 128 KiB covers it while leaving starved stacks
// (1 MB Windows main threads under Debug+ASan where the C++ host machinery
// alone consumes most of the reserve) enough room for the engine to run at
// all; the previous 256 KiB margin consumed a quarter of such stacks and
// forced the effective budget down to a few KiB.
constexpr size_t kSafetyMargin = 128 * 1024;
// Smallest stack distance below the anchor the guard may watch. Below that
// the engine cannot meaningfully run, but the guard still must not be
// disabled: runaway JS recursion on a disabled guard exhausts the process
// stack (hard crash - the Windows Debug starved-stack failure class). An
// emergency budget keeps the recursion a catchable error instead; regular
// operations on such a starved stack fail with the same catchable error
// rather than killing the host process.
constexpr size_t kMinimumUsableBudget = 64 * 1024;
// Emergency guard budget for starved stacks (see kMinimumUsableBudget).
constexpr size_t kEmergencyGuardBudget = 16 * 1024;
// Stack reserve for the QVariant conversion recursion (toVariantImpl <->
// variantFromJSObject): the C++ conversion chain runs outside the JS stack
// guard, so it needs its own floor check. One conversion level costs a frame
// pair plus the property-enumeration machinery - tens of KiB on fat
// Debug/ASan builds, a few KiB in release. 96 KiB covers the deepest
// legitimate single level (pair + JS_GetOwnPropertyNames + unwinding) on
// any known build while still allowing 60+ nesting levels on healthy
// stacks. The reserve deliberately sits 32 KiB closer to the floor than
// the JS guard's kSafetyMargin (128 KiB): the JS window must additionally
// fit the interpreter's per-frame overhead, the interrupt-handler path and
// a full JS throw + unwind below the deepest JS frame, while a conversion
// level is plain C++ recursion whose only below-the-line obligation is its
// own throw path (the RangeError construction is pure C).
constexpr size_t kVariantStackReserve = 96 * 1024;
// Bounds of the calibrated machinery allowance added on top of the requested
// budget: at least 64 KiB (release builds still have ~5-8 KiB of entry chain,
// plus variance across entry paths), at most 1 MiB (pathological fat builds
// do not need more; larger values would swallow the JS recursion share on
// small stacks).
constexpr size_t kMinMachineryAllowance = 64 * 1024;
constexpr size_t kMaxMachineryAllowance = 1024 * 1024;
// Fallback allowance when calibration could not measure (probe failed):
// matches the largest entry chains observed on sanitizer-heavy Debug builds.
constexpr size_t kFallbackMachineryAllowance = 256 * 1024;
// Usable room assumed below the anchor when the thread floor is unknown
// (platform without a measurement API, or a garbage reading with no
// last-known-good fallback): clampedStackBudget() clamps the wanted budget
// to kUnknownFloorAssumedUsable - kSafetyMargin instead of releasing it
// unclamped. 256 KiB keeps the effective budget (128 KiB) above the
// emergency threshold - entries run, tightly clamped - while never handing
// out the full 512 KiB + allowance line that small thread stacks (1 MiB
// Windows workers, embedded targets) cannot honor.
constexpr size_t kUnknownFloorAssumedUsable = 256 * 1024;
static_assert(kUnknownFloorAssumedUsable > kSafetyMargin,
              "the unknown-floor clamp must stay below the assumed usable window");
static_assert(kUnknownFloorAssumedUsable - kSafetyMargin > kMinimumUsableBudget,
              "the unknown-floor budget must stay above the degenerate threshold");

// Diagnostics switch: QJS_GUARD_TRACE=1 prints every guard anchor decision
// and the context of every caught "Maximum call stack size exceeded".
bool guardTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("QJS_GUARD_TRACE") == 1;
    return enabled;
}

// One-shot warning latches, PER PROCESS (not per engine): embedding hosts
// and test suites construct hundreds of engines, and a systematic guard
// problem (starved stack, degenerate window) is one configuration issue
// of the host process - it must not spam the log once per engine.
std::atomic<bool> g_emergencyWarned{false};
std::atomic<bool> g_degenerateWarned{false};
std::atomic<bool> g_floorUnknownWarned{false};
bool warnOnce(std::atomic<bool>& latch)
{
    return latch.exchange(true);
}

// Cached per-thread floor with the failure policy described above
// measureThreadStackFloor(). Returns 0 when the floor is unknown: that
// never means "unlimited" - clampedStackBudget() clamps to a conservative
// fixed budget and the conversion guards fall back to a descent bound.
uintptr_t threadStackFloor(uintptr_t sp)
{
    constexpr uintptr_t kFloorUnmeasured = UINTPTR_MAX;
    static thread_local uintptr_t cached = kFloorUnmeasured;
    // Last measurement that was verified below the caller's SP at the time.
    // Stacks only grow downwards, so a previously verified floor only ever
    // UNDERSTATES the remaining window (the real floor can only have moved
    // further down since) - reusing it errs safe.
    static thread_local uintptr_t lastGood = 0;
    if (Q_UNLIKELY(cached == kFloorUnmeasured || (cached != 0 && sp < cached))) {
        const uintptr_t measured = measureThreadStackFloor();
        if (measured != 0 && measured < sp) {
            // Verified below the current SP: a real floor.
            cached = measured;
            lastGood = measured;
        } else if (lastGood != 0 && lastGood < sp) {
            // Garbage reading or API failure now: keep the last verified
            // floor instead of freezing the thread at unknown - one bad
            // reading must not degrade the guard for the thread's lifetime.
            cached = lastGood;
        } else {
            // Unknown, and no last-known-good fallback: the conservative
            // clamp takes over. Say so ONCE per process - a silently
            // degraded guard is a configuration problem of the host
            // process, like the other guard latches above.
            cached = 0;
            if (!g_floorUnknownWarned.exchange(true)) {
                qWarning("QQuickJSEngine: thread stack floor unavailable - stack "
                         "budgets clamped to the conservative unknown-floor value "
                         "(%lld KiB usable assumed); overflow protection is "
                         "degraded, not off",
                         qint64(kUnknownFloorAssumedUsable / 1024));
            }
        }
    }
    return cached;
}

// One-shot (per process) detection of an ASan fake stack. In one and the
// same frame, __builtin_frame_address(0) and the raw SP register can only
// differ by that frame's own size (a few hundred bytes plus sanitizer
// redzones); a drift of tens of KiB means the frame address was rebased
// into the fake stack that detect_stack_use_after_return allocates. Two
// consumers of frame addresses are affected by that: quickjs-ng's own
// stack guard and this engine's exotic-architecture fallback in
// currentStackPointer(). Verified against the vendored quickjs.c (the
// master snapshot and the v0.18.0 release agree byte-for-byte here):
// js_get_stack_pointer() has NO inline-asm path at all - it returns
// __builtin_frame_address(0) on EVERY clang/gcc build (x86_64 included;
// an earlier revision of this comment claimed a raw-%rsp asm path on
// x86_64, which was wrong - the asm reads belong to THIS engine's
// currentStackPointer()) and _AddressOfReturnAddress() on MSVC. The
// fake-stack risk therefore covers the vendor's guard on ALL clang/gcc
// builds, not only exotic architectures; this engine's asm budget math
// (x86_64/aarch64) reads the real stack and stays immune. The honest fix
// is on the runtime side (ASAN_OPTIONS detect_stack_use_after_return=0),
// not in the code.
void warnIfFakeStackActive()
{
#if defined(__GNUC__) || defined(__clang__)
    static std::atomic<bool> checked{false};
    if (checked.exchange(true))
        return;
    const uintptr_t frameAddress = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    const uintptr_t sp = currentStackPointer();
    const uintptr_t drift = frameAddress > sp ? frameAddress - sp : sp - frameAddress;
    if (drift > 64 * 1024) {
        qWarning("QQuickJSEngine: ASan fake stack detected (frame address %llx vs SP "
                 "%llx, %llu KiB apart) - frame-address reads are rebased: quickjs-ng's "
                 "vendor guard and this engine's non-asm fallback compare fake-stack "
                 "addresses against real-stack limits (never firing on real overflow "
                 "or firing instantly), while this engine's asm budget math on "
                 "x86_64/aarch64 reads the real stack. Run without "
                 "detect_stack_use_after_return (ASAN_OPTIONS) for a meaningful guard",
                 static_cast<unsigned long long>(frameAddress),
                 static_cast<unsigned long long>(sp),
                 static_cast<unsigned long long>(drift / 1024));
    }
#else
    // MSVC: no __builtin_frame_address to compare against, and the
    // fake-stack rebasing this detects is a clang/gcc ASan behavior.
#endif
}

// Clamps the wanted guard budget (requested JS recursion share + measured
// machinery allowance) so the quickjs-ng guard line (anchor SP - stack size)
// always stays above the real thread stack bottom (plus the safety margin)
// and never grows beyond what the thread can actually spare below the entry
// anchor. Unknown bounds clamp to the conservative kUnknownFloorAssumedUsable
// window, never to the unclamped request. The caller's SP is passed in so
// the anchor and the clamp math use ONE measurement: a between-frames drift
// (budget computed from a deeper frame than the one the anchor was recorded
// in) would silently skew the diagnostics and, on tight stacks, the clamp
// itself. "ONE" is about THIS frame's arithmetic: reanchorStackGuard()
// samples the SP for the anchor here, while JS_UpdateStackTop() re-samples
// it in its own, marginally deeper frame for the engine's stack_top - a few
// frames apart, strictly in the conservative direction (the engine's own
// checks fire marginally earlier, never later). Unifying the two samples
// would mean threading SP plumbing through every entry point; not worth it
// for a bias that can only err safe.
size_t clampedStackBudget(size_t wanted, uintptr_t sp)
{
    const uintptr_t floor = threadStackFloor(sp);
    if (floor == 0 || sp <= floor || sp - floor > (64ull << 20)) {
        // Unknown floor - an uncovered platform (musl without
        // pthread_getattr_np), a garbage reading with no last-known-good
        // fallback, or absurd bounds (a floor at/above the current SP, or
        // 64 MiB+ away: sanitizer-relocated threads and the like). NEVER
        // hand 'wanted' out unclamped: the wanted line is 512 KiB plus up
        // to 1 MiB of machinery allowance, and on small thread stacks it
        // lands below the real bottom - an unreachable guard that turns
        // runaway recursion back into the hard-crash class this guard
        // exists to prevent. Assume only the kUnknownFloorAssumedUsable
        // window instead; the safety margin still applies inside it (an
        // effective 128 KiB budget), which sits above the emergency
        // threshold: entries run, tightly clamped, and the one-shot
        // unknown-floor warning told the operator why.
        return wanted < kUnknownFloorAssumedUsable - kSafetyMargin
            ? wanted : kUnknownFloorAssumedUsable - kSafetyMargin;
    }
    const uintptr_t above = sp - floor;
    // The subtraction below must never run while the thread is inside the
    // safety margin: sp - floor - kSafetyMargin is computed in unsigned
    // arithmetic and would wrap around ~2^64, sailing past the minimum-
    // usable check and installing the FULL unclamped budget - a guard line
    // far below the real bottom, unreachable, turning runaway recursion
    // into a hard process crash. That is exactly the Windows Debug
    // starved-stack failure class this branch exists for, so the margin
    // comparison happens BEFORE the subtraction.
    if (above <= kSafetyMargin + kMinimumUsableBudget) {
        // The thread is already inside (or near) the margin. There is no
        // room for a full guard, but an emergency budget still keeps runaway
        // recursion catchable instead of exhausting the process stack (the
        // Windows Debug starved-stack failure class); the throw path of the
        // overflow fits below the line of that small budget.
        // Legitimate operations on such a starved stack throw the same
        // catchable error - the engine is unusable there either way, the
        // process must stay alive. Entries this deep are refused before
        // entering JS at all (see m_stackStarved), so this budget only has
        // to cover the engine-internal JS that cannot be refused.
        return kEmergencyGuardBudget;
    }
    const size_t usable = size_t(above - kSafetyMargin);
    return wanted < usable ? wanted : usable;
}

} // namespace

QQuickJSEngine::QQuickJSEngine(QObject* parent) : QObject(parent),
    m_rt(nullptr), m_ctx(nullptr), m_globalObj(JS_UNDEFINED),
    m_qobjectClassID(0), m_methodClassID(0), m_signalClassID(0), m_objectClassID(0),
    m_executionTimeoutMs(0), m_interrupted(false), m_autoProcessPromises(true)
{
    qRegisterMetaType<QQuickJSValue>("QQuickJSValue");

    m_rt = JS_NewRuntime();
    if (!m_rt) {
        throw std::runtime_error("QQuickJSEngine: Failed to allocate JSRuntime (out of memory)");
    }

    m_ctx = JS_NewContext(m_rt);
    if (!m_ctx) {
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to allocate JSContext (out of memory)");
    }

    JS_SetMemoryLimit(m_rt, 64 * 1024 * 1024);
    JS_SetGCThreshold(m_rt, 1 * 1024 * 1024);
    // Explicit defensive default: Atomics.wait() must never block the host
    // thread in a console application. SharedArrayBuffer is not exposed
    // either (no JS_SetSharedArrayBufferFunctions call); the flag guards
    // against future intrinsic changes.
    JS_SetCanBlock(m_rt, false);

    JS_SetContextOpaque(m_ctx, this);
    JS_SetRuntimeOpaque(m_rt, this);
    JS_SetInterruptHandler(m_rt, &QQuickJSEngine::interruptHandler, this);
    JS_SetModuleLoaderFunc(m_rt, &QQuickJSEngine::moduleNormalize,
                            &QQuickJSEngine::moduleLoader, this);

    JS_SetHostPromiseRejectionTracker(m_rt, [](JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void* opaque) {
        QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
        if (!engine) return;
        // quickjs-ng reports a rejection as unhandled at the reject moment and
        // retracts it (is_handled=true) when a handler attaches later in the
        // same tick. Buffering instead of reporting immediately avoids false
        // uncaughtException for rejections handled by a late .catch().
        if (is_handled) {
            engine->retractHandledRejection(ctx, promise);
        } else {
            engine->queueUnhandledRejection(ctx, promise, reason);
        }
    }, this);

    m_atoms.length = JS_NewAtom(m_ctx, "length");
    m_atoms.getTime = JS_NewAtom(m_ctx, "getTime");
    m_atoms.stack = JS_NewAtom(m_ctx, "stack");
    m_atoms.lineNumber = JS_NewAtom(m_ctx, "lineNumber");
    m_atoms.line = JS_NewAtom(m_ctx, "line");
    m_atoms.name = JS_NewAtom(m_ctx, "name");

    JS_NewClassID(m_rt, &m_qobjectClassID);
    JS_NewClassID(m_rt, &m_methodClassID);
    JS_NewClassID(m_rt, &m_signalClassID);

    m_qobjectExotic.set_property = &QuickJSQObjectProxy::qobject_set_property;
    m_qobjectExotic.get_own_property = &QuickJSQObjectProxy::qobject_get_own_property;
    m_qobjectExotic.get_own_property_names = &QuickJSQObjectProxy::qobject_get_own_property_names;
    m_qobjectExotic.delete_property = &QuickJSQObjectProxy::qobject_delete_property;

    JSClassDef qobjectDef = {
        .class_name = "QObject",
        .finalizer = [](JSRuntime* rt, JSValue val) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                auto data = static_cast<JSQObjectData*>(JS_GetOpaque(val, engine->m_qobjectClassID));
                if (data) {
                    if (!data->obj.isNull()) {
                        auto it = engine->m_qobjectWrapperCache.find(data->obj.data());
                        if (it != engine->m_qobjectWrapperCache.end()) {
                            engine->m_qobjectWrapperCache.erase(it);
                        }
                    }
                    data->clearCaches(rt);
                    delete data;
                }
            }
        },
        .gc_mark = [](JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                const auto* data = static_cast<const JSQObjectData*>(JS_GetOpaque(val, engine->m_qobjectClassID));
                if (data) {
                    for (const auto& entry : data->methodProxyCache) {
                        JS_MarkValue(rt, entry.value, mark_func);
                    }
                }
            }
        },
        .call = nullptr,
        .exotic = &m_qobjectExotic,
    };
    if (JS_NewClass(m_rt, m_qobjectClassID, &qobjectDef) < 0) {
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create QObject class");
    }

    // Single C call instead of the global -> Function -> prototype lookup
    // chain; returns a new reference.
    JSValue funcProto = JS_GetFunctionProto(m_ctx);

    if (!JS_IsObject(funcProto)) {
        clearPendingException(m_ctx);
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr; m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to get Function prototype");
    }

    JSClassDef methodDef = {
        .class_name = "MethodProxy",
        .finalizer = [](JSRuntime* rt, JSValue val) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(val, engine->m_methodClassID));
                if (data) delete data;
            }
        },
        .gc_mark = nullptr,
        .call = &QuickJSQObjectProxy::js_method_invoke,
        .exotic = nullptr,
    };
    if (JS_NewClass(m_rt, m_methodClassID, &methodDef) < 0) {
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create MethodProxy class");
    }

    JS_SetClassProto(m_ctx, m_methodClassID, JS_DupValue(m_ctx, funcProto));

    JSClassDef signalDef = {
        .class_name = "SignalProxy",
        .finalizer = [](JSRuntime* rt, JSValue val) {
            QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(JS_GetRuntimeOpaque(rt));
            if (engine) {
                auto data = static_cast<JSMethodProxyData*>(JS_GetOpaque(val, engine->m_signalClassID));
                if (data) delete data;
            }
        },
        .gc_mark = nullptr,
        .call = &QuickJSQObjectProxy::js_method_invoke,
        .exotic = nullptr,
    };
    if (JS_NewClass(m_rt, m_signalClassID, &signalDef) < 0) {
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr;
        m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create SignalProxy class");
    }

    JSValue signalProto = JS_NewObject(m_ctx);
    if (JS_IsException(signalProto)) {
        JS_FreeValue(m_ctx, funcProto);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        m_ctx = nullptr; m_rt = nullptr;
        throw std::runtime_error("QQuickJSEngine: Failed to create signal prototype object");
    }

    if (JS_SetPrototype(m_ctx, signalProto, funcProto) < 0) {
        qWarning("QQuickJSEngine: Failed to set SignalProxy prototype");
        clearPendingException(m_ctx);
    }
    if (JS_SetPropertyStr(m_ctx, signalProto, "connect", JS_NewCFunction(m_ctx, &QuickJSSignalProxy::js_signal_connect, "connect", 1)) < 0) {
        qWarning("QQuickJSEngine: Failed to install SignalProxy.connect");
        clearPendingException(m_ctx);
    }
    if (JS_SetPropertyStr(m_ctx, signalProto, "disconnect", JS_NewCFunction(m_ctx, &QuickJSSignalProxy::js_signal_disconnect, "disconnect", 1)) < 0) {
        qWarning("QQuickJSEngine: Failed to install SignalProxy.disconnect");
        clearPendingException(m_ctx);
    }
    JS_SetClassProto(m_ctx, m_signalClassID, JS_DupValue(m_ctx, signalProto));

    JS_FreeValue(m_ctx, signalProto);
    JS_FreeValue(m_ctx, funcProto);

    m_globalObj = JS_GetGlobalObject(m_ctx);

    {
        JSValue objCtor = JS_GetPropertyStr(m_ctx, m_globalObj, "Object");
        if (JS_IsObject(objCtor)) {
            JSValue objProto = JS_GetPropertyStr(m_ctx, objCtor, "prototype");
            if (JS_IsObject(objProto)) {
                m_objectClassID = JS_GetClassID(objProto);
                JS_SetClassProto(m_ctx, m_qobjectClassID, JS_DupValue(m_ctx, objProto));
            } else {
                clearPendingException(m_ctx);
            }
            JS_FreeValue(m_ctx, objProto);
        } else {
            clearPendingException(m_ctx);
        }
        JS_FreeValue(m_ctx, objCtor);
    }

    // Stack guard initialization comes LAST, on the fully constructed
    // engine: calibrateMachineryAllowance() evaluates real JS (its probe
    // script), and the probe must not meet a half-initialized engine - the
    // opaque pointers, the interrupt handler, the module loader, the class
    // table and the global object must all be live before any JS runs. The
    // earlier placement "worked" only because the probe was self-contained;
    // any future probe that touches engineFromContext(), a QObject proxy or
    // console would have dereferenced null. calibrateMachineryAllowance()
    // installs its own provisional guard for the probe, so no pre-anchor is
    // needed; the final reanchorStackGuard() then applies the calibrated
    // budget (512 KiB JS recursion share + machinery allowance, see
    // reanchorStackGuard()) anchored at the constructor-depth entry.
    calibrateMachineryAllowance();
    reanchorStackGuard();
}

QQuickJSEngine::~QQuickJSEngine() {
    for (JSGenericReceiver* receiver : m_signalReceivers) delete receiver;
    m_signalReceivers.clear();
    for (JSGenericReceiver* receiver : m_garbageReceivers) delete receiver;
    m_garbageReceivers.clear();

    // Buffered unhandled rejections are freed without reporting: emitting
    // signals from the destructor would be unsafe for subscribers.
    for (const PendingRejection& e : m_pendingRejections) {
        JS_FreeValues(m_ctx, e.promise, e.reason);
    }
    m_pendingRejections.clear();

    QHash<QObject*, JSValue> wrapperCache = std::exchange(m_qobjectWrapperCache, {});

    for (auto it = wrapperCache.begin(); it != wrapperCache.end(); ++it) {
        JSValue jsObj = it.value();
        auto data = static_cast<JSQObjectData*>(JS_GetOpaque(jsObj, m_qobjectClassID));
        if (data) {
            data->clearCaches(m_rt);
            delete data;
            JS_SetOpaque(jsObj, nullptr);
        }
        JS_FreeValue(m_ctx, jsObj);
    }

    for (auto it = m_metaObjectCaches.begin(); it != m_metaObjectCaches.end(); ++it) {
        delete it.value();
    }
    m_metaObjectCaches.clear();

    for (auto& entry : m_evalCache) {
        JS_FreeValue(m_ctx, entry.bytecode);
    }
    m_evalCache.clear();

    for (auto it = m_atomNameCache.begin(); it != m_atomNameCache.end(); ++it) {
        JS_FreeAtom(m_ctx, it.key());
    }
    m_atomNameCache.clear();

    for (auto it = m_stringAtomCache.begin(); it != m_stringAtomCache.end(); ++it) {
        JS_FreeAtom(m_ctx, it.value());
    }
    m_stringAtomCache.clear();

    JS_FreeAtoms(m_ctx, m_atoms.length, m_atoms.getTime, m_atoms.stack,
                 m_atoms.lineNumber, m_atoms.line, m_atoms.name);

    JS_FreeValue(m_ctx, m_globalObj);
    JS_FreeContext(m_ctx);
    JS_FreeRuntime(m_rt);
}

void QQuickJSEngine::setInterrupted(bool interrupted) { m_interrupted.store(interrupted); }
void QQuickJSEngine::setExecutionTimeoutMs(quint32 ms) {
    m_executionTimeoutMs.store(ms);
    // Self-contained deadline: enabling the timeout mid-execution grants a
    // fresh full quota immediately (from any thread); disabling clears it.
    m_executionDeadlineMs.store(deadlineFromTimeout(ms));
}
void QQuickJSEngine::setMemoryLimit(size_t limit) { if (m_rt) JS_SetMemoryLimit(m_rt, limit); }
void QQuickJSEngine::setMaxStackSize(size_t size) {
    // 0 disables quickjs-ng's guard (a runaway recursion then exhausts the
    // process stack). Non-zero values are the JS recursion share; the
    // effective guard budget adds the calibrated machinery allowance on top
    // and is re-clamped at every top-level entry; call from the engine's
    // thread, like the other setters of this class.
    m_stackBudgetBytes = size;
    reanchorStackGuard();
}

void QQuickJSEngine::reanchorStackGuard()
{
    if (!m_rt)
        return;
    // Never re-anchor from inside running JS: a C++ slot invoked by a
    // script (setMaxStackSize() from a slot, a nested evaluate() called the
    // old way) reports an SP far deeper than the JS frames already running
    // under the current guard line; anchoring there would place the new
    // stack_top below live frames and push the effective line below the
    // real thread bottom. The budget request stays recorded; the next
    // top-level entry (m_evalDepth == 0, via resetExecutionState())
    // re-measures from a clean anchor.
    if (m_evalDepth != 0)
        return;
    // Measure the budget from THIS entry point. quickjs-ng anchors the guard
    // at the stack position of JS_NewRuntime()/JS_SetMaxStackSize() and never
    // moves it afterwards; entering JS from deeper host frames (nested
    // evaluate() from a slot, signal callbacks, mid-suite engine use) then
    // leaves the guard above the actual JS frames. Re-anchoring also re-clamps
    // against the real thread bounds, so the guard line can never end up
    // below the bottom (unreachable guard => process crash instead of a
    // catchable error).
    //
    // The effective budget is the requested JS recursion share plus the
    // calibrated machinery allowance: on fat Debug/ASan builds the legitimate
    // C chain below the entry (compile + JS_CallInternal + builtin/job
    // trampolines) descends hundreds of KiB below the anchor, and a bare
    // recursion share makes the guard fire inside that very machinery
    // (false "Maximum call stack size exceeded" from every check-hitting
    // operation - promise jobs, apply(), typed-array construction, async
    // resume, module linking - while plain scripts without stack checks kept
    // running; Windows MinGW Debug+ASan once showed 22 failures, all
    // cascades of this one guard misfire).
    JS_UpdateStackTop(m_rt);
    warnIfFakeStackActive();
    m_stackStarved = false;
    if (m_stackBudgetBytes == 0) {
        JS_SetMaxStackSize(m_rt, 0);
        m_guardStackSize = 0;
        m_guardAnchorSp = 0;
        return;
    }
    const size_t allowance = m_allowanceCalibrated ? m_machineryAllowance
                                                   : kFallbackMachineryAllowance;
    const size_t wanted = m_stackBudgetBytes + allowance;
    // ONE SP measurement for the anchor, the clamp math and the diagnostics:
    // m_guardAnchorSp and the budget must describe the same frame.
    const uintptr_t sp = currentStackPointer();
    const size_t clamped = clampedStackBudget(wanted, sp);
    m_guardAnchorSp = sp;
    m_guardStackSize = clamped;
    m_stackStarved = (clamped <= kEmergencyGuardBudget);
    JS_SetMaxStackSize(m_rt, clamped);
    if (Q_UNLIKELY(m_stackStarved) && !warnOnce(g_emergencyWarned)) {
        qWarning("QQuickJSEngine: stack guard on emergency budget (%lld KiB) - the "
                 "engine's thread has less than %lld KiB of stack above its bottom "
                 "at this entry (SP=%llx); script entries are refused with a "
                 "catchable error before entering JS, the process stays alive",
                 qint64(clamped / 1024),
                 qint64((kSafetyMargin + kMinimumUsableBudget) / 1024),
                 static_cast<unsigned long long>(m_guardAnchorSp));
    }
    // Usable-window sanity (the degenerate-window failure class): the window below the
    // entry must exceed the calibrated machinery allowance by a real JS
    // recursion share. When the allowance (or nearly) consumes the window,
    // the guard line sits at the machinery's bottom: every check-hitting
    // operation then false-fires with "Maximum call stack size exceeded"
    // (promise jobs, combinators) and the overflow throw path runs past the
    // real bottom (hard crash). Windows MinGW Debug+ASan measured A~440 KiB
    // vs M~344 KiB on the 2 MiB default reserve: both symptoms at once.
    // Degrading silently hides it; say so once, with actionable numbers.
    const size_t jsShare = clamped > allowance ? clamped - allowance : 0;
    if (Q_UNLIKELY(!m_stackStarved)
            && Q_UNLIKELY(jsShare < kMinimumUsableBudget) && !warnOnce(g_degenerateWarned)) {
        qWarning("QQuickJSEngine: degenerate stack guard window - the calibrated entry "
                 "machinery (%lld KiB) plus the safety margin (%lld KiB) leaves only "
                 "%lld KiB of JS recursion share below the entry (usable below the "
                 "anchor: %lld KiB, requested share: %lld KiB). Legitimate operations "
                 "(promise jobs, combinators) will throw false 'Maximum call stack "
                 "size exceeded' and a real overflow can crash the process. Raise the "
                 "executable's stack reserve (e.g. link with --stack,16777216) or slim "
                 "the host entry",
                 qint64(allowance / 1024),
                 qint64(kSafetyMargin / 1024),
                 qint64(jsShare / 1024),
                 qint64(m_guardAnchorSp > threadStackFloor(sp)
                            ? size_t(m_guardAnchorSp - threadStackFloor(sp)) / 1024 : 0),
                 qint64(m_stackBudgetBytes / 1024));
    }
    if (guardTraceEnabled()) {
        qWarning("QQuickJSEngine: [guard] anchor SP=%llx floor=%llx aboveFloor=%llu KiB "
                 "requested=%zu KiB allowance=%zu KiB stackSize=%zu KiB%s",
                 static_cast<unsigned long long>(m_guardAnchorSp),
                 static_cast<unsigned long long>(threadStackFloor(sp)),
                 static_cast<unsigned long long>(m_guardAnchorSp > threadStackFloor(sp)
                     ? size_t(m_guardAnchorSp - threadStackFloor(sp)) / 1024 : 0),
                 m_stackBudgetBytes / 1024, allowance / 1024, m_guardStackSize / 1024,
                 m_stackStarved ? " (emergency budget, entries refused)" : "");
    }
}

namespace {
// Records the probe's stack position. Each engine is single-threaded
// (ensureEngineThread), but two engines can be constructed simultaneously
// on different host threads, hence thread_local.
static thread_local uintptr_t s_guardProbeSp = 0;

JSValue js_guardProbe(JSContext* ctx, JSValueConst, int, JSValueConst*)
{
    s_guardProbeSp = currentStackPointer();
    return JS_UNDEFINED;
}
} // namespace

void QQuickJSEngine::calibrateMachineryAllowance()
{
    if (!m_rt || !m_ctx || m_allowanceCalibrated)
        return;
    m_allowanceCalibrated = true;   // one shot, even on failure

    // Provisional guard for the probe: HALF of what the thread can safely
    // spare below this anchor, never the full amount. When the machinery
    // descent on fat Debug/ASan builds exceeds the budget, the guard fires
    // mid-probe and the "Maximum call stack size exceeded" throw path (plus
    // the unwinding) runs below the line: with a full-usable budget that
    // residual path had only the safety margin to the real stack bottom, and
    // on Windows MinGW Debug+ASan it crossed it - a hard stack-overflow
    // crash of the process right at engine construction (fixed by this
    // half-budget probe plus the slim vendor build, see QUICKJS_SLIM_VENDOR
    // in the CMakeLists). With half the budget the fire point sits at least
    // usable/2 above the margin, so both the throw path and the unwind fit
    // comfortably; the probe then simply reports chain=0 and the caller
    // falls back to the conservative allowance. On healthy stacks the
    // machinery needs far less than half the spare, the probe completes and
    // the real chain is measured as before.
    JS_UpdateStackTop(m_rt);
    {
        const uintptr_t sp = currentStackPointer();
        const uintptr_t floor = threadStackFloor(sp);
        size_t provisional = kFallbackMachineryAllowance + DefaultStackBudget;
        if (floor != 0 && sp > floor) {
            const size_t usable = sp > floor + kSafetyMargin
                ? size_t(sp - floor - kSafetyMargin) : 0;
            provisional = usable > kEmergencyGuardBudget ? usable / 2 : kEmergencyGuardBudget;
        }
        JS_SetMaxStackSize(m_rt, provisional);
    }
    const uintptr_t anchor = currentStackPointer();

    // Probe: ONE nested JS frame down into a C trampoline - the shape of the
    // machinery below a top-level entry: compile + eval + the first
    // JS_CallInternal + the C trampoline. The allowance this calibrates
    // must cover exactly that C chain UP TO the first JS frame: deeper
    // nesting is JS recursion, which the guard budget itself limits, and
    // counting extra JS frames in the probe double-books them (the guard
    // line moved down by N frames AND those frames granted again by the
    // budget on top; the earlier 7-frame probe x2 allowance once pushed the
    // line a full kilobyte-chain below the entry and, capped at 1 MiB, ate
    // the whole JS share on small stacks - the degenerate-window warning
    // class). Runs through plain JS_Eval - no evaluate() bookkeeping, no
    // eval-cache or global-lexicals pollution from the probe script.
    s_guardProbeSp = 0;
    static const char kProbeScript[] = "(function(){ __qjsGuardCalibrate(); })();";
    JSValue global = JS_GetGlobalObject(m_ctx);
    JSValue probeFn = JS_NewCFunction(m_ctx, js_guardProbe, "__qjsGuardCalibrate", 0);
    JS_SetPropertyStr(m_ctx, global, "__qjsGuardCalibrate", probeFn);   // consumes probeFn
    JSValue result = JS_Eval(m_ctx, kProbeScript, sizeof(kProbeScript) - 1,
                              "<qjs-guard-calibration>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(m_ctx, result);
    const JSAtom probeAtom = JS_NewAtom(m_ctx, "__qjsGuardCalibrate");
    JS_DeleteProperty(m_ctx, global, probeAtom, 0);
    JS_FreeAtom(m_ctx, probeAtom);
    JS_FreeValue(m_ctx, global);
    if (JS_HasException(m_ctx))
        clearPendingException(m_ctx);

    const size_t chain = (s_guardProbeSp != 0 && anchor > s_guardProbeSp)
        ? size_t(anchor - s_guardProbeSp) : 0;
    if (chain == 0) {
        // Probe failed (starved stack or eval error): conservative fallback.
        m_machineryAllowance = kFallbackMachineryAllowance;
    } else {
        // x1.5 headroom for entry paths deeper than the probe's single
        // frame: module linking (JS_EvalFunction on a nested module chain),
        // promise-job dispatch and async resume descend a couple of extra
        // C frames before their first JS frame, not a couple of extra JS
        // frames; 1.5x covers that variance without re-opening the
        // double-booking of the old x2-of-7-frames form.
        size_t allowance = chain + chain / 2;
        if (allowance < kMinMachineryAllowance) allowance = kMinMachineryAllowance;
        if (allowance > kMaxMachineryAllowance) allowance = kMaxMachineryAllowance;
        m_machineryAllowance = allowance;
    }
    if (guardTraceEnabled()) {
        qWarning("QQuickJSEngine: [guard] calibration chain=%zu KiB allowance=%zu KiB",
                 chain / 1024, m_machineryAllowance / 1024);
    }
}
void QQuickJSEngine::setAutoProcessPromises(bool enabled) { m_autoProcessPromises = enabled; }

QString QQuickJSEngine::version() {
    return QString::fromUtf8(JS_GetVersion());
}

JSMemoryUsage QQuickJSEngine::memoryUsage() const {
    ensureEngineThread();
    JSMemoryUsage usage{};
    if (m_rt) {
        JS_ComputeMemoryUsage(m_rt, &usage);
    }
    return usage;
}

void QQuickJSEngine::setGCThreshold(size_t threshold) {
    if (m_rt) {
        JS_SetGCThreshold(m_rt, threshold);
    }
}

void QQuickJSEngine::clearEvalCache() {
    ensureEngineThread();
    for (auto& entry : m_evalCache) {
        JS_FreeValue(m_ctx, entry.bytecode);
    }
    m_evalCache.clear();
}

void QQuickJSEngine::setEvalCacheEnabled(bool enabled) {
    ensureEngineThread();
    m_evalCacheEnabled = enabled;
    if (!enabled) clearEvalCache();
}

void QQuickJSEngine::setEvalCacheLimit(int limit) {
    ensureEngineThread();
    m_evalCacheLimit = limit > 0 ? limit : 0;
    if (!m_evalCacheEnabled) return;

    while (m_evalCache.size() > m_evalCacheLimit) {
        JS_FreeValue(m_ctx, m_evalCache.takeLast().bytecode);
    }
}

QQuickJSValue QQuickJSEngine::newErrorObject(QQuickJSValue::ErrorType errorType, QAnyStringView message) {
    ensureEngineThread();
    if (errorType == QQuickJSValue::NoError) {
        // QJSEngine parity: NoError yields an undefined value, not an Error.
        return QQuickJSValue();
    }

    // quickjs-ng provides C constructors with the built-in prototypes for
    // these five classes: engine-created errors stay immune to scripts
    // replacing globalThis.TypeError etc. (QJSEngine parity — V4 also uses
    // its internal prototypes). EvalError/URIError have no C helpers in
    // quickjs-ng 0.18 and keep the global-constructor fallback below.
    if (errorType != QQuickJSValue::EvalError && errorType != QQuickJSValue::URIError) {
        // "%s" stops at an embedded NUL; error messages are not expected
        // to contain one.
        const QByteArray utf8 = message.toString().toUtf8();
        JSValue err = JS_EXCEPTION;
        switch (errorType) {
        case QQuickJSValue::GenericError: err = JS_NewPlainError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::RangeError: err = JS_NewRangeError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::ReferenceError: err = JS_NewReferenceError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::SyntaxError: err = JS_NewSyntaxError(m_ctx, "%s", utf8.constData()); break;
        case QQuickJSValue::TypeError: err = JS_NewTypeError(m_ctx, "%s", utf8.constData()); break;
        default: break;   // unreachable: the remaining types are handled above
        }
        if (JS_IsException(err)) return QQuickJSValue::fromException(this);
        QQuickJSValue result(this, err, true);
        result.m_isError = true;
        return result;
    }

    // EvalError/URIError land here (no C helpers). The fallback resolves
    // the global constructor and CALLS it (JS_CallConstructor) - script-
    // visible execution, the only such branch in this function. On a
    // starved stack that is exactly what the entry refusal prevents
    // (script execution under the emergency budget, where even the throw
    // path may not fit); degrade to the plain C-constructed error instead
    // (GenericError class, engine-internal prototype, message preserved):
    // an error's class is metadata the host branches on, the message is the
    // payload, and the C path can neither recurse nor run script.
    if (Q_UNLIKELY(m_stackStarved)) {
        const QByteArray utf8 = message.toString().toUtf8();
        JSValue err = JS_NewPlainError(m_ctx, "%s", utf8.constData());
        if (JS_IsException(err)) return QQuickJSValue::fromException(this);
        QQuickJSValue result(this, err, true);
        result.m_isError = true;
        return result;
    }

    const char* ctorName = "Error";
    switch (errorType) {
    case QQuickJSValue::EvalError: ctorName = "EvalError"; break;
    case QQuickJSValue::RangeError: ctorName = "RangeError"; break;
    case QQuickJSValue::ReferenceError: ctorName = "ReferenceError"; break;
    case QQuickJSValue::SyntaxError: ctorName = "SyntaxError"; break;
    case QQuickJSValue::TypeError: ctorName = "TypeError"; break;
    case QQuickJSValue::URIError: ctorName = "URIError"; break;
    default: break;
    }

    ScopedJSValue global(m_ctx, JS_GetGlobalObject(m_ctx));
    ScopedJSValue ctor(m_ctx, JS_GetPropertyStr(m_ctx, global.value(), ctorName));
    if (consumeException(m_ctx, ctor.value())) return QQuickJSValue();

    ScopedJSValue msgVal(m_ctx, qStringToJS(m_ctx, message.toString()));
    if (JS_IsException(msgVal.value())) return QQuickJSValue::fromException(this);

    JSValueConst argv[1] = { msgVal.value() };
    ScopedJSValue err(m_ctx, JS_CallConstructor(m_ctx, ctor.value(), 1, argv));

    if (JS_IsException(err.value())) return QQuickJSValue::fromException(this);
    QQuickJSValue result(this, err.release(), true);
    result.m_isError = true;
    return result;
}

void QQuickJSEngine::throwError(const QString& message) {
    throwError(QQuickJSValue::GenericError, message);
}

void QQuickJSEngine::throwError(QQuickJSValue::ErrorType errorType, const QString& message) {
    ensureEngineThread();
    QQuickJSValue err = newErrorObject(errorType, message);
    if (!err.isValid()) return;   // construction failed and was already reported
    // JS_Throw() consumes the reference it is given; duplicate so that the
    // local QQuickJSValue keeps its own reference until the end of the scope.
    JS_Throw(m_ctx, JS_DupValue(m_ctx, err.jsVal()));
}

void QQuickJSEngine::throwError(const QQuickJSValue& error) {
    ensureEngineThread();
    if (!error.m_engine && JS_IsUndefined(error.m_val)) {
        // Default-constructed value: throw undefined, like QJSEngine does for
        // an uninitialized QJSValue.
        JS_Throw(m_ctx, JS_UNDEFINED);
        return;
    }
    if (error.m_engine.data() != this) {
        qWarning("QQuickJSEngine::throwError: value belongs to another engine or its engine is gone, ignored");
        return;
    }
    JS_Throw(m_ctx, JS_DupValue(m_ctx, error.m_val));
}

bool QQuickJSEngine::hasError() const {
    ensureEngineThread();
    return JS_HasException(m_ctx);
}

QQuickJSValue QQuickJSEngine::catchError() {
    ensureEngineThread();
    // fromException() checks the flag and returns an undefined (default
    // constructed) value when nothing is pending.
    return QQuickJSValue::fromException(this);
}

void QQuickJSEngine::collectGarbage() {
    ensureEngineThread();
    if (m_rt) JS_RunGC(m_rt);
}

void QQuickJSEngine::setDumpFlags(quint64 flags) {
    ensureEngineThread();
    if (m_rt) JS_SetDumpFlags(m_rt, static_cast<uint64_t>(flags));
}

int QQuickJSEngine::interruptHandler(JSContext * /*ctx*/, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    if (!engine) return 0;

    // QJSEngine parity: the flag is sticky - it stays set (aborting every
    // execution) until setInterrupted(false); it is never consumed here.
    // The execution timeout below remains a per-run quota.
    if (engine->m_interrupted.load()) {
        return 1;
    }

    const qint64 deadline = engine->m_executionDeadlineMs.load();
    if (deadline > 0 && steadyNowMs() > deadline) {
        return 1;
    }
    return 0;
}

QQuickJSValue QQuickJSEngine::globalObject() const {
    ensureEngineThread();
    return QQuickJSValue(const_cast<QQuickJSEngine*>(this), m_globalObj, false);
}

// ---------------------------------------------------------------------------
// Console/translation extension internals (QJSEngine parity; semantics
// verified against qtdeclarative 6.10 sources: qjsengine.cpp /
// qqmlbuiltinfunctions.cpp, and against live Qt 6.12 behavior)
// ---------------------------------------------------------------------------

namespace {

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
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[2]) == 0) n = static_cast<int>(d);
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
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[i]) == 0) n = static_cast<int>(d);
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
            double d = 0;
            if (JS_ToFloat64(ctx, &d, argv[1]) == 0) n = static_cast<int>(d);
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

static void parseStackLocation(const QString& loc, QString* file, int* lineNo, int* colNo) {
    *file = loc;
    const qsizetype c = loc.lastIndexOf(u':');
    if (c <= 0) return;
    bool okCol = false;
    const int col = QStringView{loc}.mid(c + 1).toString().toInt(&okCol);
    if (!okCol) return;
    const qsizetype l = loc.lastIndexOf(u':', c - 1);
    if (l < 0) return;
    bool okLine = false;
    const int lineNo2 = QStringView{loc}.mid(l + 1, c - l - 1).toString().toInt(&okLine);
    if (!okLine) return;
    *file = loc.left(l);
    *lineNo = lineNo2;
    *colNo = col;
}

// Translates the "stack" property of a caught exception into QJSEngine's
// exceptionStackTrace frame format "function:line:column:file" (native frames
// use -1/-1 with an empty file, like V4). quickjs-ng frames look like
// "    at name (file:line:col)", "    at name (native)" or "    at file:line:col";
// a bare token without parentheses or location is an eval filename frame and
// yields an empty function name.
void QQuickJSEngine::collectExceptionStackTrace(QQuickJSEngine* engine, JSValueConst errVal, QStringList* out) {
    if (!out) return;
    out->clear();
    JSContext* ctx = engine->m_ctx;
    ScopedJSValue stack(ctx, JS_GetProperty(ctx, errVal, engine->m_atoms.stack));
    if (JS_IsException(stack.value())) {
        clearPendingException(ctx);
        return;
    }
    const QString s = QuickJSConvert::jsToQString(ctx, stack.value());
    if (s.isNull()) return;

    for (QString rawLine : s.split(u'\n')) {
        QStringView t = QStringView{rawLine}.trimmed();
        if (!t.startsWith(u"at ")) continue;
        t = t.mid(3);

        QString func;
        QString file;
        int lineNo = -1;
        int colNo = -1;

        const qsizetype open = t.indexOf(u'(');
        if (open >= 0 && t.endsWith(u')')) {
            func = t.left(open).trimmed().toString();
            const QString loc = t.mid(open + 1, t.size() - open - 2).toString();
            if (loc != QLatin1String("native") && loc != QLatin1String("missing")) {
                parseStackLocation(loc, &file, &lineNo, &colNo);
            }
        } else {
            // The eval filename frame: "file", "file:line" or "file:line:col"
            parseStackLocation(t.toString(), &file, &lineNo, &colNo);
        }

        out->append(QStringLiteral("%1:%2:%3:%4")
                        .arg(func, QString::number(lineNo), QString::number(colNo), file));
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
                    const QString label = argc > 0 ? consoleArgToString(ctx, argv[0]) : QString();
                    const int value = ++engine->m_consoleCounters[label];
                    qDebug().noquote() << "[JS console]" << QStringLiteral("%1: %2").arg(label).arg(value);
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
                        engine->m_consoleTimers.insert(consoleArgToString(ctx, argv[0]), timer);
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
                            // V4 format: qDebug("%s: %llims", name, elapsed).
                            qDebug().noquote() << "[JS console]" << QStringLiteral("%1: %2ms").arg(label).arg(elapsed);
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
            } else if (JS_IsFunction(m_ctx, logFn.value())) {
                if (JS_SetPropertyStr(m_ctx, target, "print", JS_DupValue(m_ctx, logFn.value())) < 0) {
                    clearPendingException(m_ctx);
                }
            } else {
                clearPendingException(m_ctx);
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
        }
    }

    if (toInstall.testFlag(TranslationExtension)) {
        auto addTargetFn = [this, target](const char* name, JSCFunction* fn, int length) {
            JSValue func = JS_NewCFunction(m_ctx, fn, name, length);
            if (JS_SetPropertyStr(m_ctx, target, name, func) < 0) {
                clearPendingException(m_ctx);
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
                }
            }
        }
    }

    m_installedExtensions |= toInstall;
}

void QQuickJSEngine::processAndEmitException(JSContext* ctx, JSValueConst exc) {
    QString errStr = QuickJSConvert::jsToQString(ctx, exc);
    if (errStr.isNull()) {
        // toString() of the thrown value itself failed. A legitimately thrown
        // empty string is a non-null empty QString and keeps its empty message.
        if (JS_HasException(ctx)) {
            clearPendingException(ctx);
        }
        errStr = QStringLiteral("Unknown exception (toString failed)");
    }

    // Guard diagnostics (QJS_GUARD_TRACE=1): when a stack overflow surfaces,
    // report the catch point relative to the anchor/line. NOTE: this SP is
    // where the exception reached the host catch frame AFTER the interpreter
    // unwound - the guard's fire point itself was at (or below) the line and
    // necessarily deeper than the value printed here, so this number can only
    // bound the fire depth from below; it cannot prove a false fire (a
    // near-anchor catch point can be misread as "fired inside the machinery
    // allowance" while the actual cause is a degenerate budget window, see
    // the warning in reanchorStackGuard()).
    if (guardTraceEnabled()
            && errStr.contains(QLatin1String("Maximum call stack size exceeded"))) {
        const uintptr_t sp = currentStackPointer();
        const qint64 belowAnchor = m_guardAnchorSp > sp
            ? qint64(m_guardAnchorSp - sp) : -qint64(sp - m_guardAnchorSp);
        qWarning("QQuickJSEngine: [guard] stack overflow caught at SP=%llx: %lld KiB "
                 "below the entry anchor; guard line %lld KiB below it (stackSize=%zu KiB); "
                 "the fire point was at or below the line, deeper than the catch point",
                 static_cast<unsigned long long>(sp),
                 static_cast<long long>(belowAnchor / 1024),
                 static_cast<long long>(belowAnchor / 1024
                     - static_cast<qint64>(m_guardStackSize) / 1024),
                 m_guardStackSize / 1024);
    }

    if (receivers(SIGNAL(uncaughtException(QString,int))) == 0) {
        qWarning().noquote() << "QQuickJSEngine: Uncaught JS exception:" << errStr;
        clearPendingException(ctx);
        return;
    }

    emit uncaughtException(errStr, QuickJSConvert::extractExceptionLine(ctx, exc));
}

void QQuickJSEngine::queueUnhandledRejection(JSContext* ctx, JSValueConst promise, JSValueConst reason) {
    PendingRejection entry{JS_DupValue(ctx, promise), JS_DupValue(ctx, reason)};
    try {
        m_pendingRejections.append(entry);
    } catch (...) {
        // Cannot buffer (OOM): free the duplicates and report immediately.
        JS_FreeValues(ctx, entry.promise, entry.reason);
        processAndEmitException(ctx, reason);
    }
}

void QQuickJSEngine::retractHandledRejection(JSContext* ctx, JSValueConst promise) {
    const void* ptr = JS_VALUE_GET_PTR(promise);
    for (qsizetype i = 0; i < m_pendingRejections.size(); ++i) {
        if (JS_VALUE_GET_PTR(m_pendingRejections[i].promise) == ptr) {
            JS_FreeValue(ctx, m_pendingRejections[i].promise);
            JS_FreeValue(ctx, m_pendingRejections[i].reason);
            m_pendingRejections.removeAt(i);
            return;
        }
    }
}

void QQuickJSEngine::flushUnhandledRejections() {
    ensureEngineThread();
    if (m_pendingRejections.isEmpty()) return;
    if (m_rt && JS_IsJobPending(m_rt)) return;   // handlers may still attach
    // Take the list out first: reporting runs user JS (reason toString) which
    // may buffer new rejections for the next tick.
    const QList<PendingRejection> pending = std::exchange(m_pendingRejections, {});
    for (const PendingRejection& e : pending) {
        processAndEmitException(m_ctx, e.reason);
        JS_FreeValue(m_ctx, e.promise);
        JS_FreeValue(m_ctx, e.reason);
    }
}

void QQuickJSEngine::processPendingJobs() {
    ensureEngineThread();
    if (!m_rt) return;
    JSContext* ctx = m_ctx;
    int jobCount = 0;
    constexpr int MaxPendingJobs = 100000;

    while (jobCount < MaxPendingJobs) {
        int ret = JS_ExecutePendingJob(m_rt, &ctx);
        if (ret == 0) break;
        if (ret < 0) {
            ScopedJSValue exc(ctx, JS_GetException(ctx));
            if (!JS_IsNull(exc.value()) && !JS_IsUndefined(exc.value())) {
                processAndEmitException(ctx, exc.value());
            } else {
                clearPendingException(ctx);
            }
        }
        jobCount++;
    }

    if (jobCount == MaxPendingJobs && JS_IsJobPending(m_rt)) {
        qWarning("QQuickJSEngine: Reached maximum pending jobs limit (%d). Some microtasks might be dropped.", MaxPendingJobs);
    }

    // A direct call from C++ is a top-level entry point: the tick ends here.
    if (m_evalDepth == 0) {
        flushUnhandledRejections();
    }
}

QQuickJSValue QQuickJSEngine::evaluate(const QString& script, const QString& fileName,
                                       int lineNumber, QStringList* exceptionStackTrace) {
    ensureEngineThread();
    if (exceptionStackTrace) exceptionStackTrace->clear();

    // Anchor the stack guard (and the fresh timeout quota) before anything
    // else: reporting a stale pending exception below also runs JS
    // (Error.prototype.toString in the uncaught report), which must happen
    // below a guard measured from THIS entry, not from a stale anchor.
    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
        // Starved entry: the usable window below this anchor is inside the
        // guard's safety margin, so even the C++ exception-throw path may
        // not fit below it. Fail fast with the same catchable RangeError the
        // stack guard raises, BEFORE entering JS (engine compile/run or
        // the pending-exception report below) - the engine is unusable on
        // such a stack either way, but the process must stay alive.
        if (Q_UNLIKELY(m_stackStarved)) {
            return newErrorObject(QQuickJSValue::RangeError,
                                   QStringLiteral("Maximum call stack size exceeded"));
        }
    }

    // QJSEngine parity: a pending exception (e.g. throwError() called from C++
    // between script executions) preempts evaluation: the script is not
    // executed and the pending error is returned, reported like any other
    // uncaught exception surfaced by evaluate().
    if (JS_HasException(m_ctx)) {
        QQuickJSValue errVal = QQuickJSValue::fromException(this);
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
        return errVal;
    }

    EvalDepthGuard depthGuard(m_evalDepth);
    // Declared after depthGuard so the flush (which may run user JS via reason
    // toString) happens while the depth guard is still held. Top-level calls
    // only: nested evaluates must not report the enclosing tick.
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

    if (m_evalCacheEnabled && lineNumber == 1) {
        for (int i = 0; i < m_evalCache.size(); ++i) {
            if (m_evalCache[i].script == script && m_evalCache[i].fileName == fileName) {
                m_evalCache.move(i, 0);
                const EvalCacheEntry& entry = m_evalCache.first();

                JSValue bc = JS_DupValue(m_ctx, entry.bytecode);
                JSValue ret = JS_EvalFunction(m_ctx, bc);
                ScopedJSValue retScoped(m_ctx, ret);

                const bool hadException = JS_IsException(retScoped.value());
                QQuickJSValue errVal;
                if (hadException) errVal = QQuickJSValue::fromException(this);

                if (m_autoProcessPromises) processPendingJobs();

                if (hadException) {
                    collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
                    processAndEmitException(m_ctx, errVal.jsVal());
                }
                // QJSEngine parity: a sticky interrupt flag yields an "Interrupted"
                // error object even when the script completed without hitting
                // an interrupt check.
                if (isInterrupted()) {
                    return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
                }
                if (hadException) return errVal;
                return QQuickJSValue(this, retScoped.release(), true);
            }
        }
    }

    const QByteArray utf8Script = script.toUtf8();
    const QByteArray utf8FileName = fileName.toUtf8();

    JSEvalOptions options = {
        .version = JS_EVAL_OPTIONS_VERSION,
        .eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY,
        .filename = utf8FileName.constData(),
        .line_num = lineNumber,
    };

    JSValue bc = JS_Eval2(m_ctx, utf8Script.constData(), static_cast<size_t>(utf8Script.size()), &options);

    if (JS_IsException(bc)) {
        QQuickJSValue errVal = QQuickJSValue::fromException(this);
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
        if (isInterrupted()) {
            return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
        }
        return errVal;
    }

    if (m_evalCacheEnabled && lineNumber == 1) {
        JSValue dupBc = JS_DupValue(m_ctx, bc);
        try {
            m_evalCache.prepend(EvalCacheEntry{.script = script, .fileName = fileName, .bytecode = dupBc});
            while (m_evalCache.size() > m_evalCacheLimit) {
                JS_FreeValue(m_ctx, m_evalCache.takeLast().bytecode);
            }
        } catch (...) {
            JS_FreeValue(m_ctx, dupBc);
        }
    }

    JSValue ret = JS_EvalFunction(m_ctx, bc);
    ScopedJSValue retScoped(m_ctx, ret);

    const bool hadException = JS_IsException(retScoped.value());
    QQuickJSValue errVal;
    if (hadException) errVal = QQuickJSValue::fromException(this);

    if (m_autoProcessPromises) processPendingJobs();

    if (hadException) {
        collectExceptionStackTrace(this, errVal.jsVal(), exceptionStackTrace);
        processAndEmitException(m_ctx, errVal.jsVal());
    }
    // QJSEngine parity: a sticky interrupt flag yields an "Interrupted" error
    // object even when the script completed without hitting an interrupt check.
    if (isInterrupted()) {
        return newErrorObject(QQuickJSValue::GenericError, QStringLiteral("Interrupted"));
    }
    if (hadException) return errVal;

    return QQuickJSValue(this, retScoped.release(), true);
}

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
}

JSModuleDef* QQuickJSEngine::moduleLoader(JSContext* ctx, const char* module_name, void* opaque) {
    QQuickJSEngine* engine = static_cast<QQuickJSEngine*>(opaque);
    if (!engine) return nullptr;

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
}

int QQuickJSEngine::registeredModuleInit(JSContext* ctx, JSModuleDef* m) {
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
    JS_FreePropertyEnum(ctx, props, count);
    return ret;
}

QQuickJSValue QQuickJSEngine::importModule(const QString& fileName) {
    ensureEngineThread();

    // QJSEngine parity: a pending exception preempts the import and is
    // returned as the result instead of executing anything (the leaked
    // exception of a failed registration surfaces here in live Qt 6.12).
    if (JS_HasException(m_ctx)) {
        return QQuickJSValue::fromException(this);
    }

    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
        // Starved entry (see evaluate()): the window below this anchor is
        // inside the guard's safety margin; module compilation/evaluation
        // would run on a stack that cannot even fit the throw path. Refuse
        // with the same catchable RangeError before entering JS.
        if (Q_UNLIKELY(m_stackStarved)) {
            return newErrorObject(QQuickJSValue::RangeError,
                                   QStringLiteral("Maximum call stack size exceeded"));
        }
    }
    EvalDepthGuard depthGuard(m_evalDepth);
    // Declared after depthGuard so the flush (which may run user JS via reason
    // toString) happens while the depth guard is still held. Top-level calls
    // only: nested imports must not report the enclosing tick.
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

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

    if (m_autoProcessPromises) processPendingJobs();

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
    // (getters): start a fresh execution-timeout quota and keep the tick
    // bookkeeping consistent with the other JS entry points.
    const bool topLevel = (m_evalDepth == 0);
    if (topLevel) {
        resetExecutionState();
        // Starved entry (see evaluate()): the snapshot below reads values
        // through JS_GetProperty - it runs user getters ("Values -
        // including getters - are read exactly once, here"), i.e. script
        // execution on a stack the guard already considers starved, where
        // even the overflow throw path may not fit below the anchor.
        // Refuse like the other entry points: 'false' keeps parity with
        // every other registration failure, and the pending RangeError
        // (built through the pure C path - refusing must not itself run
        // script) surfaces at the next importModule()/evaluate(), exactly
        // like a throwing getter's failed registration.
        if (Q_UNLIKELY(m_stackStarved)) {
            JS_ThrowRangeError(m_ctx, "Maximum call stack size exceeded");
            return false;
        }
    }
    EvalDepthGuard depthGuard(m_evalDepth);
    auto rejectionFlush = qScopeGuard([this, topLevel]() { if (topLevel) flushUnhandledRejections(); });

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
            if (JS_SetProperty(m_ctx, snap, props[i].atom, pv) < 0) {
                ok = false;
                continue;
            }
            // "default" is declared separately (it is the value itself,
            // like QJSEngine): a same-named property of the value is ignored.
            if (qstrcmp(name.value(), "default") != 0) {
                exportNames.append(QByteArray(name.value()));
            }
        }
        JS_FreePropertyEnum(m_ctx, props, count);
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

void QQuickJSEngine::setProperty(const char* name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (!name) return;
    if (value.m_engine.data() != this) {
        if (!(value.m_engine.isNull() && JS_IsUndefined(value.m_val))) return;
    }

    JSValue v = JS_DupValue(m_ctx, value.jsVal());
    if (JS_SetPropertyStr(m_ctx, m_globalObj, name, v) < 0) {
        clearPendingException(m_ctx);
    }
}
void QQuickJSEngine::setProperty(const QByteArray& name, const QQuickJSValue& value) { setProperty(name.constData(), value); }
void QQuickJSEngine::setProperty(QStringView name, const QQuickJSValue& value) { setProperty(name.toString(), value); }
// QString names go through the shared string->atom cache: this skips the
// toUtf8() copy and the engine atom-table probe on every call. const char*/
// and QByteArray stay on JS_SetPropertyStr, which is already allocation-free.
void QQuickJSEngine::setProperty(const QString& name, const QQuickJSValue& value) {
    ensureEngineThread();
    if (value.m_engine.data() != this) {
        if (!(value.m_engine.isNull() && JS_IsUndefined(value.m_val))) return;
    }

    JSAtom atom = QuickJSConvert::cachedStringAtom(this, m_ctx, name);
    if (atom == JS_ATOM_NULL) {
        clearPendingException(m_ctx);
        return;
    }

    JSValue v = JS_DupValue(m_ctx, value.jsVal());
    if (JS_SetProperty(m_ctx, m_globalObj, atom, v) < 0) {
        clearPendingException(m_ctx);
    }
    JS_FreeAtom(m_ctx, atom);
}

void QQuickJSEngine::setProperty(const QString& name, const QVariant& value) {
    ensureEngineThread();
    JSValue v = toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(m_ctx);
        return;
    }
    QQuickJSValue val(this, v, true);
    setProperty(name, val);
}

void QQuickJSEngine::setProperty(const char* name, const QVariant& value) {
    ensureEngineThread();
    if (!name) return;
    JSValue v = toJSValue(value);
    if (JS_IsException(v)) {
        clearPendingException(m_ctx);
        return;
    }
    QQuickJSValue val(this, v, true);
    setProperty(name, val);
}

void QQuickJSEngine::setProperty(QStringView name, const QVariant& value) {
    setProperty(name.toString(), value);
}

QQuickJSValue QQuickJSEngine::newQObject(QObject* obj) {
    ensureEngineThread();
    return QQuickJSValue(this, wrapQObject(obj), true);
}

QQuickJSValue QQuickJSEngine::newObject() {
    ensureEngineThread();
    ScopedJSValue obj(m_ctx, JS_NewObject(m_ctx));
    if (JS_IsException(obj.value())) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, obj.release(), true);
}

QQuickJSValue QQuickJSEngine::newSymbol(const QString& name) {
    ensureEngineThread();
    // The description is stored as given: an empty name yields a symbol
    // with an empty description, like QJSEngine::newSymbol().
    const QByteArray utf8 = name.toUtf8();
    ScopedJSValue sym(m_ctx, JS_NewSymbol(m_ctx, utf8.constData(), false));
    if (JS_IsException(sym.value())) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, sym.release(), true);
}

QQuickJSValue QQuickJSEngine::newArray(uint length) {
    ensureEngineThread();
    ScopedJSValue arr(m_ctx, JS_NewArray(m_ctx));
    if (JS_IsException(arr.value())) {
        return QQuickJSValue::fromException(this);
    }

    if (length > 0) {
        ScopedJSValue lenVal(m_ctx, JS_NewUint32(m_ctx, length));
        if (JS_SetProperty(m_ctx, arr.value(), m_atoms.length, lenVal.release()) < 0) {
            clearPendingException(m_ctx);
        }
    }
    return QQuickJSValue(this, arr.release(), true);
}

QQuickJSValue QQuickJSEngine::newFunction(JSCFunction* func, const char* name, int argc) {
    ensureEngineThread();
    ScopedJSValue funcVal(m_ctx, JS_NewCFunction(m_ctx, func, name, argc));
    if (JS_IsException(funcVal.value())) {
        return QQuickJSValue::fromException(this);
    }
    return QQuickJSValue(this, funcVal.release(), true);
}

JSValue QQuickJSEngine::toJSValue(const QVariant& var) {
    ensureEngineThread();
    return toJSValueImpl(var, 0);
}

JSValue QQuickJSEngine::toJSValueImpl(const QVariant& var, int depth) {
    ensureEngineThread();

    if (depth > MaxVariantConversionDepth) {
        JS_ThrowRangeError(m_ctx, "Variant nesting too deep");
        return JS_EXCEPTION;
    }

    // C++ recursion guard for the C++-to-JS direction, symmetric with the
    // floor check in toVariantImpl(): toJSValueImpl <-> listToJSArray/
    // mapToJSObject/hashToJSObject recurse per nesting level with the same
    // depth cap but OUTSIDE the JS stack guard - on fat Debug/ASan builds
    // 64 levels of container conversion exhaust the real thread stack just
    // like the reverse direction. Bail out with the same catchable
    // RangeError the depth cap uses once the remaining stack distance to
    // the thread floor drops below the conversion reserve.
    //
    // Unknown floor (0): the floor check then has nothing to compare
    // against, so the depth-0 entry records its own SP
    // (m_conversionAnchorSp) and deeper levels bound the TOTAL conversion
    // descent by the assumed usable window below that anchor instead - a
    // weaker but sound bound that keeps the guard meaningful on platforms
    // without floor measurement.
    if (depth == 0)
        m_conversionAnchorSp = currentStackPointer();
    if (depth > 0) {
        const uintptr_t sp = currentStackPointer();
        const uintptr_t realFloor = threadStackFloor(sp);
        const uintptr_t floor = realFloor != 0
            ? realFloor
            : (m_conversionAnchorSp > kUnknownFloorAssumedUsable
                   ? m_conversionAnchorSp - kUnknownFloorAssumedUsable : 0);
        if (Q_UNLIKELY(floor != 0 && sp < floor + kVariantStackReserve)) {
            JS_ThrowRangeError(m_ctx, "Variant nesting too deep");
            return JS_EXCEPTION;
        }
    }

    if (!var.isValid()) return JS_UNDEFINED;

    // Never-null types first: numeric, bool, pointer and container variants are
    // never null, so they skip the isNull() call and the enum-type lookup.
    switch (var.userType()) {
    case QMetaType::Bool: return JS_NewBool(m_ctx, var.toBool());
    case QMetaType::Int: case QMetaType::Short: case QMetaType::SChar: case QMetaType::Char:
        return JS_NewInt32(m_ctx, var.toInt());
    case QMetaType::Long: case QMetaType::LongLong:
        return QuickJSConvert::int64ToJS(m_ctx, var.toLongLong());
    case QMetaType::UInt: case QMetaType::UShort: case QMetaType::UChar: {
        quint32 val = var.toUInt();
        if (val <= static_cast<quint32>(std::numeric_limits<int32_t>::max())) return JS_NewInt32(m_ctx, static_cast<int32_t>(val));
        return JS_NewFloat64(m_ctx, static_cast<double>(val));
    }
    case QMetaType::ULong: case QMetaType::ULongLong:
        return QuickJSConvert::uint64ToJS(m_ctx, var.toULongLong());
    case QMetaType::Double: case QMetaType::Float: return JS_NewFloat64(m_ctx, var.toDouble());
    case QMetaType::QStringList:
        return QuickJSConvert::stringListToJSArray(this, m_ctx, static_cast<const QStringList*>(var.constData()));
    case QMetaType::QObjectStar:
        return wrapQObject(*static_cast<QObject* const*>(var.constData()));
    case QMetaType::QVariantList:
        return QuickJSConvert::listToJSArray(this, m_ctx, static_cast<const QVariantList*>(var.constData()), depth);
    case QMetaType::QVariantMap:
        return QuickJSConvert::mapToJSObject(this, m_ctx, static_cast<const QVariantMap*>(var.constData()), depth);
    case QMetaType::QVariantHash:
        return QuickJSConvert::hashToJSObject(this, m_ctx, static_cast<const QVariantHash*>(var.constData()), depth);
    default: break;
    }

    if (var.isNull()) return JS_NULL;
    if (var.userType() == QMetaType::QVariant) return toJSValueImpl(var.value<QVariant>(), depth);

    if (QMetaType(var.userType()).flags() & QMetaType::IsEnumeration) {
        return JS_NewInt32(m_ctx, var.toInt());
    }

    switch (var.userType()) {
    case QMetaType::QString: return qStringToJS(m_ctx, var.toString());
    case QMetaType::QChar: return qStringToJS(m_ctx, QString(var.toChar()));
    case QMetaType::QByteArray:
        return QuickJSConvert::byteArrayToJS(m_ctx, var.toByteArray());
    case QMetaType::QDateTime:
        return QuickJSConvert::dateTimeToJS(m_ctx, var.toDateTime());
    case QMetaType::QDate:
        return QuickJSConvert::dateToJS(m_ctx, var.toDate());
    case QMetaType::QTime:
        return QuickJSConvert::timeToJS(m_ctx, var.toTime());
    default: break;
    }

    if (var.userType() == qMetaTypeId<QQuickJSValue>()) {
        const auto* jsValPtr = static_cast<const QQuickJSValue*>(var.constData());
        if (jsValPtr && jsValPtr->m_engine == this) return JS_DupValue(m_ctx, jsValPtr->jsVal());
    }

    if (var.canConvert<QVariantList>()) {
        QVariantList list = var.value<QVariantList>();
        return QuickJSConvert::listToJSArray(this, m_ctx, &list, depth);
    }

    if (var.canConvert<QVariantHash>()) {
        QVariantHash hash = var.value<QVariantHash>();
        return QuickJSConvert::hashToJSObject(this, m_ctx, &hash, depth);
    }

    if (var.canConvert<QObject*>()) return wrapQObject(var.value<QObject*>());

    return JS_UNDEFINED;
}

QVariant QQuickJSEngine::toVariant(JSValueConst val) {
    ensureEngineThread();
    JSVisitedStack visited;
    return toVariantImpl(val, visited, 0, QQuickJSValue::ConvertJSObjects);
}

QVariant QQuickJSEngine::toVariant(JSValueConst val, QQuickJSValue::ObjectConversionBehavior behavior) {
    ensureEngineThread();
    JSVisitedStack visited;
    return toVariantImpl(val, visited, 0, behavior);
}

QVariant QQuickJSEngine::toVariantImpl(JSValueConst val, JSVisitedStack& visited, int depth,
                                       QQuickJSValue::ObjectConversionBehavior behavior) {
    ensureEngineThread();

    // Depth-0 conversion entry: record the anchor the unknown-floor descent
    // bound below is measured from (see toJSValueImpl's guard comment).
    if (depth == 0)
        m_conversionAnchorSp = currentStackPointer();

    switch (JS_VALUE_GET_TAG(val)) {
    case JS_TAG_UNDEFINED: return QVariant();
    case JS_TAG_NULL: {
        void* nullPtr = nullptr;
        return QVariant(QMetaType(QMetaType::VoidStar), &nullPtr);
    }
    case JS_TAG_BOOL: return JS_VALUE_GET_BOOL(val) != 0;
    case JS_TAG_INT: return JS_VALUE_GET_INT(val);
    case JS_TAG_FLOAT64: {
        double d = JS_VALUE_GET_FLOAT64(val);
        return d;
    }
    case JS_TAG_SHORT_BIG_INT: {
        return QVariant::fromValue(static_cast<qint64>(JS_VALUE_GET_SHORT_BIG_INT(val)));
    }
    case JS_TAG_BIG_INT: {
        size_t len;
        const char* s = JS_ToCStringLen(m_ctx, &len, val);
        if (s) {
            QByteArray ba(s, len);
            JS_FreeCString(m_ctx, s);

            bool ok = false;
            qint64 i64 = ba.toLongLong(&ok);
            if (ok) return QVariant::fromValue(i64);

            quint64 u64 = ba.toULongLong(&ok);
            if (ok) return QVariant::fromValue(u64);

            return QVariant(QString::fromUtf8(ba));
        }
        clearPendingException(m_ctx);
        return QVariant();
    }
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return QuickJSConvert::jsToQString(m_ctx, val);
    case JS_TAG_SYMBOL: {
        // RetainJSObjects wraps the symbol itself (QJSEngine parity). The
        // lossy conversion deliberately yields an invalid QVariant instead of
        // QJSEngine's "Symbol(name)" QString: a string-shaped variant would
        // silently coerce in metacall conversions (e.g. a nullptr written into
        // a QObject* property) instead of throwing a TypeError.
        if (behavior == QQuickJSValue::RetainJSObjects) {
            return QVariant::fromValue(QQuickJSValue(this, JS_DupValue(m_ctx, val), true));
        }
        return QVariant();
    }
    case JS_TAG_OBJECT: {
        void* ptr = JS_VALUE_GET_PTR(val);
        if (visited.contains(ptr)) return QVariant();   // cycle: invalid QVariant instead of a copy

        if (depth > MaxVariantConversionDepth) {
            return QVariant();
        }

        // C++ recursion guard: the toVariantImpl <-> variantFromJSObject
        // chain recurses per nesting level OUTSIDE the JS stack guard, and
        // on fat Debug/ASan builds (Windows MinGW Debug, ~22-36 KiB per
        // frame pair) a deep variant graph exhausts the real thread stack -
        // a hard crash (reproduced with ASan in JS_GetOwnPropertyNamesInternal
        // while converting a 64-level object). The depth cap above cannot
        // know the frame fatness; bail out with an invalid QVariant, exactly
        // like a cycle, once the remaining stack distance to the thread
        // floor drops below the conversion reserve.
        {
            const uintptr_t sp = currentStackPointer();
            const uintptr_t realFloor = threadStackFloor(sp);
            // Unknown floor: bound the descent from the conversion entry
            // anchor by the assumed usable window instead (no bound at
            // depth 0 - the anchor IS the current frame there).
            const uintptr_t floor = realFloor != 0
                ? realFloor
                : (m_conversionAnchorSp > kUnknownFloorAssumedUsable
                       ? m_conversionAnchorSp - kUnknownFloorAssumedUsable : 0);
            if (Q_UNLIKELY(floor != 0 && sp < floor + kVariantStackReserve))
                return QVariant();
        }

        visited.push(ptr);

        QVariant result;
        try {
            if (JS_IsDate(val)) {
                ScopedJSValue timeVal(m_ctx, JS_GetProperty(m_ctx, val, m_atoms.getTime));
                if (JS_IsFunction(m_ctx, timeVal.value())) {
                    ScopedJSValue ret(m_ctx, JS_Call(m_ctx, timeVal.value(), val, 0, nullptr));
                    if (consumeException(m_ctx, ret.value())) {
                        result = QVariant();
                    } else {
                        double msecs = 0;
                        if (JS_ToFloat64(m_ctx, &msecs, ret.value()) == 0 && std::isfinite(msecs)) {
                            result = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(msecs));
                        } else {
                            clearPendingException(m_ctx);
                            result = QVariant();
                        }
                    }
                } else {
                    result = QVariant();
                }
            } else {
                result = QuickJSConvert::variantFromJSObject(this, m_ctx, val, m_qobjectClassID, m_objectClassID, visited, depth + 1, behavior);
            }
        } catch (...) {
            visited.pop();
            throw;
        }

        visited.pop();
        return result;
    }
    default: break;
    }
    return QVariant();
}

JSValue QQuickJSEngine::toJSValueFromVoid(void* data, int metaType) {
    if (metaType == QMetaType::Int) return JS_NewInt32(m_ctx, *static_cast<int*>(data));
    if (metaType == QMetaType::Bool) return JS_NewBool(m_ctx, *static_cast<bool*>(data));
    if (metaType == QMetaType::Double) return JS_NewFloat64(m_ctx, *static_cast<double*>(data));
    if (metaType == QMetaType::Float) return JS_NewFloat64(m_ctx, *static_cast<float*>(data));
    if (metaType == QMetaType::QString) return qStringToJS(m_ctx, *static_cast<QString*>(data));
    if (metaType == QMetaType::QObjectStar) return wrapQObject(*static_cast<QObject**>(data));
    if (metaType == QMetaType::QVariant) return toJSValue(*static_cast<QVariant*>(data));
    if (metaType == QMetaType::UnknownType || metaType == QMetaType::Void) return JS_UNDEFINED;

    // Same scalar fast paths as toJSValueImpl(): metacall argument storage
    // points at a value of exactly the declared type. Every case reads the
    // exact C++ width: `long`/`unsigned long` are 4 bytes on LLP64 (Windows)
    // and 32-bit targets, and a full qint64/quint64 read there would go
    // past the end of the caller's storage.
    if (metaType == QMetaType::LongLong)
        return QuickJSConvert::int64ToJS(m_ctx, *static_cast<qint64*>(data));
    if (metaType == QMetaType::Long)
        return QuickJSConvert::int64ToJS(m_ctx, static_cast<qint64>(*static_cast<long*>(data)));
    if (metaType == QMetaType::ULongLong)
        return QuickJSConvert::uint64ToJS(m_ctx, *static_cast<quint64*>(data));
    if (metaType == QMetaType::ULong)
        return QuickJSConvert::uint64ToJS(m_ctx, static_cast<quint64>(*static_cast<unsigned long*>(data)));
    if (metaType == QMetaType::UInt) return QuickJSConvert::uint64ToJS(m_ctx, *static_cast<uint*>(data));
    if (metaType == QMetaType::UShort) return JS_NewInt32(m_ctx, *static_cast<ushort*>(data));
    if (metaType == QMetaType::UChar) return JS_NewInt32(m_ctx, *static_cast<uchar*>(data));
    if (metaType == QMetaType::Short) return JS_NewInt32(m_ctx, *static_cast<short*>(data));
    if (metaType == QMetaType::Char) return JS_NewInt32(m_ctx, *static_cast<char*>(data));
    if (metaType == QMetaType::SChar) return JS_NewInt32(m_ctx, *static_cast<signed char*>(data));
    if (metaType == QMetaType::QDateTime) return QuickJSConvert::dateTimeToJS(m_ctx, *static_cast<QDateTime*>(data));
    if (metaType == QMetaType::QDate) return QuickJSConvert::dateToJS(m_ctx, *static_cast<QDate*>(data));
    if (metaType == QMetaType::QTime) return QuickJSConvert::timeToJS(m_ctx, *static_cast<QTime*>(data));
    if (metaType == QMetaType::QByteArray) return QuickJSConvert::byteArrayToJS(m_ctx, *static_cast<QByteArray*>(data));
    if (metaType == qMetaTypeId<QVariantHash>())
        return QuickJSConvert::hashToJSObject(this, m_ctx, static_cast<const QVariantHash*>(data), 0);
    if (metaType == qMetaTypeId<QStringList>())
        return QuickJSConvert::stringListToJSArray(this, m_ctx, static_cast<const QStringList*>(data));
    {
        const QMetaType mt(metaType);
        if ((mt.flags() & QMetaType::IsEnumeration) && mt.sizeOf() == sizeof(int))
            return JS_NewInt32(m_ctx, *static_cast<int*>(data));
    }

    QVariant var(QMetaType(metaType), data);
    return toJSValue(var);
}

JSValue QQuickJSEngine::wrapQObject(QObject* obj) {
    ensureEngineThread();

    if (!obj) return JS_NULL;

    try {
        auto it = m_qobjectWrapperCache.find(obj);
        if (it != m_qobjectWrapperCache.end()) return JS_DupValue(m_ctx, it.value());

        auto data = std::make_unique<JSQObjectData>();
        data->obj = obj;

        ScopedJSValue jsObjScoped(m_ctx, JS_NewObjectClass(m_ctx, m_qobjectClassID));
        if (JS_IsException(jsObjScoped.value())) return JS_EXCEPTION;

        const QMetaObject* meta = obj->metaObject();
        data->metaObj = meta;

        auto cacheIt = m_metaObjectCaches.find(meta);
        if (cacheIt != m_metaObjectCaches.end() && cacheIt.value()->className != meta->className()) {
            // The address was reused by a different QMetaObject (dynamic
            // metaobjects can be freed while the cache entry survives):
            // rebuild instead of serving a foreign cache.
            delete cacheIt.value();
            m_metaObjectCaches.erase(cacheIt);
            cacheIt = m_metaObjectCaches.end();
        }
        if (cacheIt == m_metaObjectCaches.end()) {
            auto cache = std::make_unique<JSMetaObjectCache>(m_ctx);
            cache->className = meta->className();
            const int methodCount = meta->methodCount();
            const int propCount = meta->propertyCount();

            cache->entries.reserve(methodCount + propCount);

            for (int i = 0; i < methodCount; ++i) {
                QMetaMethod m = meta->method(i);
                if (m.access() == QMetaMethod::Private) continue;

                const QByteArray name = m.name();
                JSAtom atom = atomFromUtf8(m_ctx, name);
                if (atom == JS_ATOM_NULL) { JS_ThrowOutOfMemory(m_ctx); return JS_EXCEPTION; }
                if (cache->contains(atom)) { JS_FreeAtom(m_ctx, atom); continue; }

                bool ownedByHash = false;
                try {
                    int retType = m.returnType();
                    if (!QMetaType(retType).isRegistered()) retType = QMetaType::UnknownType;

                    const int pCount = m.parameterCount();
                    if (pCount > 255) {
                        // paramCount is stored in a uint8_t; without this check a
                        // release build would silently truncate (256 -> 0 args).
                        qWarning("QQuickJSEngine: Method '%s' has more than 255 parameters (%d), skipping it", name.constData(), pCount);
                        JS_FreeAtom(m_ctx, atom);
                        continue;
                    }

                    MetaCacheEntry entry{
                        .kind = MetaCacheEntry::Kind::Method,
                        .methodType = static_cast<uint8_t>(m.methodType()),
                        .isWritable = false,
                        .isReadable = true,
                        .isResettable = false,
                        .paramCount = static_cast<uint8_t>(pCount),
                        .index = i,
                        .type = retType,
                        .paramTypes = {},
                    };
                    entry.paramTypes.resize(entry.paramCount);
                    for (int j = 0; j < entry.paramCount; ++j) {
                        int paramType = m.parameterType(j);
                        if (!QMetaType(paramType).isRegistered()) paramType = QMetaType::UnknownType;
                        entry.paramTypes[j] = QuickJSConvert::adjustEnumType(paramType);
                    }

                    cache->insert(atom, entry);
                    ownedByHash = true;
                    // Hide QObject::destroyed()/deleteLater() from enumeration
                    // (QJSEngine parity). They remain reachable by direct name
                    // access - documented @warning.
                    static const int s_destroyedIdx1 =
                        QObject::staticMetaObject.indexOfSignal("destroyed(QObject*)");
                    static const int s_destroyedIdx2 =
                        QObject::staticMetaObject.indexOfSignal("destroyed()");
                    static const int s_deleteLaterIdx =
                        QObject::staticMetaObject.indexOfSlot("deleteLater()");
                    const bool isQObjectSubclass = meta->inherits(&QObject::staticMetaObject);
                    if (!isQObjectSubclass
                        || (i != s_destroyedIdx1 && i != s_destroyedIdx2
                            && i != s_deleteLaterIdx)) {
                        cache->methodAtoms.append(atom);
                    }
                } catch (...) {
                    if (!ownedByHash) JS_FreeAtom(m_ctx, atom);
                    JS_ThrowOutOfMemory(m_ctx);
                    return JS_EXCEPTION;
                }
            }

            for (int i = 0; i < propCount; ++i) {
                QMetaProperty p = meta->property(i);
                if (p.isReadable() || p.isWritable()) {
                    JSAtom atom = JS_NewAtom(m_ctx, p.name());
                    if (atom == JS_ATOM_NULL) {
                        JS_ThrowOutOfMemory(m_ctx);
                        return JS_EXCEPTION;
                    }

                    int propType = p.userType();
                    if (!QMetaType(propType).isRegistered()) propType = QMetaType::UnknownType;

                    MetaCacheEntry entry{
                        .kind = MetaCacheEntry::Kind::Property,
                        .methodType = 0,
                        .isWritable = p.isWritable(),
                        .isReadable = p.isReadable(),
                        .isResettable = p.isResettable(),
                        .paramCount = 0,
                        .index = i,
                        .type = QuickJSConvert::adjustEnumType(propType),
                        .paramTypes = {},
                    };

                    const bool existed = cache->contains(atom);
                    bool ownedByHash = false;
                    try {
                        cache->insert(atom, entry);
                        ownedByHash = true;
                        if (existed) {
                            cache->methodAtoms.removeOne(atom);
                            JS_FreeAtom(m_ctx, atom);
                        }
                        cache->propertyAtoms.append(atom);
                    } catch (...) {
                        if (!ownedByHash) {
                            JS_FreeAtom(m_ctx, atom);
                        }
                        JS_ThrowOutOfMemory(m_ctx);
                        return JS_EXCEPTION;
                    }
                }
            }

            JSMetaObjectCache* rawCache = cache.release();
            try {
                cacheIt = m_metaObjectCaches.insert(meta, rawCache);
            } catch (...) {
                delete rawCache;
                JS_ThrowOutOfMemory(m_ctx);
                return JS_EXCEPTION;
            }
        }
        data->metaCache = cacheIt.value();

        JSQObjectData* rawData = data.release();

        if (JS_SetOpaque(jsObjScoped.value(), rawData) != 0) {
            delete rawData;
            JS_ThrowInternalError(m_ctx, "Failed to set QObject opaque data");
            return JS_EXCEPTION;
        }

        JSValue valToInsert = JS_DupValue(m_ctx, jsObjScoped.value());
        try {
            m_qobjectWrapperCache.insert(obj, valToInsert);
        } catch (...) {
            JS_FreeValue(m_ctx, valToInsert);
            JS_ThrowOutOfMemory(m_ctx);
            return JS_EXCEPTION;
        }

        rawData->destroyedConn = QObject::connect(obj, &QObject::destroyed, this, [this, obj](QObject*) {
            auto it = m_qobjectWrapperCache.find(obj);
            if (it != m_qobjectWrapperCache.end()) {
                JSValue jsObj = it.value();
                m_qobjectWrapperCache.erase(it);

                auto data = static_cast<JSQObjectData*>(JS_GetOpaque(jsObj, m_qobjectClassID));
                if (data) {
                    data->clearCaches(m_rt);
                    delete data;
                    JS_SetOpaque(jsObj, nullptr);
                }
                JS_FreeValue(m_ctx, jsObj);
            }
        });
        Q_ASSERT(rawData->destroyedConn);

        return jsObjScoped.release();
    } catch (const std::exception& e) {
        try { JS_ThrowPlainError(m_ctx, "C++ exception in wrapQObject: %s", e.what()); } catch (...) {}
        return JS_EXCEPTION;
    } catch (...) {
        try { JS_ThrowInternalError(m_ctx, "Unknown C++ exception in wrapQObject"); } catch (...) {}
        return JS_EXCEPTION;
    }
}
