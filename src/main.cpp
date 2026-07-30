#include <iostream>
#include <chrono>
#include <boost/signals2.hpp>
#include "GCoreApplication.h"
#include "GThread.h"
#include "GObject.h"
#include "GSignal.h"
#include "GTimer.h"

/**
 * @brief Test event filter class to verify event interception.
 */
class TestEventFilter : public GObject
{
public:
    /**
     * @brief Constructs a new TestEventFilter.
     */
    TestEventFilter()
    : m_filteredCount(0)
    {
    }

    /**
     * @brief Intercepts events sent to watched object.
     * @param watched Watched target object.
     * @param event The event being dispatched.
     * @return True if event is consumed, false otherwise.
     */
    virtual bool eventFilter(GObject* watched, GEvent* event) override
    {
        (void) watched;
        if (event && event->type() == GEvent::User)
        {
            m_filteredCount++;
            std::cout << "  TestEventFilter intercepted User event!" << std::endl;
            return true;  // Consume event
        }
        return false;
    }

    /**
     * @brief Gets total count of intercepted events.
     * @return Filtered count.
     */
    int filteredCount() const { return m_filteredCount; }

private:
    int m_filteredCount;
};

/**
 * @brief Worker thread for testing signal emission and thread lifecycle.
 */
class WorkerThread : public GThread
{
public:
    GSignal<int, std::string> dataReady;

protected:
    virtual void run() override
    {
        std::cout << "WorkerThread running in thread ID: " << std::this_thread::get_id()
                  << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::cout << "WorkerThread emitting dataReady signal." << std::endl;
        dataReady.emit(42, "Hello from Worker");
    }
};

/**
 * @brief Receiver object for signal-slot testing.
 */
class Receiver : public GObject
{
public:
    /**
     * @brief Constructs a Receiver with a name.
     * @param name Descriptive name.
     */
    Receiver(const std::string& name)
    : m_name(name)
    {
        std::cout << "Receiver '" << m_name
                  << "' created in thread ID: " << std::this_thread::get_id() << std::endl;
    }

    /**
     * @brief Destructor.
     */
    virtual ~Receiver() override
    {
        std::cout << "Receiver '" << m_name << "' destroyed!" << std::endl;
    }

    /**
     * @brief Slot for data ready signal.
     * @param value Received integer.
     * @param message Received string.
     */
    void onDataReady(int value, std::string message)
    {
        std::cout << "Receiver '" << m_name
                  << "' onDataReady executed in thread ID: " << std::this_thread::get_id()
                  << std::endl;
        std::cout << "Received value: " << value << ", message: " << message << std::endl;
    }

private:
    std::string m_name;
};

/**
 * @brief Receiver class for testing disconnection.
 */
class DisconnectTestReceiver : public GObject
{
public:
    /**
     * @brief Constructs DisconnectTestReceiver.
     */
    DisconnectTestReceiver()
    : m_called(false)
    {
    }

    /**
     * @brief Slot called when signal fires.
     */
    void onSignal() { m_called = true; }

    /**
     * @brief Checks if slot was called.
     * @return True if called.
     */
    bool wasCalled() const { return m_called; }

private:
    bool m_called;
};

/**
 * @brief Parent class for testing hierarchy member function connections.
 */
class ParentClass : public GObject
{
public:
    ParentClass()
    : m_parentSlotCalled(false)
    {
    }

    void parentSlot(int value)
    {
        std::cout << "  ParentClass::parentSlot called with value: " << value << std::endl;
        m_parentSlotCalled = true;
    }

    bool parentSlotCalled() const { return m_parentSlotCalled; }

private:
    bool m_parentSlotCalled;
};

/**
 * @brief Derived class for testing hierarchy member function connections.
 */
class DerivedClass : public ParentClass
{
public:
    DerivedClass()
    : m_derivedSlotCalled(false)
    {
    }

    void derivedSlot(int value)
    {
        std::cout << "  DerivedClass::derivedSlot called with value: " << value << std::endl;
        m_derivedSlotCalled = true;
    }

    bool derivedSlotCalled() const { return m_derivedSlotCalled; }

private:
    bool m_derivedSlotCalled;
};

