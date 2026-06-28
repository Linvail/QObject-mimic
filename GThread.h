#pragma once

#include "GObject.h"
#include <thread>
#include <memory>
#include <atomic>

class GAbstractEventDispatcher;

/**
 * @brief Represents a thread of execution in the application.
 */
class GThread : public GObject {
public:
    /**
     * @brief Constructs a new thread object.
     * @param parent The parent object.
     */
    GThread(GObject* parent = nullptr);

    /**
     * @brief Destroys the thread, waiting for it to finish if it's still running.
     */
    virtual ~GThread();

    /**
     * @brief Starts execution of the thread by invoking run().
     */
    void start();

    /**
     * @brief Requests the thread's event loop to quit.
     */
    void quit();

    /**
     * @brief Blocks until the thread has finished executing.
     */
    void wait();

    /**
     * @brief Gets a pointer to the thread currently executing.
     * @return A pointer to the current thread.
     */
    static GThread* currentThread();

    /**
     * @brief Gets the event dispatcher for this thread.
     * @return The event dispatcher.
     */
    GAbstractEventDispatcher* eventDispatcher() const;

protected:
    /**
     * @brief The starting point for the thread. Can be overridden.
     * By default, it calls exec() to start the event loop.
     */
    virtual void run();

    /**
     * @brief Enters the event loop and waits until quit() is called.
     * @return The exit code.
     */
    int exec();

private:
    std::unique_ptr<std::thread> m_thread;
    std::atomic<GAbstractEventDispatcher*> m_dispatcher{nullptr};

    static thread_local GThread* s_currentThread;
    friend class GCoreApplication;
};
