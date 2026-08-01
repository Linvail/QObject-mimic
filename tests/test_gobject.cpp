#include <gtest/gtest.h>
#include "GObject.h"
#include "GSignal.h"
#include "GEvent.h"
#include "GCoreApplication.h"
#include "GThread.h"
#include "GEventDispatcherDefault.h"
#include "GTimer.h"
#include <string>
#include <future>

/**
 * @brief Helper test receiver class for verifying GObject slot invocations.
 */
class GObjectTestReceiver : public GObject
{
public:
    /**
     * @brief Slot for integer parameter signals.
     * @param val Received value.
     */
    void onValueReceived(int val)
    {
        m_lastValue = val;
        m_callCount++;
        m_executedThread = GThread::currentThread();
    }

    /**
     * @brief Used to test overloaded slots.
     * @param val1 First value.
     * @param val2 Second value.
     */
    void onValueReceived(int val1, int val2)
    {
        m_lastValue = val1 + val2;
        m_callCount++;
        m_executedThread = GThread::currentThread();
    }

    /**
     * @brief Slot with non-void return type.
     * @param val Value to return.
     * @return Value plus 1.
     */
    int onValueNonVoidReturn(int val)
    {
        m_lastValue = val;
        m_callCount++;
        m_executedThread = GThread::currentThread();
        return m_lastValue;
    }

    /**
     * @brief Slot taking const reference.
     * @param str String value.
     */
    void onStringConstReference(const std::string& str)
    {
        m_lastString = str;
        m_callCount++;
        m_executedThread = GThread::currentThread();
    }

    /**
     * @brief Slot taking multiple arguments.
     * @param a Int argument.
     * @param b String argument.
     * @param c Double argument.
     */
    void onMultiArg(int a, std::string b, double c)
    {
        m_lastInt    = a;
        m_lastString = b;
        m_lastDouble = c;
        m_callCount++;
        m_executedThread = GThread::currentThread();
    }

    /**
     * @brief Gets the total call count.
     * @return Call count.
     */
    int callCount() const { return m_callCount; }

    /**
     * @brief Gets the last received value.
     * @return Last value.
     */
    int lastValue() const { return m_lastValue; }

    /**
     * @brief Gets the last received int.
     * @return Last int value.
     */
    int lastInt() const { return m_lastInt; }

    /**
     * @brief Gets the last received string.
     * @return Last string value.
     */
    const std::string& lastString() const { return m_lastString; }

    /**
     * @brief Gets the last received double.
     * @return Last double value.
     */
    double lastDouble() const { return m_lastDouble; }

    /**
     * @brief Gets the thread on which the slot was executed.
     * @return Pointer to thread.
     */
    GThread* executedThread() const { return m_executedThread; }

private:
    int         m_callCount{ 0 };
    int         m_lastValue{ 0 };
    int         m_lastInt{ 0 };
    std::string m_lastString;
    double      m_lastDouble{ 0.0 };
    GThread*    m_executedThread{ nullptr };
};

/**
 * @brief Tests direct signal-slot connection and emission.
 *
 * Verifies that GObject::connect() correctly binds a GSignal to a GObject member function slot
 * using G::DirectConnection, and that GSignal::emit() properly invokes the receiver slot.
 */
TEST(GObjectTest, DirectSignalSlotConnection)
{
    GSignal<int>        sig;
    GObjectTestReceiver receiver;

    GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueReceived, G::DirectConnection);

    sig.emit(42);
    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 42);
}

/**
 * @brief Tests signal disconnection via connection handle.
 *
 * Verifies GObject::disconnect() successfully disconnects a previously connected signal handle,
 * preventing subsequent signal emissions from invoking the slot.
 */
TEST(GObjectTest, SignalDisconnection)
{
    GSignal<int>        sig;
    GObjectTestReceiver receiver;

    auto handle = GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueReceived);
    sig.emit(10);
    EXPECT_EQ(receiver.callCount(), 1);

    GObject::disconnect(handle);
    sig.emit(20);
    EXPECT_EQ(receiver.callCount(), 1);
}

