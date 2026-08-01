# Current task: reduce public API surface + fix cleanup-callback deadlock

**Delete this file once the work below is applied, builds, and tests pass — it's a handoff note,
not permanent project documentation.**

Owner approved this exact scope (their words): "Please do fuller cut plus the cleanup-callback
deadlock fix," in response to a proposal to close off the "full Qt-like event system" surface
they explicitly don't want (see `CLAUDE.md`'s Purpose & scope).

## Agreed scope

1. Remove `GCoreApplication::postEvent` entirely — unused internally (`deleteLater()`/
   `dispatchMetaCall()` post through the dispatcher directly), it was the only other public way
   to inject an arbitrary event for an arbitrary receiver besides `event()` itself.
2. Make `GObject::event()` **private and non-virtual** (was `public virtual`). Grant
   `GEventDispatcherDefault` friend access — it's the only caller
   (`ep.receiver->event(ep.event)` in `processEvents()`). `timerEvent()` remains the supported,
   overridable extension point; `event()` is pure internal dispatch plumbing now, not meant to be
   overridden or called from outside.
3. Make `GObject::dispatchMetaCall()` **private** (was `public static`) — only `connect()`/
   `callLater()`'s own member-template bodies call it, and those have private access regardless
   of caller-side access level since they're members of `GObject` themselves.
4. Move `GAbstractEventDispatcher::postEvent`/`registerTimer`/`unregisterTimer`/
   `removeEventsForReceiver` from `public` to `protected`, with `friend class GObject;` — these
   let a caller inject events/timers for an *arbitrary* receiver, which should only ever be
   `GObject`'s own internals (`deleteLater`, `startTimer`/`killTimer`, `dispatchMetaCall`,
   `~GObject()`). `processEvents()`/`wakeUp()`/`interrupt()` stay public — they drive/stop the
   loop but don't let you manipulate a specific object. Mirror the same `protected` + friend on
   `GEventDispatcherDefault`'s overrides too (belt-and-suspenders: the base class's access level
   already governs calls made through the `GAbstractEventDispatcher*` that
   `GThread::eventDispatcher()` actually exposes, but this closes the theoretical gap for anyone
   holding a `GEventDispatcherDefault*` directly).
5. Remove the event-filter subsystem entirely: `GObject::installEventFilter`/`removeEventFilter`/
   `eventFilter()`, the `m_eventFilters`/`m_eventFilterMutex` members, and the filter loop inside
   `event()`. This was a real risk on its own, not just scope creep — a filter sitting in front of
   *every* event type could silently swallow a `deleteLater()` or a queued signal delivery.
6. Remove `GObject::customEvent()` (the virtual hook). Inline its one real job — routing
   `GEvent::MetaCall` to `GMetaCallEvent::placeMetaCall()` — directly into `event()`'s switch.
   `GEvent::User`/custom events become unreachable dead code once nothing can `postEvent()` an
   arbitrary event for an arbitrary receiver; `GEvent::User`/`None` were left in the enum as-is
   (not explicitly discussed with the owner, avoided scope creep — flag it to them if they'd also
   like those pruned).
7. Fix the cleanup-callback deadlock in `~GObject()`: copy `m_cleanupCallbacks` out from under
   `m_cleanupMutex` before invoking them (it used to invoke them while still holding the lock, so
   a callback that called `addCleanupCallback()` again on the same object would deadlock on the
   non-recursive mutex). A callback registered *during* that loop is intentionally dropped, not a
   cause of deadlock — document that tradeoff in a comment at the call site.

## Explicitly NOT in scope here (tracked separately in `CHANGES.md`'s "PS" section)

- `GThread::setEventDispatcher()`'s TOCTOU double-delete race against a thread's own
  start/finish lifecycle.
- `GEventDispatcherDefault::m_interrupt` has no reset path (busy-spins at 100% CPU if a custom
  dispatcher is reused after `exit()`/`quit()` and the thread restarts).
- `GObject::callLater()` can silently and permanently drop a call if the target's dispatcher
  isn't set up yet.

## Implementation status: designed, not yet delivered as a patch

The edits above were fully worked out (including exact rationale for each access-control choice)
in a prior session, but never turned into a patch file or applied to this repo. To finish:

1. Apply the 7 edits above to `technology/GObject.h`, `GObject.cpp`,
   `GAbstractEventDispatcher.h`, `GEventDispatcherDefault.h`, `GCoreApplication.h`,
   `GCoreApplication.cpp`.
2. Ripple effects — these reference the removed APIs and won't compile until updated:
   - `tests/test_gobject.cpp`: remove the `GObjectTestFilter`, `CustomTestEvent`,
     `CustomEventReceiver` classes and the `EventFilterInterception`,
     `EventFilterRemovalAndBypass`, `CustomEventHandling` `TEST` cases. Nothing else in that file
     touches the removed APIs (verified: no other reference to `eventFilter`/`customEvent`/
     `.event(`/`dispatchMetaCall`/`GCoreApplication::postEvent` in that file).
   - `src/main.cpp` (Demo target): remove the `TestEventFilter` class and the "Test Event Filter"
     block in `main()` (uses `installEventFilter`, `targetObject.event(&userEv)`,
     `GEvent::User`). Keep the `GCoreApplication app(argc, argv);` line immediately above it —
     later sections (GTimer, WorkerThread tests) still need the running application/dispatcher.
     The removed block's printed label is `[Test 5]`; nothing after it depends on that number, so
     no renumbering is needed.
   - `tests/test_gthread.cpp`, `tests/test_gtimer.cpp`, `tests/test_defect_regressions.cpp`,
     `tests/main.cpp`: confirmed **not** affected (grepped for `eventFilter`/`customEvent`/
     `.event(`/`dispatchMetaCall`/`GCoreApplication::postEvent` — no real hits, one unrelated doc
     comment mentioning `dispatchMetaCall` by name in `test_defect_regressions.cpp`).
3. Rebuild and run the full `Tests` target (debug/ASan config) to confirm everything still passes.
4. Add a new dated entry to `CHANGES.md` describing this change set, in the same style as the
   existing entries.
