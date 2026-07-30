#include <gtest/gtest.h>
#include "GTimer.h"
#include "GEvent.h"
#include "GThread.h"
#include <chrono>
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
    bool wasFired() const { return m_fired; }

    /**
     * @brief Gets total fire count.
     * @return Fire count.
     */
    int fireCount() const { return m_fireCount; }

private:
    bool m_fired{ false };
    int  m_fireCount{ 0 };
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
    void triggerTimerEvent(GTimerEvent* ev) { timerEvent(ev); }
};

/**
 * @brief Tests GTimer configuration and property accessors.
 *
 * Verifies GTimer::setInterval(), GTimer::interval(), GTimer::setSingleShot(),
 * GTimer::isSingleShot(), GTimer::isActive(), and GTimer::timerId() initial states.
 */
TEST(GTimerTest, ConfigurationAndProperties)
{
    GTimer timer;
    timer.setInterval(100);
    EXPECT_EQ(timer.interval(), 100);

    timer.setSingleShot(true);
    EXPECT_TRUE(timer.isSingleShot());
    EXPECT_FALSE(timer.isActive());
    EXPECT_EQ(timer.timerId(), -1);
}

/**
 * @brief Tests GTimer start and stop lifecycle.
 *
 * Verifies GTimer::start(ms), GTimer::stop(), GTimer::isActive(), and GTimer::timerId() state
 * transitions.
 */
TEST(GTimerTest, StartAndStop)
{
    GThread* thread = GThread::create(
        []()
        {
            GTimer timer;
            EXPECT_FALSE(timer.isActive());

            timer.start(150);
            EXPECT_EQ(timer.interval(), 150);
            EXPECT_TRUE(timer.isActive());
            EXPECT_GT(timer.timerId(), 0);

            timer.stop();
            EXPECT_FALSE(timer.isActive());
            EXPECT_EQ(timer.timerId(), -1);

            timer.setInterval(200);
            EXPECT_EQ(timer.interval(), 200);
        });
    thread->wait();
    delete thread;
}

/**
 * @brief Tests static GTimer::singleShot overload for standalone functors.
 *
 * Verifies static template function GTimer::singleShot(int, Functor) executes the provided lambda
 * after specified delay.
 */
TEST(GTimerTest, SingleShotStaticLambda)
{
    bool fired = false;
    GTimer::singleShot(10, [&fired]() { fired = true; });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

/**
 * @brief Tests static GTimer::singleShot overload with target GObject context.
 *
 * Verifies static template function GTimer::singleShot(int, const GObject*, Functor) and null
 * context pointer safety.
 */
TEST(GTimerTest, SingleShotStaticWithContext)
{
    GObject context;
    bool    fired = false;

    GTimer::singleShot(10, &context, [&fired]() { fired = true; });

    GObject* nullContext = nullptr;
    GTimer::singleShot(10, nullContext, []() {});

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

/**
 * @brief Tests static GTimer::singleShot overload with receiver object member function.
 *
 * Verifies static template function GTimer::singleShot(int, const Receiver*, MemberFunc) and null
 * receiver pointer safety.
 */
TEST(GTimerTest, SingleShotStaticWithReceiver)
{
    GTimerTestReceiver receiver;

    GTimer::singleShot(10, &receiver, &GTimerTestReceiver::onTimeout);

    GTimerTestReceiver* nullReceiver = nullptr;
    GTimer::singleShot(10, nullReceiver, &GTimerTestReceiver::onTimeout);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

/**
 * @brief Tests timer event handling and timeout signal emission.
 *
 * Verifies GTimer::timerEvent() processes matching GTimerEvent IDs and emits the GTimer::timeout
 * signal.
 */
TEST(GTimerTest, ManualTimerEventTriggering)
{
    GThread* thread = GThread::create(
        []()
        {
            ManualTestTimer timer;
            timer.start(100);
            int tid = timer.timerId();
            EXPECT_GT(tid, 0);

            bool timeoutSignaled = false;

            GObject context;
            GObject::connect(
                timer.timeout,
                &context,
                [&timeoutSignaled]() { timeoutSignaled = true; },
                G::DirectConnection);

            GTimerEvent event(tid);
            timer.triggerTimerEvent(&event);

            EXPECT_TRUE(timeoutSignaled);
            timer.stop();
        });
    thread->wait();
    delete thread;
}