/**
 * @brief Tests object naming and thread affinity functions.
 *
 * Verifies GObject::setObjectName(), GObject::objectName(), GObject::thread(), and
 * GObject::moveToThread().
 */
TEST(GObjectTest, ObjectNameAndThreadAffinity)
{
    GObject obj;
    EXPECT_EQ(obj.objectName(), "");

    obj.setObjectName("TestObject");
    EXPECT_EQ(obj.objectName(), "TestObject");

    EXPECT_EQ(obj.thread(), GThread::currentThread());

    GThread dummyThread;
    obj.moveToThread(&dummyThread);
    EXPECT_EQ(obj.thread(), &dummyThread);
}

/**
 * @brief Tests weak life token tracking for object destruction.
 *
 * Verifies GObject::objectLife() returns a valid weak pointer during the lifetime of GObject
 * and expires upon object destruction.
 */
TEST(GObjectTest, ObjectLifeToken)
{
    std::weak_ptr<int> lifeToken;
    {
        GObject obj;
        lifeToken = obj.objectLife();
        EXPECT_FALSE(lifeToken.expired());
    }
    EXPECT_TRUE(lifeToken.expired());
}

/**
 * @brief Tests destruction cleanup callback execution.
 *
 * Verifies GObject::addCleanupCallback() registers callbacks that execute when GObject is
 * destroyed.
 */
TEST(GObjectTest, CleanupCallbacks)
{
    bool callbackFired1 = false;
    bool callbackFired2 = false;

    {
        GObject obj;
        obj.addCleanupCallback([&callbackFired1]() { callbackFired1 = true; });
        obj.addCleanupCallback([&callbackFired2]() { callbackFired2 = true; });
        EXPECT_FALSE(callbackFired1);
        EXPECT_FALSE(callbackFired2);
    }

    EXPECT_TRUE(callbackFired1);
    EXPECT_TRUE(callbackFired2);
}

/**
 * @brief Tests connecting a signal to a functor/lambda with context object.
 *
 * Verifies GObject::connect() overload for functor and lambda slots associated with a context
 * object, testing GSignal::emit() and GSignal::operator().
 */
TEST(GObjectTest, LambdaSlotConnection)
{
    GSignal<int> sig;
    GObject      context;
    int          receivedValue = 0;
    int          callCount     = 0;

    GObject::connect(
        sig,
        &context,
        [&receivedValue, &callCount](int val)
        {
            receivedValue = val;
            callCount++;
        },
        G::DirectConnection);

    sig(100);
    EXPECT_EQ(callCount, 1);
    EXPECT_EQ(receivedValue, 100);

    sig.emit(200);
    EXPECT_EQ(callCount, 2);
    EXPECT_EQ(receivedValue, 200);
}

/**
 * @brief Tests multi-argument signal template instantiation and slot invocation.
 *
 * Verifies GSignal<int, std::string, double> correctly passes diverse primitive and object
 * arguments to a receiver slot.
 */
TEST(GObjectTest, MultiArgumentSignal)
{
    GSignal<int, std::string, double> sig;
    GObjectTestReceiver               receiver;

    GObject::connect(sig, &receiver, &GObjectTestReceiver::onMultiArg, G::DirectConnection);

    sig.emit(7, "Hello Signals", 3.14159);
    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastInt(), 7);
    EXPECT_EQ(receiver.lastString(), "Hello Signals");
    EXPECT_DOUBLE_EQ(receiver.lastDouble(), 3.14159);
}

/**
 * @brief Tests null receiver and context safety when connecting signals.
 *
 * Verifies GObject::connect() safely returns an invalid G::ConnectionHandle when passed nullptr
 * for receiver or context pointers.
 */
