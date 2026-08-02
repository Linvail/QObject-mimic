# Open risks and defects — snapshot 2026-08-02

Everything known to be wrong, incomplete, or misleading in `technology/` and `tests/`, after the
two API-surface passes in `CHANGES-20260801.md` and the defect pass in `CHANGES-20260802.md`.

Each item states how it was confirmed. Items marked *by inspection* were read but not executed.
Resolved items are kept with their resolution so the history stays readable — see the status
table below for the current picture.

## Status at a glance

| ID  | Risk | Severity | Status |
|-----|------|----------|--------|
| R1  | `GCoreApplication` re-exec busy-spins at 100% CPU | High | **Open** — deferred with GCoreApplication |
| R2  | `callLater()` permanently drops calls after one failure | High | Fixed 2026-08-02 |
| R3  | `GTimer` documents thread safety it lacks | Med-High | Fixed 2026-08-02 |
| R4  | `parent` constructor argument did nothing | Medium | Fixed by owner (`bd19dbb`) |
| R5  | Three `GTimer` tests assert nothing | Medium | Fixed 2026-08-02 |
| R6  | Platform event dispatchers are empty shells | Medium | **Open** — mission stage, tied to GCoreApplication |
| R7  | `GTimer::timerEvent()` touched `this` after user code | Low-Med | Fixed 2026-08-02 (residual documented) |
| R8  | `GObject` non-copyable only by accident | Low | Fixed 2026-08-02 |
| R9  | `GCoreApplication` singleton unguarded | Low | **Open** — deferred with GCoreApplication |
| R10 | Idle event loops woke ~10×/second | Low | Fixed 2026-08-02 |
| R11 | ThreadSanitizer had never been run | Informational | Done 2026-08-02 — results in R12 |
| R12 | Dispatcher deleted while still in use at thread shutdown | High | Fixed 2026-08-02 |
| R13 | Pending `deleteLater()` leaks when an event loop stops | Medium | Fixed 2026-08-02 (worker threads) |
| R14 | Residual ThreadSanitizer warnings, untriaged | Medium | **Open** — 13 remain (was 138) |
| R15 | Remaining "Thread-safe" doc claims not audited against Qt | Medium | Partly done — timers/moveToThread/GTimer fixed |
| R16 | moveToThread() timer migration: TSan lock-order-inversion | Low | Analysed — false positive, kept for the record |

---

## R16 — `moveToThread()` timer migration: TSan lock-order-inversion *(false positive)*

**Severity: Low. Reported by ThreadSanitizer, investigated, and shown to be a phantom cycle. No
code change made; recorded so nobody re-investigates it from scratch.**

`GObject::moveToThread()` now carries active timers to the destination thread (Qt documents this:
"the timers are first stopped in the current thread and restarted ... in the targetThread"),
queueing the re-registration so it runs on the new thread. That makes the moving thread post an
event into a *second* dispatcher's queue — a cross-dispatcher lock interaction that did not exist
before. Adding it made TSan report `lock-order-inversion (potential deadlock)`; bisection confirmed
the report appears and disappears with exactly that one post.

The reported cycle is:

```
M102 (GThread::m_waitMutex) => M645 (a dispatcher mutex) => M647 (another dispatcher mutex) => M102
```

