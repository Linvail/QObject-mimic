#ifndef GTHREAD_H
#define GTHREAD_H

#include "GObject.h"
#include "GSignal.h"
#include <thread>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <climits>
#include <functional>

class GAbstractEventDispatcher;

/**
 * @brief Manages a platform execution thread with an event loop.
 */
class GThread : public GObject
{
public:
    /**
     * @brief Constructs a new thread object.
     * @param parent The parent object.
     */
    GThread(GObject* parent = nullptr);

    /**
     * @brief Destroys the thread, waiting for it to finish if running.
     */
    virtual ~GThread() override;

    /**
     * @brief Starts execution of the thread by invoking run(). Thread-safe.
     */
    void start();

    /**
     * @brief Requests the thread's event loop to quit with return code 0. Thread-safe.
     */
    void quit();

    /**
     * @brief Requests the thread's event loop to exit with specified return code. Thread-safe.
     * @param returnCode Exit return code.
     */
    void exit(int returnCode = 0);

    /**
     * @brief Blocks until the thread has finished executing or timeout expires. Thread-safe.
     * @param time Maximum time to wait in milliseconds.
     * @return True if thread finished, false if timeout occurred.
     */
    bool wait(unsigned long time = ULONG_MAX);

    /**
     * @brief Checks if the thread is currently running. Thread-safe.
     * @return True if running.
     */
    bool isRunning() const;

    /**
     * @brief Checks if the thread has finished execution. Thread-safe.
     * @return True if finished.
     */
    bool isFinished() const;

    /**
     * @brief Gets a pointer to the thread currently executing. Thread-safe.
     * @return Pointer to current thread.
     */
    static GThread* currentThread();

    /**
     * @brief Gets the event dispatcher for this thread. Thread-safe.
     * @return Pointer to event dispatcher.
     */
    GAbstractEventDispatcher* eventDispatcher() const;

    /**
     * @brief Sets a custom event dispatcher for this thread. Thread-safe.
     * @param dispatcher Pointer to the new dispatcher.
     */
    void setEventDispatcher(GAbstractEventDispatcher* dispatcher);

    /**
     * @brief Signal emitted when the thread starts running.
     */
    GSignal<> started;

    /**
     * @brief Signal emitted when the thread finishes execution.
     */
    GSignal<> finished;

    /**
     * @brief Creates and starts a GThread executing the specified function.
     * @tparam Function Callable type.
     * @tparam Args Argument types.
     * @param f Function to execute.
     * @param args Arguments to pass.
     * @return Pointer to the newly created GThread. Thread-safe.
     */
    template<typename Function, typename... Args>
    static GThread* create(Function&& f, Args&&... args);

protected:
    /**
     * @brief Starting point for thread execution. Can be overridden. Default calls exec().
     */
    virtual void run();

    /**
     * @brief Enters the event loop and waits until exit() is called.
     * @return Exit code.
     */
    int exec();

public:
    /**
     * @brief Gets the thread's internal data container holding the event dispatcher.
     * @return Shared pointer to the thread data.
     */
    std::shared_ptr<GThreadData> threadData() const { return m_data; }

private:
    std::unique_ptr<std::thread>           m_thread;
    std::shared_ptr<GThreadData>           m_data;
    std::atomic<bool>                      m_running{ false };
    std::atomic<bool>                      m_finished{ false };
    std::atomic<bool>                      m_exiting{ false };
    std::atomic<int>                       m_exitCode{ 0 };
    mutable std::mutex                     m_waitMutex;
    std::condition_variable                m_waitCv;

    static thread_local GThread* s_currentThread;
    friend class GCoreApplication;
};

template<typename Function, typename... Args>
GThread* GThread::create(Function&& f, Args&&... args)
{
    auto task = std::bind(std::forward<Function>(f), std::forward<Args>(args)...);

    class GFuncThread : public GThread
    {
    public:
        GFuncThread(std::function<void()> fn)
        : m_fn(std::move(fn))
        {
        }

    protected:
        virtual void run() override
        {
            if (m_fn)
            {
                m_fn();
            }
        }

    private:
        std::function<void()> m_fn;
    };

    auto* threadObj = new GFuncThread(task);
    threadObj->start();
    return threadObj;
}

#endif // GTHREAD_H
