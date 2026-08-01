// Regression tests for the defects found in the code review of technology/G* (see CHANGES.md /
// the accompanying patch). Kept separate from test_gobject.cpp / test_gthread.cpp / test_gtimer.cpp
// since these specifically target crash/UAF/leak/race scenarios rather than day-to-day API
// behavior, and several of them are stress tests rather than single-shot deterministic checks --
// see each test's doc comment for what it actually proves and how to get the strongest signal
// out of it (most benefit from being run under AddressSanitizer and/or ThreadSanitizer; this
// project's default debug build already enables AddressSanitizer, see tools/toolchain-linux.py).
#include <gtest/gtest.h>
#include "GObject.h"
#include "GThread.h"
#include "GTimer.h"
#include "GSignal.h"
#include "GEventDispatcherDefault.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

// ---------------------------------------------------------------------------------------------
// Defect: GThread::start() could call std::terminate().
// ---------------------------------------------------------------------------------------------

/**
 * @brief Minimal GThread subclass whose run() returns immediately, used to reliably reach the
 * "finished but never waited on" state needed by RestartAfterFinishWithoutWaitDoesNotTerminate.
 */
class DefectInstantFinishThread : public GThread
{
protected:
    /**
     * @brief Returns immediately so the thread reaches the finished state quickly.
     */
    virtual void run() override {}
};

/**
 * @brief Regression test for GThread::start() calling std::terminate() when restarted after a
 * previous run finished but wait() was never called.
 *
 * If a previous run has already completed, m_thread still holds a joinable std::thread;
 * overwriting it (as start() used to do unconditionally) destroys a joinable std::thread, which
 * calls std::terminate() per the standard -- aborting the whole test process, not just failing
 * this test. The fix joins any existing joinable thread first. Fully deterministic.
 */
TEST(GThreadDefectTest, RestartAfterFinishWithoutWaitDoesNotTerminate)
{
    DefectInstantFinishThread thread;

    thread.start();

    // Poll isFinished() without ever calling wait(), so m_thread is left holding a joinable
    // std::thread when we restart below.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!thread.isFinished() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(thread.isFinished())
        << "thread did not finish in time to set up the regression scenario";

    // Before the fix, this line calls std::terminate() and aborts the whole test process rather
    // than failing gracefully -- that abort *is* the failure signal for this test.
    thread.start();

    EXPECT_TRUE(thread.wait(2000));
    EXPECT_TRUE(thread.isFinished());
}

// ---------------------------------------------------------------------------------------------
// Defect: use-after-free in GEventDispatcherDefault::processEvents()'s dispatch loop when a
// GDeferredDeleteEvent and another event for the same receiver land in the same drained batch.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Minimal receiver used only to give processEvents() a second event type to dispatch to
 * the same receiver that a GDeferredDeleteEvent targets, in DeferredDeleteFollowedByQueuedEventInSameBatchDoesNotCrash.
 */
class DefectUafTestReceiver : public GObject
{
public:
    /**
     * @brief No-op slot; only its signature and being invoked (or not) on a live object matters.
     * @param val Unused.
     */
    void onValue(int val) { (void) val; }
};

/**
 * @brief Regression test verifying GEventDispatcherDefault::processEvents() does not call
 * through a receiver after it has already been deleted earlier in the same dispatched batch.
 *
 * eventsToProcess is a flat snapshot drained once per processEvents() call. If a receiver has a
 * GDeferredDeleteEvent queued before another event also targeting it, and both are already
 * sitting in the queue by the time processEvents() drains it, dispatching the DeferredDelete
 * event first deletes the receiver; before the fix, the loop's next iteration then called a
 * virtual function through the now-dangling pointer. removeEventsForReceiver() (invoked from
 * ~GObject()) cannot help here -- it only prunes the dispatcher's live queue, not the
 * already-copied local batch.
 *
 * This is made reliable (not a race) by blocking the worker thread's event loop inside a
 * queued slot while we post both events, guaranteeing they land in the same queue/batch before
 * the worker ever gets a chance to drain it -- the same technique the existing
 * ReceiverDestroyedBeforeQueuedEventHandled test in test_gobject.cpp uses. Reaching the end of
 * this test without crashing is the assertion; run under AddressSanitizer for a hard failure
 * (heap-use-after-free) if this regresses, rather than a possibly-silent one.
 */