TEST(GObjectTest, NullReceiverOrContextConnection)
{
    GSignal<int>         sig;
    GObjectTestReceiver* nullReceiver = nullptr;

    auto handle1 = GObject::connect(sig, nullReceiver, &GObjectTestReceiver::onValueReceived);
    EXPECT_FALSE(handle1.connected());

    GObject* nullContext = nullptr;
    auto     handle2     = GObject::connect(sig, nullContext, [](int) {});
    EXPECT_FALSE(handle2.connected());
}

/**
 * @brief Tests low-level timer registration and cleanup.
 *
 * Verifies GObject::startTimer() returns valid timer IDs and GObject::killTimer() stops active
 * timers.
 */
TEST(GObjectTest, TimerStartAndKill)
{
    GThread* thread = GThread::create(
        []()
        {
            GObject obj;
            int     timerId1 = obj.startTimer(100);
            int     timerId2 = obj.startTimer(200);

            EXPECT_GT(timerId1, 0);
            EXPECT_GT(timerId2, 0);
            EXPECT_NE(timerId1, timerId2);

            obj.killTimer(timerId1);
            obj.killTimer(timerId2);
        });
    thread->wait();
    delete thread;
}

/**
 * @brief Tests connect() with member function slot when receiver lives in another thread.
 *
 * Verifies that GObject::connect() with G::AutoConnection or G::QueuedConnection routes signal
 * emissions across thread boundaries into the receiver's thread event loop for execution.
 */
TEST(GObjectTest, CrossThreadMemberFunctionConnection)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GSignal<int>        sig;
    GObjectTestReceiver receiver;
    receiver.moveToThread(&workerThread);

    GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueReceived, G::AutoConnection);
    GObject::connect(
        sig, &receiver, [&workerThread](int) { workerThread.quit(); }, G::AutoConnection);

    sig.emit(100);

    workerThread.wait();

    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 100);
    EXPECT_EQ(receiver.executedThread(), &workerThread);
}

/**
 * @brief Tests connect() with a lambda slot and context living in another thread.
 *
 * Verifies that GObject::connect() with a context object living in a worker thread executes
 * the lambda slot inside the worker thread context.
 */
TEST(GObjectTest, CrossThreadLambdaConnection)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GSignal<std::string> sig;
    GObject              context;
    context.moveToThread(&workerThread);

    std::string receivedMsg;
    GThread*    executedInThread = nullptr;

    GObject::connect(
        sig,
        &context,
        [&receivedMsg, &executedInThread, &workerThread](std::string msg)
        {
            receivedMsg      = msg;
            executedInThread = GThread::currentThread();
            workerThread.quit();
        },
        G::AutoConnection);

    sig.emit("hello cross thread");

    workerThread.wait();

    EXPECT_EQ(receivedMsg, "hello cross thread");
    EXPECT_EQ(executedInThread, &workerThread);
}

/**
 * @brief Tests signal emission when receiver is destroyed before event processing.
 *
 * Verifies that when a receiver object connected via G::QueuedConnection is destroyed prior to
 * the event dispatcher processing the pending GMetaCallEvent, the event is safely removed/ignored
 * and no slot function is invoked or use-after-free error occurs.
 */
TEST(GObjectTest, ReceiverDestroyedBeforeQueuedEventHandled)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

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

    GSignal<int> sig;

    {
        auto receiver = std::make_unique<GObjectTestReceiver>();
        receiver->moveToThread(&workerThread);

        GObject::connect(
            sig, receiver.get(), &GObjectTestReceiver::onValueReceived, G::QueuedConnection);

        sig.emit(55);
        // Receiver is deleted here before workerThread event loop processes the queued event
    }

    blockReleasePromise.set_value();

    // Make another event to make sure the `emit(55)` is processed before the thread terminates.
    GSignal<> quitSig;
    GObject::connect(
        quitSig, &dummyContext, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_TRUE(workerThread.isFinished());
}

