//
// Copyright Kuzminov Sergey (kuzminov_sergey@mail.ru) 2026
//
// Distributed under the Unlicense License. (See accompanying
// file UNLICENSE.txt or copy at https://unlicense.org/)
//

// Stack guard of QQuickJSEngine (one class, several translation units - see
// QuickJSEngine_p.h): raw stack-pointer reads, per-thread floor measurement
// and caching, budget clamping, the per-entry re-anchoring
// (setMaxStackSize/reanchorStackGuard), the machinery-allowance calibration
// probe and the one-shot process-wide warning latches of the guard. The
// helpers currentStackPointer()/threadStackFloor()/guardTraceEnabled() are
// declared in QuickJSEngine_p.h and used by the core and conversion TUs too.

#include "QuickJSEngine_p.h"

#include <QDebug>

#include <atomic>

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
// The Linux main thread is the one worth a word: pthread_getattr_np derives
// the MAIN thread's bounds from the [stack] VMA and RLIMIT_STACK. Modern
// glibc (verified on 2.41: the reported floor sits at top - rlimit and does
// not move as recursion touches new pages) extends the mapping to the rlimit
// growth boundary, so the cached floor is the true hard bottom and never
// goes stale. Older glibc reported the VMA grown SO FAR - a floor ABOVE the
// true bottom that only extended downwards as pages were touched. The
// re-measure branch in threadStackFloor() (sp < cached => measure again)
// keeps the cache correct under BOTH semantics without telling them apart:
// a too-high floor only makes the guard fire a little earlier, never below
// the real bottom, and re-measuring from a deeper SP refreshes it.

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

static_assert(kUnknownFloorAssumedUsable > kSafetyMargin,
              "the unknown-floor clamp must stay below the assumed usable window");
static_assert(kUnknownFloorAssumedUsable - kSafetyMargin > kMinimumUsableBudget,
              "the unknown-floor budget must stay above the degenerate threshold");

// One-shot warning latches, PER PROCESS (not per engine): embedding hosts
// and test suites construct hundreds of engines, and a systematic guard
// problem (starved stack, degenerate window) is one configuration issue
// of the host process - it must not spam the log once per engine.
std::atomic<bool> g_emergencyWarned{false};
std::atomic<bool> g_degenerateWarned{false};
std::atomic<bool> g_floorUnknownWarned{false};
// Separate one-shot latch for INVALID bounds (a floor at/above the current
// SP, or an absurd distance away): the unknown-floor latch above covers
// the "no measurement API" regime only, and its one-shot warning must not
// be consumed by a different degenerate input (L-8).
std::atomic<bool> g_floorInvalidWarned{false};

// Process-wide calibration cache for calibrateMachineryAllowance(): the
// measured machinery chain is a property of THIS BINARY's frame layout
// (build type, sanitizer flags), not of an engine instance - every engine
// runs the same compile+eval+trampoline code. The first engine in the
// process pays the probe; every later constructor reuses the cached value,
// which is the common monitoring shape (one engine per script, hundreds
// over a process lifetime). Two engines racing on different threads both
// probe and both store - harmless: both measurements are valid for the
// same binary, and the store is idempotent for the reader.
std::atomic<size_t> g_cachedMachineryAllowance{0};

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
        if (floor != 0 && sp > floor) {
            // An absurd distance (64 MiB+ above the floor: sanitizer-relocated
            // threads and the like): clamp silently would hide a guard that
            // no longer reflects reality - say so once, through its own latch
            // so it cannot consume the unknown-floor warning.
            if (!g_floorInvalidWarned.exchange(true)) {
                qWarning("QQuickJSEngine: thread stack bounds implausible "
                         "(%llu KiB above floor) - clamping to the conservative "
                         "unknown-floor value",
                         static_cast<unsigned long long>((sp - floor) / 1024));
            }
        }
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
        // The budget never exceeds what is actually left (N-4): with
        // `above` smaller than the emergency constant, a fixed 16 KiB line
        // would land below the real bottom - an unreachable guard.
        return above > 2 * kEmergencyGuardBudget ? kEmergencyGuardBudget : above / 2;
    }
    const size_t usable = size_t(above - kSafetyMargin);
    return wanted < usable ? wanted : usable;
}
} // namespace

// threadStackFloor/guardTraceEnabled are declared in QuickJSEngine_p.h
// (shared with the core and conversion TUs); their definitions live here at
// global scope, after the anonymous helpers they call
// (measureThreadStackFloor, the one-shot latches). currentStackPointer()
// is defined INLINE in QuickJSEngine_p.h: it sits on the hot path of every
// conversion node, and a cross-TU call would cost a measurable percentage
// on container conversions.

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

// Diagnostics switch: QJS_GUARD_TRACE=1 prints every guard anchor decision
// and the context of every caught "Maximum call stack size exceeded".
bool guardTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("QJS_GUARD_TRACE") == 1;
    return enabled;
}

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
    // resume, module linking - while plain scripts without stack checks
    // keep running).
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

JSValue js_guardProbe(JSContext*, JSValueConst, int, JSValueConst*)
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

    // Process-wide cache hit (see g_cachedMachineryAllowance): skip the
    // probe entirely - no provisional guard, no eval, no /proc churn.
    if (const size_t cached = g_cachedMachineryAllowance.load(std::memory_order_acquire)) {
        m_machineryAllowance = cached;
        if (guardTraceEnabled()) {
            qWarning("QQuickJSEngine: [guard] calibration reused from process cache: allowance=%zu KiB",
                     m_machineryAllowance / 1024);
        }
        return;
    }

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
        // Probe failed (starved stack or eval error): conservative fallback
        // for THIS engine only. A failed probe is not published to the
        // process cache (Q4): the failure is typically a property of the
        // THREAD that happened to construct this engine (a starved stack),
        // and a later engine on a healthy thread must get its chance to
        // measure a real value - publishing the fallback here would install
        // an inflated allowance for every engine in the process.
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
        // Publish only successful calibrations (see above).
        g_cachedMachineryAllowance.store(m_machineryAllowance, std::memory_order_release);
    }
    if (guardTraceEnabled()) {
        qWarning("QQuickJSEngine: [guard] calibration chain=%zu KiB allowance=%zu KiB",
                 chain / 1024, m_machineryAllowance / 1024);
    }
}
