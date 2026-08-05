#include <gtest/gtest.h>
#include "GTimer.h"
#include "GEvent.h"
#include "GSignal.h"
#include "GThread.h"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

/**
 * @brief Helper test receiver class for verifying GTimer singleShot member function invocations.
 */
class GTimerTestReceiver : public GObject
{
public:
    /**
     * @brief Slot called when timer expires.
     */
    void onTimeout()
    {
        m_fired = true;
        m_fireCount++;
    }

    /**
     * @brief Checks if slot was called.
     * @return True if fired.
     */
    bool wasFired() const
    {
        return m_fired;
    }

    /**
     * @brief Gets total fire count.
     * @return Fire count.
     */
    int fireCount() const
    {
        return m_fireCount;
    }

private:
    // Atomic so this test thread can poll them while the worker thread writes them.
    std::atomic<bool> m_fired { false };
    std::atomic<int>  m_fireCount { 0 };
};

/**
 * @brief Test timer subclass to verify manual event handling.
 */
class ManualTestTimer : public GTimer
{
public:
    /**
     * @brief Invokes protected timerEvent for test verification.
     * @param ev Timer event.
     */
    void triggerTimerEvent
        (
        GTimerEvent* ev
        )
    {
        timerEvent( ev );
    }

};

/**
 * @brief Tests GTimer configuration and property accessors.
 *
 * Verifies GTimer::setInterval(), GTimer::interval(), GTimer::setSingleShot(),
 * GTimer::isSingleShot(), GTimer::isActive(), and GTimer::timerId() initial states.
 */
TEST( GTimerTest, ConfigurationAndProperties )
{
    GTimer timer;
    timer.setInterval( 100 );
    EXPECT_EQ( timer.interval(), 100 );

    timer.setSingleShot( true );
    EXPECT_TRUE( timer.isSingleShot() );
    EXPECT_FALSE( timer.isActive() );
    EXPECT_EQ( timer.timerId(), -1 );
}

/**
 * @brief Tests GTimer start and stop lifecycle.
 *
 * Verifies GTimer::start(ms), GTimer::stop(), GTimer::isActive(), and GTimer::timerId() state
 * transitions.
 */
TEST( GTimerTest, StartAndStop )
{
    GThread* thread = GThread::create(
        []()
        {
            GTimer timer;
            EXPECT_FALSE( timer.isActive() );

            timer.start( 150 );
            EXPECT_EQ( timer.interval(), 150 );
            EXPECT_TRUE( timer.isActive() );
            EXPECT_GT( timer.timerId(), 0 );

            timer.stop();
            EXPECT_FALSE( timer.isActive() );
            EXPECT_EQ( timer.timerId(), -1 );

            timer.setInterval( 200 );
            EXPECT_EQ( timer.interval(), 200 );
        } );
    thread->wait();
    delete thread;
}

/**
 * @brief Starts a worker thread running an event loop and blocks until its dispatcher exists.
 * @param thread The thread to start.
 */
static void startWorkerAndWaitForDispatcher
    (
    GThread& thread
    )
{
    thread.start();
    while( !thread.eventDispatcher() )
    {
        std::this_thread::yield();
    }
}

/**
 * @brief Blocks until a queued slot has run on the context object's thread.
 *
 * Used before quitting a worker so that anything already sitting in its queue is processed first
 * -- in particular a single-shot helper's own deleteLater(), which the helper posts immediately
 * after invoking its functor. Quitting without this can stop the loop while that delete is still
 * queued, and the object leaks: the dispatcher's destructor frees the pending event but has no
 * way to delete its receiver. ASan/LSan reports it, so the tests must not race it.
 * @param context Object whose thread's event loop should be drained.
 */