TEST(GEventDispatcherDefaultDefectTest, DeferredDeleteFollowedByQueuedEventInSameBatchDoesNotCrash)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto                blockEnteredFuture = blockEnteredPromise.get_future();
    auto                blockReleaseFuture = blockReleasePromise.get_future();

    GObject dummyContext;
    dummyContext.moveToThread(&workerThread);

    GSignal<> blockSig;
    GObject::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        G::QueuedConnection);

    blockSig.emit();
    blockEnteredFuture.get();

    // Worker thread is now stuck inside the blocking slot above, so anything we post next
    // accumulates in the queue and gets drained into a single processEvents() batch once we
    // release it below.
    auto* victim = new DefectUafTestReceiver();
    victim->moveToThread(&workerThread);

    // First: a DeferredDelete event for `victim` ...
    victim->deleteLater();

    // ... then a second, unrelated queued event for the SAME (about-to-be-deleted) receiver.
    GSignal<int> sig;
    GObject::connect(sig, victim, &DefectUafTestReceiver::onValue, G::QueuedConnection);
    sig.emit(123);

    // Release the worker thread; it will now drain and dispatch BOTH events in one
    // processEvents() call, deleting `victim` on the first and (pre-fix) touching the dangling
    // pointer on the second.
    blockReleasePromise.set_value();

    // Sync point: this queued event, posted strictly after both above, is guaranteed to be
    // dispatched strictly after them (single dispatcher, FIFO queue, single consumer thread), so
    // by the time workerThread.wait() returns below, the scenario above has already run.
    GSignal<> quitSig;
    GObject::connect(
        quitSig, &dummyContext, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: GEventDispatcherDefault::processEvents()'s wait_for() predicate ignored timer changes,
// so a newly-registered shorter timer could be starved until the stale wait computed for an
// existing, longer-interval timer happened to elapse.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Regression test verifying that registering a short timer while processEvents() is
 * already asleep waiting on a longer-interval timer causes it to wake and re-evaluate promptly,
 * instead of sleeping out the stale wait duration computed before the new timer existed.
 *
 * Deterministic with generous margins: a 3000ms timer is registered first (letting the worker
 * thread's exec() loop settle into its long wait_for() sleep), then a 50ms single-shot timer is
 * registered and we assert it fires within 1500ms -- more than enough headroom over its actual
 * ~50ms interval, but well under the unrelated long timer's 3000ms, so the two cases are not
 * confusable even accounting for CI scheduling jitter.
 */
TEST(GEventDispatcherDefaultDefectTest, NewShorterTimerWakesPromptly)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GTimer longTimer;
    longTimer.moveToThread(&workerThread);
    longTimer.setInterval(3000);
    longTimer.start();

    // Give the worker thread's exec() loop a chance to notice the long timer and enter its
    // ~3s wait_for() sleep before we register the short timer below.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    GObject context;
    context.moveToThread(&workerThread);

    std::promise<std::chrono::steady_clock::time_point> firePromise;
    auto                                                 fireFuture = firePromise.get_future();

    GTimer shortTimer;
    shortTimer.moveToThread(&workerThread);
    shortTimer.setSingleShot(true);
    GObject::connect(
        shortTimer.timeout,
        &context,
        [&firePromise]() { firePromise.set_value(std::chrono::steady_clock::now()); },
        G::DirectConnection);

    auto shortTimerStart = std::chrono::steady_clock::now();
    shortTimer.setInterval(50);
    shortTimer.start();

    auto status = fireFuture.wait_for(std::chrono::milliseconds(1500));
    ASSERT_EQ(status, std::future_status::ready)
        << "short timer did not fire within 1500ms of being registered while an unrelated "
           "3000ms timer was already pending -- the dispatcher likely slept through the stale "
           "wait_for timeout instead of waking to notice the new, shorter timer.";

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        fireFuture.get() - shortTimerStart)
                        .count();
    EXPECT_LT(elapsedMs, 1500) << "short timer fired after " << elapsedMs
                               << "ms; expected well under the unrelated 3000ms timer's "
                                  "interval.";

    workerThread.quit();
    workerThread.wait();
}

