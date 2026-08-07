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
#include <memory>
#include <thread>

using namespace QtLikeSignal;

// ---------------------------------------------------------------------------------------------
// Defect: GThread::start() could call std::terminate().
// ---------------------------------------------------------------------------------------------

//! Minimal GThread subclass whose run() returns immediately, used to reliably reach the
//! "finished but never waited on" state needed by RestartAfterFinishWithoutWaitDoesNotTerminate.
class DefectInstantFinishThread : public GThread
{
protected:
    //! Returns immediately so the thread reaches the finished state quickly.
    virtual void run() override
    {
    }

};

//! Regression test for GThread::start() calling std::terminate() when restarted after a
//! previous run finished but wait() was never called.
//!
//! If a previous run has already completed, m_thread still holds a joinable std::thread;
//! overwriting it (as start() used to do unconditionally) destroys a joinable std::thread, which
//! calls std::terminate() per the standard -- aborting the whole test process, not just failing
//! this test. The fix joins any existing joinable thread first. Fully deterministic.
TEST( GThreadDefectTest, RestartAfterFinishWithoutWaitDoesNotTerminate )
{
    DefectInstantFinishThread thread;

    thread.start();

    // Poll isFinished() without ever calling wait(), so m_thread is left holding a joinable
    // std::thread when we restart below.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
    while( !thread.isFinished() && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    ASSERT_TRUE( thread.isFinished() )
        << "thread did not finish in time to set up the regression scenario";

    // Before the fix, this line calls std::terminate() and aborts the whole test process rather
    // than failing gracefully -- that abort *is* the failure signal for this test.
    thread.start();

    EXPECT_TRUE( thread.wait( 2000 ) );
    EXPECT_TRUE( thread.isFinished() );
}

// ---------------------------------------------------------------------------------------------
// Defect: use-after-free in GEventDispatcherDefault::processEvents()'s dispatch loop when a
// GDeferredDeleteEvent and another event for the same receiver land in the same drained batch.
// ---------------------------------------------------------------------------------------------

//! Minimal receiver used only to give processEvents() a second event type to dispatch to
//! the same receiver that a GDeferredDeleteEvent targets, in DeferredDeleteFollowedByQueuedEventInSameBatchDoesNotCrash.
class DefectUafTestReceiver : public GObject
{
public:
    //! No-op slot; only its signature and being invoked (or not) on a live object matters.
    void onValue
        (
        int val  //!< Unused.
        )
    {
        ( void ) val;
    }

};

//! Regression test verifying GEventDispatcherDefault::processEvents() does not call
//! through a receiver after it has already been deleted earlier in the same dispatched batch.
//!
//! eventsToProcess is a flat snapshot drained once per processEvents() call. If a receiver has a
//! GDeferredDeleteEvent queued before another event also targeting it, and both are already
//! sitting in the queue by the time processEvents() drains it, dispatching the DeferredDelete
//! event first deletes the receiver; before the fix, the loop's next iteration then called a
//! virtual function through the now-dangling pointer. removeEventsForReceiver() (invoked from
//! ~GObject()) cannot help here -- it only prunes the dispatcher's live queue, not the
//! already-copied local batch.
//!
//! This is made reliable (not a race) by blocking the worker thread's event loop inside a
//! queued slot while we post both events, guaranteeing they land in the same queue/batch before
//! the worker ever gets a chance to drain it -- the same technique the existing
//! ReceiverDestroyedBeforeQueuedEventHandled test in test_gobject.cpp uses. Reaching the end of
//! this test without crashing is the assertion; run under AddressSanitizer for a hard failure
//! (heap-use-after-free) if this regresses, rather than a possibly-silent one.
TEST( GEventDispatcherDefaultDefectTest, DeferredDeleteFollowedByQueuedEventInSameBatchDoesNotCrash
    )
{
    GThread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    GObject dummyContext;
    dummyContext.moveToThread( &workerThread );

    GSignal<> blockSig;
    GObject::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        G::QueuedConnection );

    blockSig.emit();
    blockEnteredFuture.get();

    // Worker thread is now stuck inside the blocking slot above, so anything we post next
    // accumulates in the queue and gets drained into a single processEvents() batch once we
    // release it below.
    auto* victim = new DefectUafTestReceiver();
    victim->moveToThread( &workerThread );

    // First: a DeferredDelete event for `victim` ...
    victim->deleteLater();

    // ... then a second, unrelated queued event for the SAME (about-to-be-deleted) receiver.
    GSignal<int> sig;
    GObject::connect( sig, victim, &DefectUafTestReceiver::onValue, G::QueuedConnection );
    sig.emit( 123 );

    // Release the worker thread; it will now drain and dispatch BOTH events in one
    // processEvents() call, deleting `victim` on the first and (pre-fix) touching the dangling
    // pointer on the second.
    blockReleasePromise.set_value();

    // Sync point: this queued event, posted strictly after both above, is guaranteed to be
    // dispatched strictly after them (single dispatcher, FIFO queue, single consumer thread), so
    // by the time workerThread.wait() returns below, the scenario above has already run.
    GSignal<> quitSig;
    GObject::connect(
        quitSig, &dummyContext, [&workerThread]()
        {
            workerThread.quit();
        }, G::QueuedConnection );
    quitSig.emit();

    workerThread.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: GEventDispatcherDefault::processEvents()'s wait_for() predicate ignored timer changes,
// so a newly-registered shorter timer could be starved until the stale wait computed for an
// existing, longer-interval timer happened to elapse.
// ---------------------------------------------------------------------------------------------

//! Regression test verifying that registering a short timer while processEvents() is
//! already asleep waiting on a longer-interval timer causes it to wake and re-evaluate promptly,
//! instead of sleeping out the stale wait duration computed before the new timer existed.
//!
//! Deterministic with generous margins: a 3000ms timer is registered first (letting the worker
//! thread's exec() loop settle into its long wait_for() sleep), then a 50ms single-shot timer is
//! registered and we assert it fires within 1500ms -- more than enough headroom over its actual
//! ~50ms interval, but well under the unrelated long timer's 3000ms, so the two cases are not
//! confusable even accounting for CI scheduling jitter.
TEST( GEventDispatcherDefaultDefectTest, NewShorterTimerWakesPromptly )
{
    GThread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    // Configure each timer *before* handing it to the worker: once it lives there, its members
    // belong to that thread and touching them from here would be a plain data race (GTimer has no
    // locking, exactly like QTimer). Arming likewise happens on the worker, via callLater(),
    // because start() is thread-confined.
    GTimer longTimer;
    longTimer.moveToThread( &workerThread );
    GObject::callLater( &longTimer, &GTimer::start, 3000 );

    // Give the worker thread's exec() loop a chance to notice the long timer and enter its
    // ~3s wait_for() sleep before we register the short timer below.
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    GObject context;
    context.moveToThread( &workerThread );

    std::promise<std::chrono::steady_clock::time_point> firePromise;
    auto fireFuture = firePromise.get_future();

    GTimer shortTimer;
    shortTimer.setSingleShot( true ); // configure before the object belongs to another thread
    shortTimer.moveToThread( &workerThread );
    GObject::connect(
        shortTimer.timeout,
        &context,
        [&firePromise]()
        {
            firePromise.set_value( std::chrono::steady_clock::now() );
        },
        G::DirectConnection );

    auto shortTimerStart = std::chrono::steady_clock::now();
    GObject::callLater( &shortTimer, &GTimer::start, 50 );

    auto status = fireFuture.wait_for( std::chrono::milliseconds( 1500 ) );
    ASSERT_EQ( status, std::future_status::ready )
        << "short timer did not fire within 1500ms of being registered while an unrelated "
        "3000ms timer was already pending -- the dispatcher likely slept through the stale "
        "wait_for timeout instead of waking to notice the new, shorter timer.";

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        fireFuture.get() - shortTimerStart )
        .count();
    EXPECT_LT( elapsedMs, 1500 ) << "short timer fired after " << elapsedMs
                                 << "ms; expected well under the unrelated 3000ms timer's "
        "interval.";

    // Stop the still-running long timer on the thread that owns it. Leaving it to ~GTimer() would
    // call stop() from this thread, which killTimer() refuses -- correct behaviour (Qt warns the
    // same way), but this test should not be the thing committing the misuse.
    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    GSignal<>          stopSignal;
    GObject::connect(
        stopSignal,
        &context,
        [&longTimer, &stoppedPromise]()
        {
            longTimer.stop();
            stoppedPromise.set_value();
        },
        G::QueuedConnection );
    stopSignal.emit();
    EXPECT_EQ( stoppedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready );

    workerThread.quit();
    workerThread.wait();
}

