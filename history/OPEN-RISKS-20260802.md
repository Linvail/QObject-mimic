# Open risks and defects — snapshot 2026-08-02

Everything known to be wrong, incomplete, or misleading in `technology/` and `tests/` as of this
date, after the two API-surface passes recorded in `CHANGES-20260801.md`. Nothing in this list is
fixed; this is the backlog, not a changelog.

Each item states how it was confirmed. Where something was demonstrated by running code, the
measured result is quoted — items marked *by inspection* were read but not executed, and should be
treated as slightly less certain.

Verification baseline for this snapshot: `Tests` (42 cases) passes on Windows/MSVC and on
WSL2/GCC with AddressSanitizer, both exit 0, no LeakSanitizer output. No ThreadSanitizer run has
ever been done on this codebase — see R11.

---

## R1 — `GCoreApplication::exec()` busy-spins at 100% CPU after a `quit()`/`exec()` cycle

**Severity: High.** `GEventDispatcherDefault::m_interrupt`
([GEventDispatcherDefault.h:118](../technology/GEventDispatcherDefault.h#L118)) is set by
`interrupt()` and there is no code path anywhere that clears it. `processEvents()` returns `false`
immediately while it is set ([GEventDispatcherDefault.cpp:21](../technology/GEventDispatcherDefault.cpp#L21)),
and `GCoreApplication::exec()` loops on `processEvents()` without blocking on anything else.

`quit()` sets `m_interrupt`; a second `exec()` then spins forever, processing nothing, at full
core utilisation.

**Confirmed by execution.** A standalone harness calling `interrupt()` then looping
`processEvents()` the way `exec()` does measured **6,158,569 returns in 200 ms** — no blocking at
all.

**Note the trigger moved.** Previously recorded as "custom dispatcher reused after a thread
restart". Removing `GThread::setEventDispatcher()` closed that path: `GThread::start()` now always
constructs a fresh dispatcher, because the previous one was deleted and the pointer exchanged to
`nullptr` on thread exit. What remains reachable is `GCoreApplication`, whose dispatcher lives for
the whole application lifetime. The prior description of this defect is stale.

**Directly contradicts the mission's own constraint** for the event-dispatcher stage: *"100%
cpu-spin is not allowed."*

**Suggested fix:** clear `m_interrupt` when an event loop (re-)enters, or have `processEvents()`
consume the interrupt rather than latch on it. Needs a decision on which of the two is the
intended semantic before coding.

---

## R2 — `callLater()` can silently and permanently drop a call

**Severity: High.** By inspection of
[GObject.cpp `scheduleCallLater()`](../technology/GObject.cpp) and `dispatchMetaCall()`.

The sequence: `scheduleCallLater()` inserts the key into the pending registry and sets
`isNew = true`, then calls `dispatchMetaCall()`. If the target has no thread data or no dispatcher
yet, `dispatchMetaCall()` deletes the freshly allocated `GMetaCallEvent` and returns. The call
never runs — and the registry entry stays behind.

The compounding part: every later `callLater()` for the same `(context, target)` pair now finds
that stale entry, takes the `isNew = false` branch, updates the invoker, and **never dispatches
again**. The pair is permanently dead for the rest of the object's life, even once a dispatcher
exists. No diagnostic is emitted.

**Suggested fix:** erase the pending entry when dispatch fails, so a later call re-arms. A
louder alternative is to report the failure, but that changes the API contract.

---

## R3 — `GTimer` documents thread safety it does not implement

**Severity: Medium-High.** [GTimer.h:119-122](../technology/GTimer.h#L119-L122).

`m_interval`, `m_timerId`, `m_singleShot`, and `m_active` are plain non-atomic members with no
mutex, read and written by `interval()`/`setInterval()`/`isActive()`/`isSingleShot()`/
`setSingleShot()`/`timerId()`/`start()`/`stop()`. Every one of those accessors carries a doxygen
line ending in "Thread-safe."

`GObject` next door does this properly (`m_nameMutex`, `m_threadDataMutex`,
`std::atomic<GThread*>`), so the inconsistency is likely to be read as intentional by anyone
trusting the docs. Concurrent `start()`/`stop()` from two threads is a plain data race.

Mission guide 7 asks for attention to exactly this. **Either add the synchronisation or correct
the comments — but the current state is worse than both**, because it actively misinforms.

---

## R4 — The `parent` constructor argument does nothing

**Severity: Medium.** `m_parent` ([GObject.h:712](../technology/GObject.h#L712)) is assigned in the
constructor initialiser list and **never read anywhere else** — confirmed by grep across
`technology/`: the only two occurrences are the declaration and that initialisation.

`GObject(GObject* parent = nullptr)` and `GTimer(GObject* parent = nullptr)` reproduce Qt's
signature exactly, and Qt's contract is that the parent takes ownership and deletes the child.
Here it does not. A caller writing the idiomatic Qt thing leaks:

```cpp
GObject* parent = new GObject();
GTimer*  t      = new GTimer(parent);
delete parent;                          // t is leaked; nothing owns it
```

That snippet compiles today (probe `r3`). This is a misleading-API defect rather than a crash: the
signature makes a promise the implementation does not keep.

**Suggested fix:** pick one of implement child ownership, drop the parameter, or document the
non-ownership prominently. Implementing it properly also means deciding what happens when parent
and child have different thread affinity.

---

## R5 — Three of six `GTimer` tests assert nothing

**Severity: Medium (test quality).**
[test_gtimer.cpp:107](../tests/test_gtimer.cpp#L107),
[:121](../tests/test_gtimer.cpp#L121), [:140](../tests/test_gtimer.cpp#L140).

`SingleShotStaticLambda`, `SingleShotStaticWithContext`, and `SingleShotStaticWithReceiver` each
declare a `fired` flag, call `singleShot`, sleep 30 ms, and then **never `EXPECT` anything**. They
would pass if `singleShot()` were an empty function.

Worse, they are probably no-ops in practice: those tests run on the main thread of a binary with
no `GCoreApplication`, so there is no dispatcher, `startTimer()` returns `-1`, and the helper is
deleted immediately. The static `GTimer::singleShot()` family is therefore effectively **untested**
despite appearing covered.

---

## R6 — Platform event dispatchers are empty shells (mission stage 5 incomplete)

**Severity: Medium (unimplemented feature, not a defect).**

`GEventDispatcherWin32.cpp` and `GEventDispatcherLinux.cpp` contain nothing but defaulted
constructors and destructors; both classes inherit `GEventDispatcherDefault` unchanged. There is
no native message-loop integration, so the mission's *"I want to receive OS/platform's messages"*
is not met on either platform.

---

## R7 — `GTimer::timerEvent()` touches `this` after user code may have deleted it

**Severity: Low-Medium.** By inspection of
[GTimer.cpp:42-52](../technology/GTimer.cpp#L42-L52).

```cpp
timeout.emit();
if (m_singleShot) { stop(); }
```

A slot connected with `G::DirectConnection` runs inside `emit()`. If it does `delete timer`, the
subsequent `stop()` — and the `m_singleShot` read before it — operate on freed memory. Using
`deleteLater()` instead is safe, and Qt has a comparable hazard, so this is a sharp edge rather
than a guaranteed bug. Reordering `stop()` before the emit, or holding a life-token guard across
it, would remove it.

---

## R8 — `GObject` is non-copyable only by accident

**Severity: Low.** Probes `r1`/`r2` confirm `GObject b = a;` and `b = a;` both fail to compile
today — but only because the class happens to contain `std::mutex` members, which implicitly
delete the copy operations. Nothing declares that intent: there is no `= delete` anywhere in
`technology/`.

This matters because of what copying *would* do if a future refactor removed or replaced those
mutexes. `m_life` is a `shared_ptr<int>`; a copy would raise its refcount to 2, so `~GObject()`'s
`m_life.reset()` would no longer expire the token. Every `connect()`/`callLater()` wrapper's
`weakLife.lock()` would keep succeeding and would then call a member function on a destroyed
object — a use-after-free, reintroduced silently by an unrelated change.

**Suggested fix:** declare the intent explicitly (delete copy and move construction/assignment).
Cheap, and it converts a future silent regression into a compile error.

---

## R9 — `GCoreApplication`'s singleton is unguarded

**Severity: Low.** By inspection of
[GCoreApplication.cpp](../technology/GCoreApplication.cpp).

`s_instance` is a plain static pointer. The constructor assigns it unconditionally and the
destructor nulls it unconditionally, with no check for an existing instance and no atomicity.
Constructing two applications silently orphans the first; destroying either then leaves
`instance()` returning `nullptr` while a live application still exists. Qt asserts on this.

---

## R10 — Idle event loops wake roughly ten times per second

**Severity: Low.** [GEventDispatcherDefault.cpp:28](../technology/GEventDispatcherDefault.cpp#L28).

`maxWait` defaults to 100 ms and is only shortened by pending timers, never lengthened. A thread
with no events and no timers still wakes ~10×/second forever. Not a spin and not a correctness
bug, but it is avoidable power draw and sits in the same area as R1's mission constraint.

---

## R11 — Confidence limits of the existing test suite

**Severity: informational — please read before treating a green run as proof.**

- Several regression tests in `test_defect_regressions.cpp` are explicitly *best-effort stress
  tests*. The races they target are a few CPU instructions wide and cannot be forced from a
  black-box test. A clean run raises confidence; it is not evidence the defect is gone. Each such
  test says so in its own doc comment.
- **ThreadSanitizer has never been run on this codebase.** ASan/LSan catch memory errors and
  leaks, but most of the fixed and outstanding issues here are *data races*, which is precisely
  what TSan is for and ASan is not. This is the single largest gap in verification.
- Coverage is Windows/MSVC and WSL2/GCC only. No clang build, no 32-bit, no cross-compile,
  despite the mission's cross-platform and no-compiler-specific-tolerance requirements.
- `GObject::objectLife()` and `GTimerEvent`'s constructor remain publicly reachable by deliberate
  decision (rationale in `CHANGES-20260801.md`), not by oversight.

---

## Suggested order

R1 and R2 are the two that can bite a working application at runtime, and both are small,
well-understood fixes — those first. R3 is nearly free if the resolution is "correct the docs" and
should not sit misleading readers either way. R8 is a handful of lines and prevents a future
silent regression. R5 is worth doing before trusting any future timer work. R4 and R6 are design
decisions rather than patches and want a scoping conversation first. R11 argues for a
ThreadSanitizer run being the highest-value single action available, independent of any code
change.