// ---------------------------------------------------------------------------------------------
// Defect: leaked GTimerEvent allocations if interrupt() lands between
// GEventDispatcherDefault::processEvents() collecting already-expired timers and its subsequent
// m_interrupt check.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Best-effort stress test targeting the narrow window in processEvents() between
 * collecting already-expired timers into a local batch and checking m_interrupt right after.
 *
 * Before the fix, any GTimerEvent objects already allocated during collection were leaked if
 * interrupt() landed in that window, since processEvents() returned early without ever handing
 * them to the dispatch loop that would otherwise delete them.
 *
 * There is no portable, non-racy way to deterministically land inside that exact window from
 * outside the class, so this runs many trials with a large number of already-expired timers (to
 * widen the collection loop's duration) racing against a concurrent interrupt() caller. The real
 * assertion this test is written to support is "no leak reported at process exit" -- this
 * project's default debug build already enables AddressSanitizer, which includes
 * LeakSanitizer on Linux (see tools/toolchain-linux.py) -- so run this as part of a normal debug
 * build/test invocation to get that signal. The check below only confirms the scenario runs to
 * completion without crashing or hanging; it cannot by itself prove the leak window was hit.
 */
TEST(GEventDispatcherDefaultDefectTest, InterruptDuringTimerCollectionStress)
{
    constexpr int kTrials         = 30;
    constexpr int kTimersPerTrial = 8000;

    GObject dummyReceiver;

    for (int trial = 0; trial < kTrials; ++trial)
    {
        GEventDispatcherDefault dispatcher;
        for (int i = 0; i < kTimersPerTrial; ++i)
        {
            // interval 0 => already due by the time processEvents() checks it.
            dispatcher.registerTimer(trial * kTimersPerTrial + i, 0, &dummyReceiver);
        }

        std::atomic<bool> go{ false };
        std::thread       racer(
            [&dispatcher, &go]()
            {
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (int i = 0; i < 500; ++i)
                {
                    dispatcher.interrupt();
                }
            });

        go.store(true, std::memory_order_release);
        dispatcher.processEvents();
        racer.join();
    }

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: data race on GObject::m_threadData (written by moveToThread(), read unsynchronized
// elsewhere).
// ---------------------------------------------------------------------------------------------

/**
 * @brief Best-effort stress test targeting the data race on GObject::m_threadData, previously
 * written by moveToThread() and read -- with no synchronization -- by threadData(),
 * startTimer(), killTimer(), deleteLater(), and dispatchMetaCall(), despite all of those being
 * documented thread-safe.
 *
 * Concurrent unsynchronized read/write of a std::shared_ptr is undefined behavior and, in
 * practice, can corrupt the control block (torn reference counts), which typically surfaces as
 * heap corruption / a double-free crash catchable by AddressSanitizer even without
 * ThreadSanitizer, given enough iterations. Best validated under one of those sanitizers; the
 * check below only confirms the scenario runs to completion without crashing.
 */
TEST(GObjectDefectTest, ConcurrentMoveToThreadAndThreadDataAccessStress)
{
    GThread threadA;
    threadA.start();
    while (!threadA.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GThread threadB;
    threadB.start();
    while (!threadB.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GObject subject;

    constexpr int     kIterations = 2000;
    std::atomic<bool> stop{ false };

    // Continuously bounce the subject's thread affinity between threadA and threadB.
    std::thread mover(
        [&subject, &threadA, &threadB, &stop]()
        {
            bool useA = true;
            while (!stop.load(std::memory_order_acquire))
            {
                subject.moveToThread(useA ? &threadA : &threadB);
                useA = !useA;
            }
        });

    // Concurrently hammer every public accessor that reads m_threadData.
    std::thread reader(
        [&subject, &stop]()
        {
            while (!stop.load(std::memory_order_acquire))
            {
                (void) subject.threadData();
                (void) subject.thread();
                int timerId = subject.startTimer(1000000); // never expected to fire in this test
                if (timerId != -1)
                {
                    subject.killTimer(timerId);
                }
            }
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // A few thousand more moveToThread() calls from this third thread too, so the race isn't
    // limited to just `mover` vs `reader`.
    for (int i = 0; i < kIterations; ++i)
    {
        subject.moveToThread(i % 2 == 0 ? &threadA : &threadB);
    }

    stop.store(true, std::memory_order_release);
    mover.join();
    reader.join();

    threadA.quit();
    threadA.wait();
    threadB.quit();
    threadB.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: ~GObject() invalidated the life token (m_life) as its LAST step rather than its first,
// widening the window in which a concurrent connect()/callLater() wrapper could still queue a
// new event for an object that is mid-destruction.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Regression/stress test for the ~GObject() teardown ordering fix.
 *
 * The life token is now invalidated as the very first step of destruction, before running
 * cleanup callbacks or erasing pending call-laters, rather than as the last step after
 * removeEventsForReceiver() has already run. The residual window this leaves (a connect()
 * wrapper's weakLife.lock() succeeding in the brief moment before m_life.reset() executes) is
 * narrow in both the old and new code and not reliably reproducible from a black-box test on its
 * own; what the reordering robustly guarantees is that removeEventsForReceiver() now always runs
 * strictly *after* the life token is invalidated within the same destructor call, giving it a
 * chance to clean up anything that slips past that narrow check -- unlike before, where
 * anything posted in the tail window after removeEventsForReceiver() had already run had no
 * further safety net at all.
 *
 * This test exercises many iterations of concurrent destroy-vs-emit and only checks that they
 * complete without crashing; treat it as a stress/robustness check best combined with
 * AddressSanitizer/ThreadSanitizer, not as proof either the old code always fails or the new
 * code is fully race-free (see the "not patched" notes in CHANGES.md for the fully-airtight fix
 * this would need).
 */
TEST(GObjectDefectTest, ConcurrentEmitDuringDestructionStress)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    constexpr int kIterations = 300;

    for (int i = 0; i < kIterations; ++i)
    {
        auto* victim = new GObject();
        victim->moveToThread(&workerThread);

        GSignal<int> sig;
        GObject::connect(sig, victim, [](int) {}, G::QueuedConnection);

        std::atomic<bool> go{ false };
        std::thread       racer(
            [&sig, &go]()
            {
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                sig.emit(0); // races against `delete victim` below
            });

        go.store(true, std::memory_order_release);
        delete victim;

        racer.join();
    }

    workerThread.quit();
    workerThread.wait();

    SUCCEED();
}
