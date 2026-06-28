#include <iostream>
#include <chrono>
#include <boost/signals2.hpp>
#include "GCoreApplication.h"
#include "GThread.h"
#include "GObject.h"
#include "GSignal.h"

class WorkerThread : public GThread {
public:
    GSignal<int, std::string> dataReady;

protected:
    void run() override {
        std::cout << "WorkerThread running in thread ID: " << std::this_thread::get_id() << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::cout << "WorkerThread emitting dataReady signal." << std::endl;
        dataReady.emit(42, "Hello from Worker");
    }
};

class Receiver : public GObject {
public:
    Receiver(const std::string& name) : m_name(name) {
        std::cout << "Receiver '" << m_name << "' created in thread ID: " << std::this_thread::get_id() << std::endl;
    }

    ~Receiver() override {
        std::cout << "Receiver '" << m_name << "' destroyed!" << std::endl;
    }

    void onDataReady(int value, std::string message) {
        std::cout << "Receiver '" << m_name << "' onDataReady executed in thread ID: " << std::this_thread::get_id() << std::endl;
        std::cout << "Received value: " << value << ", message: " << message << std::endl;

        GCoreApplication::instance()->quit();
    }

private:
    std::string m_name;
};

/**
 * @brief Test receiver class for verifying signal disconnection.
 */
class DisconnectTestReceiver : public GObject {
public:
    /**
     * @brief Constructs a new DisconnectTestReceiver.
     */
    DisconnectTestReceiver() : m_called(false) {}

    /**
     * @brief Slot called when the signal is emitted.
     */
    void onSignal() {
        m_called = true;
    }

    /**
     * @brief Checks if the slot was called.
     * @return True if called, false otherwise.
     */
    bool wasCalled() const {
        return m_called;
    }

private:
    bool m_called;
};

int main(int argc, char** argv) {
    std::cout << "Main thread ID: " << std::this_thread::get_id() << std::endl;

    // Test Boost.Signals2 compilation and execution
    {
        std::cout << "Testing Boost.Signals2 compilation..." << std::endl;
        boost::signals2::signal<void()> sig;
        sig.connect([]() {
            std::cout << "  Boost.Signals2 slot invoked successfully!" << std::endl;
        });
        sig();
    }

    // Test context-based functor/lambda connect
    {
        std::cout << "Testing context-based functor connect (Direct)..." << std::endl;
        GSignal<int> sig;
        {
            GObject context;
            int receivedValue = 0;
            GObject::connect(sig, &context, [&receivedValue](int val) {
                receivedValue = val;
            }, G::DirectConnection);

            sig.emit(42);
            if (receivedValue != 42) {
                std::cerr << "FAIL: Direct functor connect did not execute or receive value!" << std::endl;
                return 1;
            }
            std::cout << "  Direct functor connect passed." << std::endl;
        } // context is destroyed here

        sig.emit(99); // Should not crash and not call
        std::cout << "  Functor connect with destroyed context passed (no crash)." << std::endl;
    }

    // Test manual disconnect
    {
        DisconnectTestReceiver testReceiver;
        GSignal<> testSignal;
        G::ConnectionHandle handle = GObject::connect(testSignal, &testReceiver, &DisconnectTestReceiver::onSignal);

        std::cout << "Emitting testSignal before disconnect..." << std::endl;
        testSignal.emit();
        if (!testReceiver.wasCalled()) {
            std::cerr << "FAIL: slot not called before disconnect" << std::endl;
            return 1;
        }

        std::cout << "Disconnecting testSignal..." << std::endl;
        testSignal.disconnect(handle);

        DisconnectTestReceiver testReceiver2;
        G::ConnectionHandle handle2 = GObject::connect(testSignal, &testReceiver2, &DisconnectTestReceiver::onSignal);
        testSignal.disconnect(handle2);

        std::cout << "Emitting testSignal after disconnect..." << std::endl;
        testSignal.emit();
        if (testReceiver2.wasCalled()) {
            std::cerr << "FAIL: slot called after disconnect" << std::endl;
            return 1;
        }
        std::cout << "SUCCESS: disconnect test passed!" << std::endl;
    }

    GCoreApplication app(argc, argv);

    WorkerThread worker;

    // Main long-lived receiver
    Receiver receiver("LongLived");

    // Short-lived receiver that will be destroyed before the signal is emitted
    auto* temporaryReceiver = new Receiver("Temporary");

    // Modern syntax: GObject::connect(sender.signal, receiver, slot)
    bool lambdaCalled = false;
    GObject::connect(worker.dataReady, &receiver, [&lambdaCalled](int value, std::string message) {
        std::cout << "Lambda slot with receiver context executed in thread ID: " << std::this_thread::get_id() << std::endl;
        std::cout << "Received value: " << value << ", message: " << message << std::endl;
        lambdaCalled = true;
    });

    GObject::connect(worker.dataReady, &receiver, &Receiver::onDataReady);
    GObject::connect(worker.dataReady, temporaryReceiver, &Receiver::onDataReady);

    // Destroy the temporary receiver immediately!
    // The signal should NOT invoke temporaryReceiver's slot because the connection
    // should be automatically removed via the new cleanup mechanism.
    delete temporaryReceiver;

    worker.start();

    std::cout << "Starting main event loop..." << std::endl;
    app.exec();

    std::cout << "Main event loop ended." << std::endl;

    if (!lambdaCalled) {
        std::cerr << "FAIL: Queued functor connection was not executed!" << std::endl;
        return 1;
    }
    std::cout << "SUCCESS: Queued functor connection test passed!" << std::endl;

    worker.wait();

    return 0;
}