/**
 * @brief Tests lambda slot execution when context object is destroyed before event handling.
 *
 * Verifies that when a context GObject associated with a lambda connection is destroyed before
 * the queued metacall is processed, the lambda slot is not executed.
 */
TEST(GObjectTest, ContextDestroyedBeforeQueuedLambdaHandled)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    std::promise<void> blockEnteredPromise;
    std::promise<void> blockReleasePromise;
    auto blockEnteredFuture = blockEnteredPromise.get_future();
    auto blockReleaseFuture = blockReleasePromise.get_future();

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

    GSignal<int> sig;
    bool         lambdaExecuted = false;

    {
        GObject context;
        context.moveToThread(&workerThread);

        GObject::connect(
            sig, &context, [&lambdaExecuted](int) { lambdaExecuted = true; }, G::QueuedConnection);

        sig.emit(77);
        // context goes out of scope and is destroyed here
    }

    blockReleasePromise.set_value();

    // Make another event to make sure the `emit(77)` is processed before the thread terminates.
    GSignal<> quitSig;
    GObject::connect(
        quitSig, &dummyContext, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_FALSE(lambdaExecuted);
}

/**
 * @brief Tests connect() with explicit G::DirectConnection when receiver lives in another thread.
 *
 * Verifies that when G::DirectConnection is explicitly requested, the slot function is invoked
 * synchronously on the emitting thread, even if the receiver belongs to a different thread.
 */
TEST(GObjectTest, CrossThreadDirectConnection)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GSignal<int>        sig;
    GObjectTestReceiver receiver;
    receiver.moveToThread(&workerThread);

    GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueReceived, G::DirectConnection);

    sig.emit(888);

    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 888);
    EXPECT_EQ(receiver.executedThread(), GThread::currentThread());
    EXPECT_NE(receiver.executedThread(), &workerThread);

    workerThread.quit();
    workerThread.wait();
}

static int g_testFreeFuncCount   = 0;
static int g_testFreeFuncLastVal = 0;

/**
 * @brief Free function used for testing GObject::callLater free function overload.
 * @param val Received test value.
 */
static void testCallLaterFreeFunc(int val)
{
    g_testFreeFuncLastVal = val;
    g_testFreeFuncCount++;
}

/**
 * @brief Tests GObject::callLater with a member function slot.
 */
TEST(GObjectTest, CallLaterMemberFunction)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GObjectTestReceiver receiver;
    receiver.moveToThread(&workerThread);

    GObject::callLater(&receiver, &GObjectTestReceiver::onValueReceived, 42);

    GSignal<> quitSig;
    GObject::connect(
        quitSig, &receiver, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 42);
    EXPECT_EQ(receiver.executedThread(), &workerThread);
}

/**
 * @brief Tests GObject::callLater deduplication and parameter overwriting.
 *
 * Verifies that invoking callLater multiple times in the same cycle collapses to a single
 * execution using the arguments of the last call.
 */
TEST(GObjectTest, CallLaterDeduplicationAndLastArgs)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GObjectTestReceiver receiver;
    receiver.moveToThread(&workerThread);

    // Pause workerThread's event processing so all callLater invocations land in the same event
    // loop cycle
    std::mutex              blockMutex;
    std::condition_variable blockCv;
    bool                    canProceed = false;

    GSignal<> blockSig;
    GObject::connect(
        blockSig,
        &receiver,
        [&blockMutex, &blockCv, &canProceed]()
        {
            std::unique_lock<std::mutex> lock(blockMutex);
            blockCv.wait(lock, [&canProceed]() { return canProceed; });
        },
        G::QueuedConnection);

    blockSig.emit();

    // Ensure workerThread has entered the block slot before issuing callLater
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    GObject::callLater(&receiver, &GObjectTestReceiver::onValueReceived, 10);
    GObject::callLater(&receiver, &GObjectTestReceiver::onValueReceived, 20);
    GObject::callLater(&receiver, &GObjectTestReceiver::onValueReceived, 30);

    // Release workerThread to process pending event loop queue
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        canProceed = true;
    }
    blockCv.notify_all();

    GSignal<> quitSig;
    GObject::connect(
        quitSig, &receiver, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 30);
}