static void drainQueuedEvents
    (
    GObject& context
    )
{
    std::promise<void> syncPromise;
    auto syncFuture = syncPromise.get_future();
    GSignal<>          syncSignal;
    GObject::connect(
        syncSignal,
        &context,
        [&syncPromise]()
        {
            syncPromise.set_value();
        },
        G::QueuedConnection );
    syncSignal.emit();
    EXPECT_EQ( syncFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "worker event loop did not drain.";
}

/**
 * @brief Tests static GTimer::singleShot overload for standalone functors.
 *
 * Verifies GTimer::singleShot(int, Functor) actually runs the functor. The call is made from
 * inside a queued slot so it executes on a thread that both owns a dispatcher and is running an
 * event loop -- the helper object registers its timer against the calling thread, so invoking
 * this from a thread without a running loop (as this test previously did from the main thread of
 * a binary with no GCoreApplication) silently does nothing: startTimer() returns -1 and the
 * helper is destroyed immediately.
 */
TEST( GTimerTest, SingleShotStaticLambdaFires )
{
    GThread worker;
    startWorkerAndWaitForDispatcher( worker );

    GObject context;
    context.moveToThread( &worker );

    std::promise<void> firedPromise;
    auto firedFuture = firedPromise.get_future();

    GSignal<> trigger;
    GObject::connect(
        trigger,
        &context,
        [&firedPromise]()
        {
            GTimer::singleShot( 10, [&firedPromise]()
            {
                firedPromise.set_value();
            } );
        },
        G::QueuedConnection );
    trigger.emit();

    EXPECT_EQ( firedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "singleShot(int, Functor) never invoked its functor.";

    drainQueuedEvents( context );
    worker.quit();
    worker.wait();
}

/**
 * @brief Tests static GTimer::singleShot overload with target GObject context.
 *
 * Verifies GTimer::singleShot(int, const GObject*, Functor) runs the functor on the context
 * object's thread, and that a null context is handled safely.
 */
TEST( GTimerTest, SingleShotStaticWithContextFires )
{
    GThread worker;
    startWorkerAndWaitForDispatcher( worker );

    GObject context;
    context.moveToThread( &worker );

    std::promise<GThread*> firedPromise;
    auto firedFuture = firedPromise.get_future();

    GTimer::singleShot( 10,
        &context,
        [&firedPromise]()
        {
            firedPromise.set_value( GThread::currentThread() );
        } );

    GObject* nullContext = nullptr;
    GTimer::singleShot( 10, nullContext, []()
        {
        } );

    ASSERT_EQ( firedFuture.wait_for( std::chrono::seconds( 5 ) ), std::future_status::ready )
        << "singleShot(int, context, Functor) never invoked its functor.";
    EXPECT_EQ( firedFuture.get(), &worker ) <<
        "functor did not run on the context object's thread.";

    drainQueuedEvents( context );
    worker.quit();
    worker.wait();
}

/**
 * @brief Tests static GTimer::singleShot overload with receiver object member function.
 *
 * Verifies GTimer::singleShot(int, const Receiver*, MemberFunc) invokes the member function, and
 * that a null receiver is handled safely.
 */
TEST( GTimerTest, SingleShotStaticWithReceiverFires )
{
    GThread worker;
    startWorkerAndWaitForDispatcher( worker );

    GTimerTestReceiver receiver;
    receiver.moveToThread( &worker );

    GTimerTestReceiver* nullReceiver = nullptr;
    GTimer::singleShot( 10, nullReceiver, &GTimerTestReceiver::onTimeout );

    GTimer::singleShot( 10, &receiver, &GTimerTestReceiver::onTimeout );

    // wasFired() is atomic, so polling it from this thread while the worker writes it is safe.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( !receiver.wasFired() && std::chrono::steady_clock::now() < deadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    drainQueuedEvents( receiver );
    worker.quit();
    worker.wait();

    EXPECT_TRUE( receiver.wasFired() )
        << "singleShot(int, receiver, MemberFunc) never invoked the member function.";
    EXPECT_EQ( receiver.fireCount(), 1 ) << "the single-shot fired more than once.";
}

/**
 * @brief Tests timer event handling and timeout signal emission.
 *
 * Verifies GTimer::timerEvent() processes matching GTimerEvent IDs and emits the GTimer::timeout
 * signal.
 */
TEST( GTimerTest, ManualTimerEventTriggering )
{
    GThread* thread = GThread::create(
        []()
        {
            ManualTestTimer timer;
            timer.start( 100 );
            int tid = timer.timerId();
            EXPECT_GT( tid, 0 );

            bool timeoutSignaled = false;

            GObject context;
            GObject::connect(
            timer.timeout,
            &context,
            [&timeoutSignaled]()
            {
                timeoutSignaled = true;
            },
            G::DirectConnection );

            GTimerEvent event( tid );
            timer.triggerTimerEvent( &event );

            EXPECT_TRUE( timeoutSignaled );
            timer.stop();
        } );
    thread->wait();
    delete thread;
}
