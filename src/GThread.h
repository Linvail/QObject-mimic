#ifndef GTHREAD_H
#define GTHREAD_H

#include "GObject.h"
#include "GSignal.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class GAbstractEventDispatcher;

/**
 * @brief Manages a platform execution thread with an event loop.
 */
class GThread : public GObject
{
public:
    /**
     * @brief Constructs a new thread object.
     */
    GThread();

    /**
     * @brief Destroys the thread, waiting for it to finish if running.
     */
    virtual ~GThread() override;

    /**
     * @brief Scheduling priority of a thread, mirroring QThread::Priority.
     *
     * The numeric order is load-bearing, not cosmetic: the UNIX backend scales these values
     * arithmetically onto whatever range the platform scheduler reports, so IdlePriority must
     * stay lowest and TimeCriticalPriority highest, with no gaps introduced between them.
     *
     * InheritPriority means "whatever the thread that called start() was running at". It is the
     * state a thread begins in and is reported for a thread that is not running. start() accepts
     * it -- it is the default -- but setPriority() does not, because once a thread is running
     * there is no operation that corresponds to re-inheriting.
     */

    enum Priority
    {
        IdlePriority,

        LowestPriority,
        LowPriority,
        NormalPriority,
        HighPriority,
        HighestPriority,

        TimeCriticalPriority,

        InheritPriority
    };
    /**
     * @brief Starts execution of the thread by invoking run(). Thread-safe.
     *
     * The new thread applies @p priority to itself as its first action, before started() is
     * emitted and before run() is entered, so the whole of run() executes at the requested
     * priority.
     *
     * It is not applied quite as early as Qt manages, and the difference is worth knowing if you
     * are relying on priority for correctness rather than tuning. Qt creates the thread suspended
     * on Windows, and passes the priority in pthread_attr_t on UNIX, so the thread never executes
     * a single instruction at the wrong priority. std::thread offers neither, so between the OS
     * creating the thread and the thread's first instruction it briefly runs at the creating
     * thread's priority. Nothing belonging to this class runs in that window.
     * @param priority Priority for the new thread. InheritPriority, the default, keeps the
     * creating thread's priority and preserves the behaviour of the no-argument call.
     */
    void start
        (
        Priority priority = InheritPriority
        );

    /**
     * @brief Requests the thread's event loop to quit with return code 0. Thread-safe.
     */
    void quit();

    /**
     * @brief Requests the thread's event loop to exit with specified return code. Thread-safe.
     * @param returnCode Exit return code.
     */
    void exit
        (
        int returnCode = 0
        );

    /**
     * @brief Blocks until the thread has finished executing or timeout expires. Thread-safe.
     * @param time Maximum time to wait in milliseconds.
     * @return True if thread finished, false if timeout occurred.
     */
    bool wait
        (
        unsigned long time = ULONG_MAX
        );

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
     * @brief Sets the scheduling priority of this thread. Thread-safe.
     *
     * Only meaningful while the thread is running, as in Qt: there is no OS thread to act on
     * before start(), and the value is deliberately not remembered for a later start() either.
     * A call made when the thread is not running is rejected with a warning and changes nothing,
     * so priority() will still report InheritPriority afterwards. To give a thread a priority
     * from the outset, pass one to start() instead.
     *
     * Rejected with a warning if @p priority is InheritPriority.
     *
     * What the OS does with the request varies, and a successful call does not promise the
     * thread's scheduling actually changed. On Linux the default SCHED_OTHER policy reports a
     * priority range of exactly one value, so every priority maps onto the same number and the
     * call is accepted but has no effect; real prioritisation there needs a realtime policy and
     * the privileges to select it. Qt behaves the same way. Windows applies all seven levels.
     * @param priority The priority to apply. InheritPriority is not accepted.
     */
    void setPriority
        (
        Priority priority
        );

    /**
     * @brief Gets the scheduling priority of this thread. Thread-safe.
     * @return The priority last set on the running thread, or InheritPriority if the thread is
     * not running or no priority has been set on this run.
     */
    Priority priority() const;

    /**
     * @brief Gets a pointer to the thread currently executing. Thread-safe.
     * @return Pointer to current thread.
     */
    static GThread* currentThread();