// ---------------------------------------------------------------------------------------------
// Defect: leaked GTimerEvent allocations if interrupt() lands between
// GEventDispatcherDefault::processEvents() collecting already-expired timers and its subsequent
// m_interrupt check.
// ---------------------------------------------------------------------------------------------

//! Best-effort stress test targeting the narrow window in processEvents() between
//! collecting already-expired timers into a local batch and checking m_interrupt right after.
//!
//! Before the fix, any GTimerEvent objects already allocated during collection were leaked if
//! interrupt() landed in that window, since processEvents() returned early without ever handing
//! them to the dispatch loop that would otherwise delete them.
//!
//! There is no portable, non-racy way to deterministically land inside that exact window from
//! outside the class, so this runs many trials with a large number of already-expired timers (to
//! widen the collection loop's duration) racing against a concurrent interrupt() caller. The real
//! assertion this test is written to support is "no leak reported at process exit" -- this
//! project's default debug build already enables AddressSanitizer, which includes
//! LeakSanitizer on Linux (see tools/toolchain-linux.py) -- so run this as part of a normal debug
//! build/test invocation to get that signal. The check below only confirms the scenario runs to
//! completion without crashing or hanging; it cannot by itself prove the leak window was hit.
//!
//! Exposes GEventDispatcherDefault::registerTimer() for this whitebox stress test.
//!
//! registerTimer() is protected and friended to GObject alone, so that only GObject's internals
//! (startTimer()/killTimer()) can register a timer on another object's behalf. This test needs to
//! drive it directly -- it registers thousands of raw timer IDs on a standalone dispatcher that is
//! deliberately not attached to any thread, to widen the collection loop that the race targets.
//! A using-declaration re-widens access in this subclass without loosening the shipping class.
class DefectTestableDispatcher : public GEventDispatcherDefault
{
public:
    using GEventDispatcherDefault::postEvent;
    using GEventDispatcherDefault::registerTimer;
};