int main(int argc, char** argv)
{
    std::cout << "Main thread ID: " << std::this_thread::get_id() << std::endl;

    // 1. Test Boost.Signals2
    {
        std::cout << "[Test 1] Testing Boost.Signals2 compilation..." << std::endl;
        boost::signals2::signal<void()> sig;
        sig.connect([]()
                    { std::cout << "  Boost.Signals2 slot invoked successfully!" << std::endl; });
        sig();
    }

    // 2. Test Direct Functor Connect
    {
        std::cout << "[Test 2] Testing context-based functor connect (Direct)..." << std::endl;
        GSignal<int> sig;
        {
            GObject context;
            int     receivedValue = 0;
            GObject::connect(
                sig,
                &context,
                [&receivedValue](int val) { receivedValue = val; },
                G::DirectConnection);

            sig.emit(42);
            if (receivedValue != 42)
            {
                std::cerr << "FAIL: Direct functor connect did not receive value!" << std::endl;
                return 1;
            }
            std::cout << "  Direct functor connect passed." << std::endl;
        }

        sig.emit(99);
        std::cout << "  Functor connect with destroyed context passed." << std::endl;
    }

    // 3. Test Disconnect
    {
        std::cout << "[Test 3] Testing signal disconnection..." << std::endl;
        DisconnectTestReceiver testReceiver;
        GSignal<>              testSignal;
        G::ConnectionHandle    handle
            = GObject::connect(testSignal, &testReceiver, &DisconnectTestReceiver::onSignal);

        testSignal.emit();
        if (!testReceiver.wasCalled())
        {
            std::cerr << "FAIL: slot not called before disconnect" << std::endl;
            return 1;
        }

        testSignal.disconnect(handle);
        DisconnectTestReceiver testReceiver2;
        G::ConnectionHandle    handle2
            = GObject::connect(testSignal, &testReceiver2, &DisconnectTestReceiver::onSignal);
        testSignal.disconnect(handle2);

        testSignal.emit();
        if (testReceiver2.wasCalled())
        {
            std::cerr << "FAIL: slot called after disconnect" << std::endl;
            return 1;
        }
        std::cout << "  Disconnect test passed!" << std::endl;
    }

    // 4. Test Derived/Parent Slot Connections
    {
        std::cout
            << "[Test 4] Testing connect with DerivedClass pointer & &ParentClass::parentSlot..."
            << std::endl;
        DerivedClass derivedObj;
        GSignal<int> sig;

        GObject::connect(sig, &derivedObj, &ParentClass::parentSlot);
        GObject::connect(sig, &derivedObj, &DerivedClass::derivedSlot);

        sig.emit(100);

        if (!derivedObj.parentSlotCalled() || !derivedObj.derivedSlotCalled())
        {
            std::cerr << "FAIL: Inheritance member function slot connection failed!" << std::endl;
            return 1;
        }
        std::cout << "  Parent/Derived member function connect test passed!" << std::endl;
    }

    // 5. Initialize GCoreApplication & Event Loop Tests
    GCoreApplication app(argc, argv);

    // Test Event Filter
    TestEventFilter filter;
    GObject         targetObject;
    targetObject.installEventFilter(&filter);

    GEvent userEv(GEvent::User);
    targetObject.event(&userEv);
    if (filter.filteredCount() != 1)
    {
        std::cerr << "FAIL: Event filter did not intercept event!" << std::endl;
        return 1;
    }
    std::cout << "[Test 5] Event filter test passed!" << std::endl;

    // Test GTimer
    bool   timerFired = false;
    GTimer timer;
    timer.setInterval(50);
    timer.setSingleShot(true);
    GObject::connect(timer.timeout,
                     &targetObject,
                     [&timerFired]()
                     {
                         std::cout << "  GTimer singleShot fired!" << std::endl;
                         timerFired = true;
                     });
    timer.start();

    // Test GTimer::singleShot static
    bool staticSingleShotFired = false;
    GTimer::singleShot(100,
                       &targetObject,
                       [&staticSingleShotFired]()
                       {
                           std::cout << "  GTimer::singleShot static helper fired!" << std::endl;
                           staticSingleShotFired = true;
                       });

    // Test WorkerThread & GThread signals
    WorkerThread worker;
    bool         threadStartedFired  = false;
    bool         threadFinishedFired = false;

    GObject::connect(worker.started,
                     &targetObject,
                     [&threadStartedFired]()
                     {
                         std::cout << "  GThread::started signal received." << std::endl;
                         threadStartedFired = true;
                     });

    GObject::connect(worker.finished,
                     &targetObject,
                     [&threadFinishedFired]()
                     {
                         std::cout << "  GThread::finished signal received." << std::endl;
                         threadFinishedFired = true;
                     });

    Receiver receiver("MainReceiver");
    bool     lambdaCalled = false;
    GObject::connect(worker.dataReady,
                     &receiver,
                     [&lambdaCalled](int value, std::string message)
                     {
                         std::cout << "  Queued dataReady lambda executed (value=" << value
                                   << ", msg=" << message << ")" << std::endl;
                         lambdaCalled = true;
                     });

    GObject::connect(worker.dataReady, &receiver, &Receiver::onDataReady);

    worker.start();

    // Schedule application exit after 500ms
    GTimer::singleShot(500,
                       [&app]()
                       {
                           std::cout << "Quitting main event loop..." << std::endl;
                           app.quit();
                       });

    std::cout << "Starting main event loop..." << std::endl;
    app.exec();
    std::cout << "Main event loop ended." << std::endl;

    worker.wait();

    if (!timerFired)
    {
        std::cerr << "FAIL: GTimer did not fire!" << std::endl;
        return 1;
    }

    if (!staticSingleShotFired)
    {
        std::cerr << "FAIL: GTimer::singleShot did not fire!" << std::endl;
        return 1;
    }

    if (!lambdaCalled)
    {
        std::cerr << "FAIL: Queued functor connection was not executed!" << std::endl;
        return 1;
    }

    if (!threadStartedFired || !threadFinishedFired)
    {
        std::cerr << "FAIL: GThread started/finished signals were not emitted!" << std::endl;
        return 1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "ALL TESTS PASSED SUCCESSFULLY!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