    /**
     * @brief Gets the event dispatcher for this thread. Thread-safe.
     *
     * Read-only by design. There is deliberately no setter: swapping a running thread's
     * dispatcher raced against that thread's own start/finish lifecycle, which could delete a
     * dispatcher an active exec()/processEvents() loop was still calling into. A thread creates
     * and owns its dispatcher in start(); GCoreApplication supplies the main thread's.
     *
     * Returns a strong reference rather than a raw pointer, so the dispatcher cannot be destroyed
     * by its owning thread finishing while the caller is still using it.
     * @return Shared pointer to the event dispatcher, or nullptr before start(). Thread-safe.
     */
    std::shared_ptr<GAbstractEventDispatcher> eventDispatcher() const;

    /**
     * @brief Queues an arbitrary task to run on this thread's event loop.
     *
     * Always deferred to a later iteration of this thread's loop -- never run inline, even when
     * post() is called from this thread itself. Implemented as a thin wrapper over
     * GObject::dispatchMetaCall() targeting this GThread as both context and receiver, so it goes
     * through the exact same queue, GMetaCallEvent, and lifetime handling as every other queued
     * call in this library (removeEventsForReceiver() on destruction, processDeferredDeletes() on
     * shutdown, etc.) rather than a second, parallel task queue.
     * @param task The callable to run on this thread. Ignored (returns false) if empty.
     * @return True if the task was queued; false if this thread has no dispatcher yet (before
     * start()/exec(), or after it has fully finished and released it), in which case the task is
     * dropped rather than run.
     */
    bool post
        (
        std::function<void()> task
        );

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
    template <typename Function, typename ... Args>
    static GThread* create( Function&& f, Args&&... args );

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

private:
    /**
     * @brief Gets the thread's internal data container holding the event dispatcher.
     *
     * Private for the same reason as GObject::threadData(): it is the handle onto the dispatcher
     * plumbing, not API. GObject reaches it when adopting a thread's affinity.
     * @return Shared pointer to the thread data.
     */
    std::shared_ptr<GThreadData> threadData() const
    {
        return m_data;
    }

    /**
     * @brief Pushes a priority down to the OS thread.
     *
     * Split out from setPriority() only so the platform code sits in one place. The caller must
     * hold m_priorityMutex and must already have established that the thread is running, because
     * this dereferences m_thread and uses its native handle.
     * @param priority The priority to apply. Never InheritPriority.
     */
    void applyPriority
        (
        Priority priority
        );

    std::unique_ptr<std::thread> m_thread;
    std::shared_ptr<GThreadData> m_data;
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_finished { false };
    std::atomic<bool> m_exiting { false };
    std::atomic<int> m_exitCode { 0 };
    mutable std::mutex m_waitMutex;
    std::condition_variable m_waitCv;

    /**
     * @brief Guards m_priority and every use of m_thread's native handle.
     *
     * Not merely protecting the enum. The run body clears m_running while holding this mutex, so
     * a setPriority() that has observed m_running == true under the same lock is guaranteed the
     * OS thread has not yet reached the end of its body -- without that, the handle could be
     * touched after the thread had exited.
     */
    mutable std::mutex m_priorityMutex;
    Priority m_priority { InheritPriority };

    static thread_local GThread* s_currentThread;
    friend class GCoreApplication;
    /** @brief Grants GObject access to threadData() when adopting or releasing thread affinity. */
    friend class GObject;
};

template <typename Function, typename ... Args>
GThread* GThread::create
    (
    Function&& f,
    Args&&... args
    )
{
    auto task = std::bind( std::forward<Function>( f ), std::forward<Args>( args )... );

    class GFuncThread : public GThread
    {
    public:
        GFuncThread
            (
            std::function<void()> fn
            )
            : m_fn( std::move( fn ) )
        {
        }

    protected:
        virtual void run() override
        {
            if( m_fn )
            {
                m_fn();
            }
        }

    private:
        std::function<void()> m_fn;
    };

    auto* threadObj = new GFuncThread( task );
    threadObj->start();
    return threadObj;
}

#endif // GTHREAD_H