/**
 * @brief Tests GObject::callLater with a free function.
 */
TEST(GObjectTest, CallLaterFreeFunction)
{
    g_testFreeFuncCount   = 0;
    g_testFreeFuncLastVal = 0;

    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GObject context;
    context.moveToThread(&workerThread);

    GObject::callLater(&context, &testCallLaterFreeFunc, 99);

    GSignal<> quitSig;
    GObject::connect(
        quitSig, &context, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ(g_testFreeFuncCount, 1);
    EXPECT_EQ(g_testFreeFuncLastVal, 99);
}

/**
 * @brief Tests GObject::callLater with a GSignal instance.
 */
TEST(GObjectTest, CallLaterSignal)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GObjectTestReceiver receiver;
    receiver.moveToThread(&workerThread);

    GSignal<int> sig;
    GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueReceived, G::DirectConnection);

    GObject::callLater(&receiver, sig, 777);

    GSignal<> quitSig;
    GObject::connect(
        quitSig, &receiver, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 777);
}

/**
 * @brief Tests GObject::callLater execution across multiple event loop cycles.
 */
TEST(GObjectTest, CallLaterMultipleCycles)
{
    GThread workerThread;
    workerThread.start();
    while (!workerThread.eventDispatcher())
    {
        std::this_thread::yield();
    }

    GObjectTestReceiver receiver;
    receiver.moveToThread(&workerThread);

    // Cycle 1
    GObject::callLater(&receiver, &GObjectTestReceiver::onValueReceived, 100);

    GSignal<> syncSig1;
    bool      sync1Done = false;
    GObject::connect(
        syncSig1, &receiver, [&sync1Done]() { sync1Done = true; }, G::QueuedConnection);
    syncSig1.emit();

    while (!sync1Done)
    {
        std::this_thread::yield();
    }

    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 100);

    // Cycle 2
    GObject::callLater(&receiver, &GObjectTestReceiver::onValueReceived, 200);

    GSignal<> quitSig;
    GObject::connect(
        quitSig, &receiver, [&workerThread]() { workerThread.quit(); }, G::QueuedConnection);
    quitSig.emit();

    workerThread.wait();

    EXPECT_EQ(receiver.callCount(), 2);
    EXPECT_EQ(receiver.lastValue(), 200);
}

/**
 * @brief Tests GObject::connect with overloaded slots.
 */
TEST(GObjectTest, ConnectOverloadedSlot)
{
    GSignal<int, int>   sig;
    GObjectTestReceiver receiver;

    // onValueReceived has two overload 1-arg and 2-arg. Try if we can connect to the 2-arg one.
    GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueReceived);

    sig.emit(3, 4);
    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 7);
}

/**
 * @brief Tests GObject::connect with non-void-return slot.
 */
TEST(GObjectTest, ConneectNonVoidReturnSlot)
{
    GSignal<int>        sig;
    GObjectTestReceiver receiver;

    GObject::connect(sig, &receiver, &GObjectTestReceiver::onValueNonVoidReturn);

    sig.emit(42);
    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastValue(), 42);
}

/**
 * @brief Tests GObject::connect with const reference argument slot.
 */
TEST(GObjectTest, ConnectConstReferenceSlot)
{
    GSignal<std::string> sig;
    GObjectTestReceiver  receiver;

    GObject::connect(sig, &receiver, &GObjectTestReceiver::onStringConstReference);

    sig.emit("Hello, GObject!");
    EXPECT_EQ(receiver.callCount(), 1);
    EXPECT_EQ(receiver.lastString(), "Hello, GObject!");
}