TEST( GEventDispatcherDefaultDefectTest, InterruptDuringTimerCollectionStress )
{
    constexpr int kTrials         = 30;
    constexpr int kTimersPerTrial = 8000;

    GObject dummyReceiver;

    for( int trial = 0; trial < kTrials; ++trial )
    {
        DefectTestableDispatcher dispatcher;
        for( int i = 0; i < kTimersPerTrial; ++i )
        {
            // interval 0 => already due by the time processEvents() checks it.
            dispatcher.registerTimer( trial * kTimersPerTrial + i, 0, &dummyReceiver );
        }

        std::atomic<bool> go { false };
        std::thread racer(
            [&dispatcher, &go]()
            {
                while( !go.load( std::memory_order_acquire ) )
                {
                    std::this_thread::yield();
                }
                for( int i = 0; i < 500; ++i )
                {
                    dispatcher.interrupt();
                }
            } );

        go.store( true, std::memory_order_release );
        dispatcher.processEvents();
        racer.join();
    }

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: moveToThread() left the object's active timers registered with the thread it left, so
// timerEvent() kept being delivered on a thread the object no longer lived in.
// ---------------------------------------------------------------------------------------------

//! Records which thread its timerEvent() is delivered on.
class DefectTimerAffinityProbe : public GObject
{
public:
    //! Gets the thread the most recent timer event arrived on, or nullptr if no event has
    //! arrived.
    GThread* firingThread() const
    {
        return m_firingThread.load();
    }

    //! Gets how many timer events have been delivered.
    int fireCount() const
    {
        return m_fireCount.load();
    }

protected:
    //! Records the delivering thread.
    virtual void timerEvent
        (
        GTimerEvent* event  //!< Unused.
        ) override
    {
        ( void ) event;
        m_firingThread.store( GThread::currentThread() );
        m_fireCount.fetch_add( 1 );
    }

private:
    std::atomic<GThread*> m_firingThread { nullptr };
    std::atomic<int>      m_fireCount { 0 };
};

//! Verifies moveToThread() carries active timers to the destination thread.
//!
//! Qt documents this: "all active timers for the object will be reset. The timers are first stopped
//! in the current thread and restarted (with the same interval) in the targetThread." Without it,
//! a moved object's timers keep firing on the thread it left -- delivering timerEvent() somewhere
//! the object no longer lives, which is precisely what the thread-confinement rules exist to
//! prevent.
//!
//! The timer is started on threadA (start is thread-confined, so via callLater), the object is then
//! moved to threadB by threadA (the only thread allowed to move it), and the test asserts the
//! events subsequently arrive on threadB.
TEST( GObjectDefectTest, MoveToThreadCarriesActiveTimersToTheNewThread )
{
    GThread threadA;
    threadA.start();
    while( !threadA.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    GThread threadB;
    threadB.start();
    while( !threadB.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    DefectTimerAffinityProbe probe;
    ASSERT_TRUE( probe.moveToThread( &threadA ) ); // legal: no affinity yet

    // Arm on threadA, then let it fire there at least once.
    GObject::callLater( &probe, &GObject::startTimer, 10 );

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( probe.fireCount() == 0 && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    ASSERT_GT( probe.fireCount(), 0 ) << "timer never fired on its original thread.";
    ASSERT_EQ( probe.firingThread(), &threadA ) <<
        "timer did not fire on the thread it was started on.";

    // Only threadA may move it, so ask threadA to do so.
    const int countBeforeMove = probe.fireCount();
    GObject::callLater( &probe, &GObject::moveToThread, &threadB );

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( probe.firingThread() != &threadB && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    EXPECT_EQ( probe.firingThread(), &threadB )
        << "after moveToThread(), the timer kept firing on the old thread -- active timers were "
        "not carried across.";
    EXPECT_GT( probe.fireCount(), countBeforeMove ) <<
        "the timer stopped firing entirely after the move.";

    // Stop it on its current owner before tearing down.
    GObject::callLater( &probe, &GObject::moveToThread, nullptr );
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

    threadA.quit();
    threadA.wait();
    threadB.quit();
    threadB.wait();
}

// ---------------------------------------------------------------------------------------------
// Defect: data race on GObject::m_threadData (written by moveToThread(), read unsynchronized
// elsewhere).
// ---------------------------------------------------------------------------------------------

//! Thread that repeatedly re-homes its own object, to drive the m_threadData write side.
//!
//! moveToThread() is now thread-confined, so only the thread that owns an object may re-home it.
//! A thread *can* legally toggle its own object between itself and "no affinity": releasing it is
//! allowed because the caller is the current owner, and re-adopting it is allowed by the
//! unowned-object exception. That gives a tight, entirely legal write loop.
class DefectAffinityTogglingThread : public GThread
{
public:
    GObject subject;  //!< The object whose affinity is toggled. Owned by this thread once run() starts.

    std::atomic<bool> stopToggling { false };  //!< Set to stop the toggle loop.

protected:
    //! Toggles the subject's affinity between this thread and none until stopped.
    virtual void run() override
    {
        while( !stopToggling.load( std::memory_order_acquire ) )
        {
            subject.moveToThread( this );    // adopt: legal, subject currently has no affinity
            subject.moveToThread( nullptr ); // release: legal, this thread is the current owner
        }
    }

};

//! Best-effort stress test targeting the data race on GObject::m_threadData, previously
//! written by moveToThread() and read -- with no synchronization -- by threadData(),
//! startTimer(), killTimer(), deleteLater(), and dispatchMetaCall(), despite all of those being
//! documented thread-safe.
//!
//! Concurrent unsynchronized read/write of a std::shared_ptr is undefined behavior and, in
//! practice, can corrupt the control block (torn reference counts), which typically surfaces as
//! heap corruption / a double-free crash catchable by AddressSanitizer even without
//! ThreadSanitizer, given enough iterations. Best validated under one of those sanitizers; the
//! check below only confirms the scenario runs to completion without crashing.
//!
//! Restructured once moveToThread() became thread-confined. It used to bounce a shared object
//! between two threads from two *other* threads, which the confinement rule now (correctly)
//! refuses -- so it tested nothing but the rejection path. The write side is now driven by the
//! object's own owner, which is the only arrangement the rule permits, while the reads that the
//! m_threadData mutex actually protects continue to come from other threads.
TEST( GObjectDefectTest, ConcurrentMoveToThreadAndThreadDataAccessStress )
{
    DefectAffinityTogglingThread toggler;
    toggler.start();

    std::atomic<bool> stopReading { false };

    // Readers hammering the paths that read m_threadData from another thread -- exactly what the
    // mutex exists for. threadData() itself is no longer public (it is internal plumbing, as in
    // Qt), so the read is driven through a queued signal emission instead: dispatchMetaCall()
    // calls target->threadData() to find the dispatcher to post to, which takes the same lock.
    auto readerBody = [&toggler, &stopReading]()
        {
            GSignal<> sig;
            GObject::connect( sig, &toggler.subject, []()
                {
                }, G::QueuedConnection );
            while( !stopReading.load( std::memory_order_acquire ) )
            {
                ( void ) toggler.subject.thread();
                sig.emit();
            }
        };
    std::thread reader1( readerBody );
    std::thread reader2( readerBody );

    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    stopReading.store( true, std::memory_order_release );
    reader1.join();
    reader2.join();

    toggler.stopToggling.store( true, std::memory_order_release );
    toggler.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: ~GObject() invalidated the life token (m_life) as its LAST step rather than its first,
// widening the window in which a concurrent connect()/callLater() wrapper could still queue a
// new event for an object that is mid-destruction.
// ---------------------------------------------------------------------------------------------

//! Regression/stress test for the ~GObject() teardown ordering fix.
//!
//! The life token is now invalidated as the very first step of destruction, before running
//! cleanup callbacks or erasing pending call-laters, rather than as the last step after
//! removeEventsForReceiver() has already run. The residual window this leaves (a connect()
//! wrapper's weakLife.lock() succeeding in the brief moment before m_life.reset() executes) is
//! narrow in both the old and new code and not reliably reproducible from a black-box test on its
//! own; what the reordering robustly guarantees is that removeEventsForReceiver() now always runs
//! strictly *after* the life token is invalidated within the same destructor call, giving it a
//! chance to clean up anything that slips past that narrow check -- unlike before, where
//! anything posted in the tail window after removeEventsForReceiver() had already run had no
//! further safety net at all.
//!
//! This test exercises many iterations of concurrent destroy-vs-emit and only checks that they
//! complete without crashing; treat it as a stress/robustness check best combined with
//! AddressSanitizer/ThreadSanitizer, not as proof either the old code always fails or the new
//! code is fully race-free (see the "not patched" notes in CHANGES.md for the fully-airtight fix
//! this would need).
TEST( GObjectDefectTest, ConcurrentEmitDuringDestructionStress )
{
    GThread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    constexpr int kIterations = 300;

    for( int i = 0; i < kIterations; ++i )
    {
        auto* victim = new GObject();
        victim->moveToThread( &workerThread );

        GSignal<int> sig;
        GObject::connect( sig, victim, []( int )
            {
            }, G::QueuedConnection );

        std::atomic<bool> go { false };
        std::thread racer(
            [&sig, &go]()
            {
                while( !go.load( std::memory_order_acquire ) )
                {
                    std::this_thread::yield();
                }
                sig.emit( 0 ); // races against `delete victim` below
            } );

        go.store( true, std::memory_order_release );
        delete victim;

        racer.join();
    }

    workerThread.quit();
    workerThread.wait();

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: ~GObject() invoked cleanup callbacks while still holding m_cleanupMutex, so a callback
// that called addCleanupCallback() on the same object self-deadlocked on a non-recursive mutex.
// ---------------------------------------------------------------------------------------------

//! Regression test for the cleanup-callback deadlock in ~GObject().
//!
//! ~GObject() used to run the callbacks inside the m_cleanupMutex lock_guard scope. A callback
//! that re-entered addCleanupCallback() on the same object then blocked forever trying to relock
//! a mutex its own call stack already held (locking a non-recursive std::mutex recursively is
//! undefined behavior; deadlock is the usual manifestation). The fix swaps the callback vector out
//! from under the lock and invokes the copies with the mutex released.
//!
//! Deterministic -- there is no timing window here; the old code failed on every run. How that
//! failure presents is platform-dependent, and both forms were confirmed by temporarily restoring
//! the old destructor body:
//!   - MSVC (verified): its std::mutex detects the recursive lock and aborts the test binary with
//!     exit code 3, before this test can reach any assertion. That abort *is* the failure signal,
//!     the same way GThreadDefectTest.RestartAfterFinishWithoutWaitDoesNotTerminate's
//!     std::terminate() is.
//!   - Implementations that simply block instead (the classic deadlock) are caught by the 5s
//!     timeout below, which is why the destruction runs on its own thread rather than inline --
//!     otherwise a regression would hang the whole binary with no output.
//!
//! On the timeout path the worker stays blocked forever and is detached, so the process may also
//! report a leak or hang at exit; the EXPECT_TRUE is the intended signal and the messy exit is a
//! side effect. Everything the worker touches is owned by it or held via shared_ptr, so nothing
//! dangles if the test body returns first.
TEST( GObjectDefectTest, CleanupCallbackRegisteringAnotherDoesNotDeadlock )
{
    auto reentrantCallSucceeded = std::make_shared<std::atomic<bool> >( false );
    auto* subject                = new GObject();

    subject->addCleanupCallback(
        [subject, reentrantCallSucceeded]()
        {
            // Pre-fix, this call blocks forever: ~GObject() is still holding m_cleanupMutex.
            subject->addCleanupCallback( []()
            {
            } );
            reentrantCallSucceeded->store( true );
        } );

    std::promise<void> donePromise;
    auto doneFuture = donePromise.get_future();
    std::thread destroyer(
        [subject, p = std::move( donePromise )]() mutable
        {
            delete subject;
            p.set_value();
        } );

    const bool finished
        = doneFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;
    EXPECT_TRUE( finished ) << "~GObject() did not finish within 5s -- a cleanup callback that "
        "re-registers another callback deadlocked on m_cleanupMutex.";

    if( finished )
    {
        destroyer.join();
        EXPECT_TRUE( reentrantCallSucceeded->load() )
            << "the re-entrant addCleanupCallback() never returned.";
    }
    else
    {
        destroyer.detach();
    }
}

// ---------------------------------------------------------------------------------------------
// Defect: callLater() permanently disabled a (context, slot) pair if the very first call could
// not be delivered, because the pending-registry entry was left behind.
// ---------------------------------------------------------------------------------------------

//! Minimal callLater() target whose invocation count is safe to poll across threads.
class GObjectDefectCallLaterTarget : public GObject
{
public:
    //! Slot invoked by callLater().
    void onCall()
    {
        m_callCount.fetch_add( 1 );
    }

    //! Gets how many times onCall() has run.
    int callCount() const
    {
        return m_callCount.load();
    }

private:
    std::atomic<int> m_callCount { 0 };
};

//! Regression test for callLater() silently dropping every future call after one failure.
//!
//! scheduleCallLater() inserts the key into the pending registry before attempting to dispatch. If
//! the target has no dispatcher, dispatch fails and the queued metacall is destroyed -- but the
//! registry entry used to survive. Every later callLater() for the same target then matched that
//! stale entry, took the "already scheduled" branch, and never dispatched again, so the pair stayed
//! dead for the rest of the object's life even once a dispatcher existed. The fix erases the entry
//! when dispatch reports failure.
//!
//! Deterministic: the first callLater() targets an object with no thread affinity (this test binary
//! has no GCoreApplication, so the main thread has no dispatcher) and is expected to be lost. The
//! assertion is that the *second* call, after the object is moved to a running worker thread,
//! actually runs. Pre-fix that second call never executes and the wait times out.
TEST( GObjectDefectTest, CallLaterRecoversAfterFirstDispatchFails )
{
    GObjectDefectCallLaterTarget target;

    // No dispatcher anywhere yet -- this call cannot be delivered and is expected to be lost.
    GObject::callLater( &target, &GObjectDefectCallLaterTarget::onCall );
    EXPECT_EQ( target.callCount(), 0 ) << "call ran despite there being no dispatcher.";

    GThread worker;
    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }
    target.moveToThread( &worker );

    // Same target, same slot -- must re-arm now that a dispatcher exists.
    GObject::callLater( &target, &GObjectDefectCallLaterTarget::onCall );

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( target.callCount() == 0 && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    worker.quit();
    worker.wait();

    EXPECT_EQ( target.callCount(), 1 )
        << "callLater() stayed permanently disabled after its first dispatch failed.";
}

// ---------------------------------------------------------------------------------------------
// Defect: a thread deleted its own event dispatcher while other threads were still calling into
// it through the pointer they had already loaded from GThreadData.
// ---------------------------------------------------------------------------------------------

//! Stress test for dispatcher destruction racing against cross-thread use.
//!
//! GThreadData used to hold the dispatcher as an atomic raw pointer. That made the *pointer* load
//! safe and did nothing for the object's lifetime: a worker finishing deleted its dispatcher while
//! another thread sat between its own `dispatcher.load()` and the `disp->registerTimer(...)` call
//! that followed, i.e. a use-after-free on every one of startTimer(), killTimer(), deleteLater(),
//! dispatchMetaCall() and ~GObject(). The dispatcher is now a shared_ptr and callers hold a strong
//! reference for the duration of the call, so a finishing thread merely drops its own reference.
//!
//! This is a best-effort stress test, and an honest one: the window is narrow, and a clean run
//! here is *not* what demonstrates the fix. The real signal came from ThreadSanitizer, which
//! reported this race (as ~GEventDispatcherDefault against GObject::startTimer /
//! registerTimer / GObject::event / GThread::exec) on the old code while this very suite passed.
//! Build with -fsanitize=thread to get that signal; see OPEN-RISKS for the exact command. Under a
//! plain build this only checks the scenario runs to completion without crashing.
TEST( GThreadDefectTest, DispatcherUseDuringThreadShutdownStress )
{
    constexpr int kTrials = 40;

    for( int trial = 0; trial < kTrials; ++trial )
    {
        GThread workerThread;
        workerThread.start();
        while( !workerThread.eventDispatcher() )
        {
            std::this_thread::yield();
        }

        // Objects living on the worker, driven from this thread while the worker shuts down.
        GObject subject;
        subject.moveToThread( &workerThread );

        // Queued signal emission is the cross-thread dispatcher path that must stay safe:
        // dispatchMetaCall() loads the target thread's dispatcher and posts to it, which is
        // exactly the load-then-use window this defect lived in. (startTimer()/killTimer() are
        // now thread-confined and can no longer be driven from here.)
        GSignal<>         sig;
        GObject::connect( sig, &subject, []()
            {
            }, G::QueuedConnection );

        std::atomic<bool> stop { false };
        std::thread hammer(
            [&sig, &stop]()
            {
                while( !stop.load( std::memory_order_acquire ) )
                {
                    sig.emit();
                }
            } );

        // Tear the worker down underneath the hammering thread.
        workerThread.quit();
        workerThread.wait();

        stop.store( true, std::memory_order_release );
        hammer.join();
    }

    SUCCEED();
}

// ---------------------------------------------------------------------------------------------
// Defect: an object that called deleteLater() was never destroyed if its thread's event loop
// stopped before the deferred-delete event was dispatched.
// ---------------------------------------------------------------------------------------------

//! Regression test for a pending deleteLater() being dropped when a thread stops.
//!
//! deleteLater() posts a GDeferredDeleteEvent; the receiver is only destroyed when that event is
//! dispatched. If the loop stopped first, ~GEventDispatcherDefault() freed the pending event but
//! had no way to free its receiver, so the object leaked silently. GThread now drains deferred
//! deletes after run() returns, the way Qt's QThreadPrivate::finish() does.
//!
//! Made deterministic rather than racy: the worker is parked inside a queued slot while the
//! deleteLater() is posted and quit() is called, guaranteeing the delete is still sitting in the
//! queue when the loop is told to stop. On release, exec() sees m_exiting and returns without
//! draining -- so the object survives only if the shutdown path handles it.
//!
//! Failure shows up twice over: the flag below stays false, and AddressSanitizer reports the leak.
TEST( GObjectDefectTest, PendingDeleteLaterIsProcessedWhenThreadStops )
{
    GThread workerThread;
    workerThread.start();
    while( !workerThread.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

    GObject dummyContext;
    dummyContext.moveToThread( &workerThread );

    GSignal<> blockSig;
    GObject::connect(
        blockSig,
        &dummyContext,
        [&blockEnteredPromise, &blockReleaseFuture]()
        {
            blockEnteredPromise.set_value();
            blockReleaseFuture.wait();
        },
        G::QueuedConnection );

    blockSig.emit();
    blockEnteredFuture.get();

    // The worker is now parked inside the slot above, so nothing below can be dispatched until
    // it is released.
    auto destroyed = std::make_shared<std::atomic<bool> >( false );
    GObject* victim    = new GObject();
    victim->moveToThread( &workerThread );
    victim->addCleanupCallback( [destroyed]()
        {
            destroyed->store( true );
        } );
    victim->deleteLater();

    workerThread.quit();
    blockReleasePromise.set_value();
    workerThread.wait();

    EXPECT_TRUE( destroyed->load() )
        << "deleteLater() was still pending when the loop stopped, and the object was leaked "
        "instead of destroyed.";
}

// ---------------------------------------------------------------------------------------------
// Defect: an idle GEventDispatcherDefault woke ~10x/second, and wakeUp() could not actually end
// a wait because the wait is predicate-based and wakeUp() changed no state.
// ---------------------------------------------------------------------------------------------

//! Verifies an idle processEvents() blocks instead of polling, and that a posted event
//! still ends the wait promptly.
//!
//! The wait used to be capped at 100ms regardless of whether anything was scheduled, so an idle
//! thread burned a wakeup ten times a second forever. It is now unbounded when no timers are
//! registered. This checks both halves of that: it must not return on its own while idle, and it
//! must still return quickly once there is something to do -- the second half is the important
//! one, since an unbounded wait that misses a notification would hang forever.
TEST( GEventDispatcherDefaultDefectTest, IdleWaitBlocksButStillWakesOnPostedEvent )
{
    DefectTestableDispatcher dispatcher;
    GObject receiver;

    std::promise<void> returnedPromise;
    auto returnedFuture = returnedPromise.get_future();
    std::thread loop(
        [&dispatcher, &returnedPromise]()
        {
            dispatcher.processEvents();
            returnedPromise.set_value();
        } );

    EXPECT_EQ( returnedFuture.wait_for( std::chrono::milliseconds( 300 ) ), std::future_status::
        timeout )
        << "idle processEvents() returned on its own; it should block until there is work.";

    dispatcher.postEvent( &receiver, new GTimerEvent( 1 ) );

    const bool woke
        = returnedFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;
    if( !woke )
    {
        // Force the loop out so this fails on its assertion rather than hanging in join(). The
        // thread must not simply be detached: it would still be inside processEvents() touching
        // `dispatcher`, which is about to be destroyed with this stack frame.
        dispatcher.interrupt();
    }
    loop.join();

    EXPECT_TRUE( woke ) <<
        "posting an event did not wake the idle wait -- notification was missed.";
}

//! Verifies wakeUp() actually ends an idle wait.
//!
//! processEvents() waits on a predicate, so wakeUp()'s bare notify_all() used to be a no-op: with
//! no state change to observe, the waiter re-evaluated the predicate, saw nothing, and slept
//! again. It only ever appeared to work because the wait was capped at 100ms and would have
//! returned anyway. wakeUp() now sets a flag under the mutex that the predicate tests.
TEST( GEventDispatcherDefaultDefectTest, WakeUpEndsIdleWait )
{
    DefectTestableDispatcher dispatcher;

    std::promise<void> returnedPromise;
    auto returnedFuture = returnedPromise.get_future();
    std::thread loop(
        [&dispatcher, &returnedPromise]()
        {
            dispatcher.processEvents();
            returnedPromise.set_value();
        } );

    ASSERT_EQ( returnedFuture.wait_for( std::chrono::milliseconds( 300 ) ), std::future_status::
        timeout )
        << "idle processEvents() returned before wakeUp() was called.";

    dispatcher.wakeUp();

    const bool woke
        = returnedFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;
    if( !woke )
    {
        // See IdleWaitBlocksButStillWakesOnPostedEvent: interrupt() rather than detach(), so a
        // regression fails on the assertion instead of hanging the binary in join() or leaving a
        // detached thread using a destroyed dispatcher.
        dispatcher.interrupt();
    }
    loop.join();

    EXPECT_TRUE( woke ) << "wakeUp() did not end the wait.";
}

// ---------------------------------------------------------------------------------------------
// Defect: GTimer emitted timeout before stopping a single-shot timer, and touched its own members
// after the emit -- so a slot could observe a stale isActive(), and any member access after user
// code ran was a hazard if that code destroyed the timer.
// ---------------------------------------------------------------------------------------------

//! Verifies a single-shot GTimer is already stopped by the time its timeout slot runs.
//!
//! GTimer::timerEvent() used to emit first and stop afterwards, so a slot observed isActive()
//! == true for a timer that was about to be stopped, and GTimer touched m_singleShot/stop() after
//! arbitrary user code had run. It now stops first and emits last, matching Qt's QTimer ordering.
//!
//! This also covers the reentrancy half of the GTimer locking work: the slot calls back into
//! isActive() and start(), which take the same mutex timerEvent() uses. Holding that mutex across
//! the emit would self-deadlock on a non-recursive mutex, so a hang here is a real failure.
TEST( GTimerDefectTest, SingleShotIsStoppedBeforeTimeoutIsEmitted )
{
    GThread worker;
    worker.start();
    while( !worker.eventDispatcher() )
    {
        std::this_thread::yield();
    }

    std::promise<bool> activePromise;
    auto activeFuture = activePromise.get_future();

    // Stack-allocated deliberately. A heap timer would have to be reclaimed with deleteLater(),
    // which needs the worker's loop to keep running long enough to process it -- quitting first
    // leaks the object, since the dispatcher's destructor frees the queued event but not its
    // receiver. Destroying it here after wait() avoids depending on that entirely.
    GTimer timer;
    timer.moveToThread( &worker );
    timer.setSingleShot( true );

    GObject::connect(
        timer.timeout,
        &timer,
        [&timer, &activePromise]()
        {
            // Reentrant calls into the timer's own locked API from inside the emit.
            const bool active = timer.isActive();
            timer.stop();
            activePromise.set_value( active );
        },
        G::DirectConnection );

    // start() must run on the worker thread so the timer registers against its dispatcher.
    GObject::callLater( &timer, &GTimer::start, 10 );

    const bool fired
        = activeFuture.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready;

    worker.quit();
    worker.wait();

    ASSERT_TRUE( fired ) <<
        "timeout never fired, or the slot deadlocked against GTimer's own mutex.";
    EXPECT_FALSE( activeFuture.get() )
        << "single-shot timer was still active inside its own timeout slot.";
}
