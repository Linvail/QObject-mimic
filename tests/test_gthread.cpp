#include <gtest/gtest.h>
#include "GThread.h"
#include "GSignal.h"
#include "GEventDispatcherDefault.h"
#include <chrono>
#include <vector>
#include <atomic>

/**
 * @brief Custom GThread subclass for testing thread execution.
 */
class CustomTestThread : public GThread
{
protected:
    virtual void run() override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        m_executed = true;
    }

public:
    /**
     * @brief Checks if run() executed.
     * @return True if run() completed.
     */
    bool wasExecuted() const { return m_executed; }

private:
    bool m_executed{ false };
};

/**
 * @brief Thread subclass for testing GThread::currentThread() pointer.
 */
class ThreadPointerCheckThread : public GThread
{
protected:
    virtual void run() override { m_selfPointer = GThread::currentThread(); }

public:
    /**
     * @brief Gets the captured currentThread pointer.
     * @return Captured thread pointer.
     */
    GThread* selfPointer() const { return m_selfPointer; }

private:
    GThread* m_selfPointer{ nullptr };
};

/**
 * @brief Thread subclass for testing exit code signaling.
 */
class ExitCodeTestThread : public GThread
{
protected:
    virtual void run() override { exit(123); }
};

/**
 * @brief Thread subclass for testing wait timeout logic.
 */
class SlowTestThread : public GThread
{
protected:
    virtual void run() override { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
};

/**
 * @brief Tests thread lifecycle methods and lifecycle signals.
 *
 * Verifies GThread::start(), GThread::wait(), GThread::isRunning(), GThread::isFinished(),
 * and emission of GThread::started and GThread::finished signals.
 */
TEST(GThreadTest, LifecycleAndSignals)
{
    CustomTestThread thread;
    bool             startedFired  = false;
    bool             finishedFired = false;

    GObject context;
    GObject::connect(
        thread.started, &context, [&startedFired]() { startedFired = true; }, G::DirectConnection);

    GObject::connect(
        thread.finished,
        &context,
        [&finishedFired]() { finishedFired = true; },
        G::DirectConnection);

    thread.start();
    thread.wait();

    EXPECT_TRUE(thread.isFinished());
    EXPECT_FALSE(thread.isRunning());
    EXPECT_TRUE(thread.wasExecuted());
    EXPECT_TRUE(startedFired);
    EXPECT_TRUE(finishedFired);
}

/**
 * @brief Tests static thread factory creation.
 *
 * Verifies static function GThread::create() instantiates, starts, and executes a functor on a new
 * thread.
 */
TEST(GThreadTest, CreateStaticFactory)
{
    bool     funcExecuted = false;
    GThread* threadObj    = GThread::create([&funcExecuted]() { funcExecuted = true; });

    threadObj->wait();
    EXPECT_TRUE(funcExecuted);
    delete threadObj;
}

/**
 * @brief Tests retrieval of current thread pointer.
 *
 * Verifies static function GThread::currentThread() returns the pointer to the active GThread
 * instance inside its execution context.
 */
TEST(GThreadTest, CurrentThreadPointer)
{
    ThreadPointerCheckThread thread;
    thread.start();
    thread.wait();

    EXPECT_EQ(thread.selfPointer(), &thread);
}

/**
 * @brief Tests thread exit signal and completion status.
 *
 * Verifies GThread::exit() requests thread termination and transitions GThread to finished state.
 */
TEST(GThreadTest, ThreadExitAndReturnCode)
{
    ExitCodeTestThread thread;
    thread.start();
    thread.wait();
    EXPECT_TRUE(thread.isFinished());
}

/**
 * @brief Tests timed waiting on thread execution.
 *
 * Verifies GThread::wait(ms) returns false when thread execution exceeds timeout, and true once
 * finished.
 */
TEST(GThreadTest, WaitTimeout)
{
    SlowTestThread thread;
    thread.start();

    bool finishedInShortTime = thread.wait(20);
    EXPECT_FALSE(finishedInShortTime);
    EXPECT_TRUE(thread.isRunning());

    bool finishedEventually = thread.wait(1000);
    EXPECT_TRUE(finishedEventually);
    EXPECT_TRUE(thread.isFinished());
}

/**
 * @brief Tests concurrent multi-thread execution.
 *
 * Verifies launching multiple GThread instances in parallel, joining each via GThread::wait(), and
 * ensuring thread safety.
 */
TEST(GThreadTest, MultipleThreadsExecution)
{
    constexpr int         count = 5;
    std::atomic<int>      completedCount{ 0 };
    std::vector<GThread*> threads;

    for (int i = 0; i < count; ++i)
    {
        GThread* t = GThread::create(
            [&completedCount]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                completedCount.fetch_add(1);
            });
        threads.push_back(t);
    }

    for (auto* t : threads)
    {
        t->wait();
        delete t;
    }

    EXPECT_EQ(completedCount.load(), count);
}

/**
 * @brief Tests event dispatcher lifetime across a thread's own start/finish cycle.
 *
 * Replaces the former EventDispatcherSetAndGet test, which drove the removed
 * GThread::setEventDispatcher(). That setter could delete a dispatcher a running
 * exec()/processEvents() loop was still calling into, so a thread now creates and owns its
 * dispatcher itself and eventDispatcher() is read-only. This verifies that contract: none before
 * start(), one owned while running, and cleaned up on exit (the last part relies on
 * AddressSanitizer/LeakSanitizer in the debug build to catch a leak or double free).
 */
TEST(GThreadTest, EventDispatcherOwnedAcrossThreadLifecycle)
{
    GThread thread;
    EXPECT_EQ(thread.eventDispatcher(), nullptr) << "no dispatcher should exist before start()";

    thread.start();
    while (!thread.eventDispatcher())
    {
        std::this_thread::yield();
    }
    EXPECT_NE(thread.eventDispatcher(), nullptr) << "start() should create the thread's dispatcher";

    thread.quit();
    thread.wait();
    EXPECT_TRUE(thread.isFinished());
}