**The third edge cannot happen.** TSan claims a dispatcher mutex is held while `m_waitMutex` is
acquired at [GThread.cpp:91](../technology/GThread.cpp#L91) — but that site is the thread lambda's
final `m_waitCv.notify_all()`, where the only lock taken is `m_waitMutex` and no dispatcher mutex
is held or reachable. One provably impossible edge means the cycle is not real.

Corroborating: both dispatcher mutexes print with address `0x000000000000`, i.e. TSan no longer
has the objects — they were destroyed. The suite creates and destroys dozens of GThreads, each
allocating and freeing a dispatcher (and its mutex) at recurring heap addresses, so the lock-order
graph accumulates edges from dead mutexes and mixes them with live ones. The migration post did not
create a hazard; it added the last edge that closed a cycle in an already-polluted graph.

The real code holds no lock across the post: `moveToThread()` releases `m_threadDataMutex`,
`takeTimersForReceiver()` releases the source dispatcher's mutex, and `processEvents()` dispatches
at [GEventDispatcherDefault.cpp:143](../technology/GEventDispatcherDefault.cpp#L143), outside the
lock scope that closes at line 117.

**Note this does not explain R14.** The same "stale mutex identity" theory does *not* cover
`NewShorterTimerWakesPromptly`, which still reports 6 warnings when run in isolation with a single
worker thread and a single dispatcher. R14 remains open and unexplained.

---

## R15 — Other "Thread-safe" doc claims have not been audited against Qt *(new)*

**Severity: Medium — a documentation-correctness problem that produces real work in the wrong
direction.** `GObject::startTimer()`/`killTimer()` were documented "Thread-safe"; Qt refuses those
calls cross-thread outright. The comment was wrong, and it was believed: R3 added a mutex to
`GTimer` to honour a promise the library should never have made. Those two are now corrected, but
the same class of claim appears elsewhere and has **not** been checked against Qt:

- **`GObject::moveToThread()`** — documented "Thread-safe". Qt restricts it to the object's own
  thread: *"QObject::moveToThread: Current thread (%p) is not the object's thread (%p). Cannot
  move to target thread"*. Qt additionally refuses when the object has a parent. Almost certainly
  the same mistake.
- **`GObject::objectName()` / `setObjectName()`** — documented "Thread-safe". Qt makes no such
  promise for `QObject` property access.
- **`GTimer::interval()` / `setInterval()` / `isActive()` / `isSingleShot()` / `setSingleShot()` /
  `timerId()`** — documented "Thread-safe" and currently mutex-guarded. `QTimer` offers no thread
  safety at all, so this exceeds Qt even though it is now honestly implemented.
- ~~**`GObject::threadData()`**~~ — **done 2026-08-02.** It and `GThread::threadData()` are now
  private (the latter with `friend class GObject`), matching Qt, where
  `QObjectPrivate::threadData` is not public API. It was the last public handle onto the
  dispatcher plumbing — the route by which the R12 hijack was reachable before `GThreadData`'s
  members were encapsulated. Verified unreachable by compile probe. The one test that poked it now
  drives the same mutex through a queued signal emission, which is how `dispatchMetaCall()` reads
  it internally.

**Do not "fix" these unilaterally in either direction.** Each one is a decision about whether to
match Qt or deliberately exceed it, and the owner has asked to be consulted. What must not happen
again is treating a comment as a contract and building to it.

---

## R14 — Residual ThreadSanitizer warnings, not yet triaged *(new)*

**Severity: Medium — unknown, which is the point.** After the R12/R13 fixes the suite still
reports **18 TSan warnings** (14 data races, 4 double-lock), down from **138** at the start of the
day. These have *not* been explained and must not be assumed benign.

What is known:

- They are **not** artifacts of running the whole suite: the affected tests reproduce them when
  run in isolation.
- They are **not** newly introduced. `NewShorterTimerWakesPromptly` — the densest source, 6 of the
  14 — reports **16** warnings at the pre-fix baseline and 6 now, so the work reduced them.
- In nearly every remaining report, **both sides of the "race" hold the same mutex**
  (`GEventDispatcherDefault::m_mutex`), and the accesses are inside `registerTimer()`'s
  `m_timers.push_back()` and `processEvents()`'s timer-collection loop — both plainly inside
  `lock_guard`/`unique_lock` scopes. A genuine data race should not look like that, which suggests
  either a subtlety not yet understood (condition-variable/`unique_lock` modelling, or memory
  reuse defeating TSan's happens-before edges) or a real ordering bug that the mutex does not
  actually cover.
- The 4 double-lock reports are likewise unexplained; the one inspected involves
  `GThread::m_waitMutex` on a stack-allocated thread.

**Next step:** triage properly rather than guess — narrow one report to a minimal reproducer, and
check whether TSan's mutex modelling is being confused by dispatcher memory being freed and
reallocated at the same address across trials. Until then this is an open question, not a clean
bill of health.

---

## R12 — Dispatcher deleted while other threads were still calling into it *(fixed)*

**Severity: High. Found by ThreadSanitizer; fixed 2026-08-02.**

`GThreadData` held the dispatcher as an atomic raw pointer, so a thread finishing could delete its
dispatcher while another thread sat between its own `dispatcher.load()` and the call that
followed. The atomic made the *pointer* load safe and did nothing for the object's lifetime. Every
one of `GObject::startTimer()`, `killTimer()`, `deleteLater()`, `dispatchMetaCall()` and
`~GObject()` had that shape, as did `GThread::exec()`'s own loop.

**Fix:** the dispatcher is now held by `std::shared_ptr` in `GThreadData`, reachable only through
`dispatcher()`, which returns a *strong* reference. A finishing thread merely drops its own
reference; anything mid-call keeps the object alive until it is done. `GThread::eventDispatcher()`
returns a `shared_ptr` for the same reason, and `GCoreApplication` holds its dispatcher by
`shared_ptr` rather than `unique_ptr`.

Verified: the 3 vptr races (ctor/dtor vs virtual call) are gone, and the dedicated stress test
`GThreadDefectTest.DispatcherUseDuringThreadShutdownStress` reports zero TSan warnings.

Note this was the same hazard the removed `GThread::setEventDispatcher()` had, reached through the
thread's own shutdown rather than an external swap — so the earlier claim that removing the setter
"resolves PS item 1 outright" held only for the external-swap path. Both routes are now closed.

---

## R13 — Pending `deleteLater()` leaked when the event loop stopped *(fixed for worker threads)*

**Severity: Medium. Confirmed by LeakSanitizer; fixed 2026-08-02.**

`deleteLater()` posts a `GDeferredDeleteEvent`, and the receiver is only destroyed when that event
is dispatched. If the loop stopped first, `~GEventDispatcherDefault()` freed the pending event but
had no way to free its receiver. Any `deleteLater()` closely followed by `quit()` lost the object;
`GTimer::singleShot()` was especially exposed, since its helper always reclaims itself that way.

**Fix:** `GAbstractEventDispatcher::processDeferredDeletes()` drains outstanding deferred deletes,
and `GThread` calls it after `run()` returns and before releasing the dispatcher — mirroring Qt's
`QThreadPrivate::finish()`, which calls `sendPostedEvents(nullptr, QEvent::DeferredDelete)` at
exactly that point. Covered by
`GObjectDefectTest.PendingDeleteLaterIsProcessedWhenThreadStops`, made deterministic by parking
the worker inside a queued slot while the delete is posted and `quit()` is called.

**Still open for the main thread:** `GCoreApplication::exec()` does not call
`processDeferredDeletes()` on the way out, so the same leak remains there. That is a one-line
addition deliberately left to the GCoreApplication rework rather than folded in here.

---

## R1 — `GCoreApplication::exec()` busy-spins at 100% CPU after a `quit()`/`exec()` cycle

**Severity: High. Open — deliberately deferred**, since `GCoreApplication` is being redesigned
(see `ForAI/misson-detail-GCoreApplication.txt`) and the fix belongs with that work.

`GEventDispatcherDefault::m_interrupt` is set by `interrupt()` and no code path ever clears it.
`processEvents()` returns `false` immediately while it is set, and `exec()` loops on
`processEvents()`. `quit()` sets the flag, so a second `exec()` spins forever, processing nothing.

**Confirmed by execution:** a harness calling `interrupt()` then looping `processEvents()` the way
`exec()` does measured **6,158,569 returns in 200 ms** — no blocking at all.

Note the trigger moved: removing `GThread::setEventDispatcher()` closed the worker-thread route,
because `GThread::start()` always constructs a fresh dispatcher. `GCoreApplication` remains
reachable because its dispatcher lives for the whole application lifetime.

**Directly contradicts the mission's own constraint** for the dispatcher stage: *"100% cpu-spin is
not allowed."*

---

## R6 — Platform event dispatchers are empty shells (mission stage incomplete)

**Severity: Medium (unimplemented feature). Open.**

`GEventDispatcherWin32.cpp` and `GEventDispatcherLinux.cpp` contain only defaulted constructors
and destructors; both classes inherit `GEventDispatcherDefault` unchanged. No native message-loop
integration exists, so *"I want to receive OS/platform's messages"* is unmet on both platforms.
`ForAI/misson-detail-GCoreApplication.txt` sketches the intended approach (an FD the main thread
can wait on for Linux; the harder Windows case where messages go to the thread that created the
window). Sequence this with R12.

---

## R9 — `GCoreApplication`'s singleton is unguarded

**Severity: Low. Open — deferred with GCoreApplication.** By inspection.

`s_instance` is a plain static pointer. The constructor assigns it unconditionally and the
destructor nulls it unconditionally, with no duplicate check and no atomicity. Two applications
silently orphan the first; destroying either leaves `instance()` returning `nullptr` while a live
application still exists. Qt asserts on this.

---

## R11 — ThreadSanitizer: now run, and it found things

**Status: the "never been run" gap is closed.** Reproduce with:

```bash
g++ -std=c++17 -g -O1 -fsanitize=thread -pthread \
  -Itechnology -Isubmodules/external/boost \
  -Isubmodules/external/googletest/googletest/include \
  -Isubmodules/external/googletest/googletest \
  technology/*.cpp tests/*.cpp \
  submodules/external/googletest/googletest/src/gtest-all.cc -o tests_tsan
```

Measured at three points during the day, with the suite passing every time — which is the point:
none of this is visible without TSan, and ASan cannot see it.

| Code state | Tests | TSan warnings |
|---|---|---|
| `bd19dbb` (start of day) | 42 pass | **138** — 120 data race, 3 vptr, 15 double-lock |
| after R2/R3/R7/R8/R10 | 46 pass | **31** — 24 data race, 3 vptr, 4 double-lock |
| after R12/R13 | 48 pass | **18** — 14 data race, 4 double-lock |
| after thread-confining timers | 48 pass | **13** — 11 data race, 2 double-lock |

The large first drop is mostly R3: `GTimer`'s four unguarded members were being hammered across
threads by the existing tests. The vptr races disappearing tracks the R12 fix. What remains is
filed as R14 and is explicitly **not** triaged.

*(An earlier revision of this file reported 31 as the baseline. That was the figure after the first
defect pass had already landed, not the true starting point.)*

**Remaining verification gaps:** TSan is not wired into the build system, so this was a one-off
manual run and will rot unless someone repeats it. Coverage is still Windows/MSVC and WSL2/GCC
only — no clang build, no 32-bit, no cross-compile, despite the mission's cross-platform and
no-compiler-specific-tolerance requirements. And several tests in `test_defect_regressions.cpp`
remain explicitly best-effort stress tests whose clean runs are not proof; each says so in its own
doc comment.

---

## Resolved on 2026-08-02

Full rationale for each in `CHANGES-20260802.md`. Summarised here so this file stays a complete
picture:

- **R2** — `scheduleCallLater()` now erases its registry entry when dispatch fails, so a later
  call re-arms instead of the pair staying dead forever. `dispatchMetaCall()` returns `bool` to
  report deliverability. Regression test verified by reverting the fix.
- **R3** — `GTimer` gained a mutex covering all four members; `start()` is now atomic as a whole
  rather than a racy read-modify-write. The documented thread-safety claim is now true.
- **R5** — the three assertion-free `GTimer` tests were rewritten to run against a real worker
  event loop and actually assert that the functor/member ran. They previously could not have
  fired at all.
- **R7** — `timerEvent()` stops a single-shot timer *before* emitting and touches no member
  afterwards, matching Qt's ordering. **Residual, documented in the code:** `emit()` still runs
  inside the `GSignal` member of the object, so deleting a timer outright from a directly
  connected slot is still unsupported — use `deleteLater()`.
- **R8** — copy and move construction/assignment are now explicitly `= delete`.
- **R10** — an idle dispatcher with no timers now blocks indefinitely instead of waking ten times
  a second. This required two supporting correctness fixes: `wakeUp()` had never actually been
  able to end a predicate-based wait (it changed no state, and only appeared to work because the
  wait was capped at 100 ms), and `interrupt()` set its flag without holding `m_mutex`, leaving a
  lost-wakeup window that an unbounded wait would have turned into a permanent hang.

## Suggested order

R14 is the most valuable next step: 18 unexplained TSan warnings are worth more attention than any
new feature, and until they are triaged the concurrency story is unfinished. R1, R9 and the
main-thread half of R13 are all pinned to the GCoreApplication rework and should go together. R6
is the remaining mission stage and will rework the dispatcher anyway, so it is worth doing after
R14 rather than before. Wiring TSan into the build system is worth doing regardless — every figure
in this document came from a manual one-off run that will rot the moment someone forgets it.
