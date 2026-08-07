#include "GThread.h"

#include "GEventDispatcherDefault.h"
#if defined( _WIN32 )
    #include "GEventDispatcherWin32.h"
#elif defined( __linux__ )
    #include "GEventDispatcherLinux.h"
#endif

#include <cstdio>

#if defined( _WIN32 )
    #include <windows.h>
#else
    #include <cerrno>
    #include <pthread.h>
    #include <sched.h>
    #include <unistd.h>
    // Priority scheduling is an optional part of POSIX. Where it is absent there is no portable
    // way to ask for a priority at all, so setPriority() records the value and does nothing else,
    // which is what Qt does behind its own QT_HAS_THREAD_PRIORITY_SCHEDULING guard.
    #if defined( _POSIX_THREAD_PRIORITY_SCHEDULING )
        #define G_HAS_THREAD_PRIORITY_SCHEDULING
    #endif
#endif

namespace QtLikeSignal
{
    thread_local GThread* GThread::s_currentThread = nullptr;

    #if !defined( _WIN32 ) && defined( G_HAS_THREAD_PRIORITY_SCHEDULING )

        namespace
        {

            //! Maps a GThread priority onto a scheduler policy and priority number.
            //!
            //! This is Qt's mapping from qthread_unix.cpp, including its deliberately coarse scaling: the
            //! divisor is TimeCriticalPriority rather than the span between the lowest and highest values, so
            //! the enum lands on the low end of the platform's range rather than spreading across it. Kept as
            //! Qt has it so behaviour matches; the alternative would be a library that claims to mimic QThread
            //! and then schedules differently. Returns true if a priority could be calculated; false if the
            //! platform would not report a range.
            bool calculateUnixPriority
                (
                int priority,          //!< The GThread priority to convert.
                int* schedPolicy,      //!< In: the thread's current policy. Out: the policy to apply, which
                                       //!< only changes when IdlePriority selects SCHED_IDLE.
                int* schedPriority     //!< Out: the priority number to apply under that policy.
                )
            {
                #ifdef SCHED_IDLE
                    if( priority == GThread::IdlePriority )
                    {
                        *schedPolicy = SCHED_IDLE;
                        *schedPriority = 0;
                        return true;
                    }
                    const int lowestPriority = GThread::LowestPriority;
                #else
                    const int lowestPriority = GThread::IdlePriority;
                #endif
                const int highestPriority = GThread::TimeCriticalPriority;

                const int prioMin = sched_get_priority_min( *schedPolicy );
                const int prioMax = sched_get_priority_max( *schedPolicy );
                if( prioMin == -1 || prioMax == -1 )
                {
                    return false;
                }

                int prio = ( ( priority - lowestPriority ) * ( prioMax - prioMin ) / highestPriority
                           ) +
                    prioMin;
                if( prio < prioMin )
                {
                    prio = prioMin;
                }
                if( prio > prioMax )
                {
                    prio = prioMax;
                }

                *schedPriority = prio;
                return true;
            }

        } // namespace

    #endif

    //! Constructs a new thread object.
    GThread::GThread()
        : GObject()
    {
        m_data = std::make_shared<GThreadData>();
    }

    //! Destroys the thread, waiting for it to finish if running.
    GThread::~GThread()
    {
        quit();
        wait();
    }

    //! Starts execution of the thread by invoking run(). Thread-safe.
    //!
    //! The new thread applies priority to itself as its first action, before started() is
    //! emitted and before run() is entered, so the whole of run() executes at the requested
    //! priority.
    //!
    //! It is not applied quite as early as Qt manages, and the difference is worth knowing if you
    //! are relying on priority for correctness rather than tuning. Qt creates the thread suspended
    //! on Windows, and passes the priority in pthread_attr_t on UNIX, so the thread never executes
    //! a single instruction at the wrong priority. std::thread offers neither, so between the OS
    //! creating the thread and the thread's first instruction it briefly runs at the creating
    //! thread's priority. Nothing belonging to this class runs in that window.
    void GThread::start
        (
        Priority priority  //!< Priority for the new thread. InheritPriority, the default, keeps the
                           //!< creating thread's priority and preserves the behaviour of the
                           //!< no-argument call.
        )
    {
        if( m_running.load() )
        {
            return;
        }

        // If a previous run finished but the caller never called wait(), m_thread can still hold a
        // joinable std::thread. Overwriting m_thread below would destroy that std::thread while it
        // is still joinable, which calls std::terminate(). Join it first.
        if( m_thread && m_thread->joinable() )
        {
            m_thread->join();
        }

        // Held across the std::thread construction so setPriority() can never observe m_running ==
        // true while m_thread is still the previous run's object (or null). A run body that finishes
        // before this scope ends simply waits for the lock at its tail.
        std::lock_guard<std::mutex> startLock( m_priorityMutex );

        m_running.store( true );
        m_finished.store( false );
        m_exiting.store( false );
        // Each run starts from what start() was given, never from what the previous run ended at: a
        // priority set on an earlier run said nothing about this one, and reporting the stale value
        // would be a lie about a thread that never got it.
        m_priority = priority;

        m_thread = std::make_unique<std::thread>(
            [this]()
            {
                s_currentThread = this;
                this->moveToThread( this );

                {
                    // First thing the new thread does, so started() and the whole of run() happen at
                    // the requested priority. It blocks here until start() releases the lock, which
                    // is also what guarantees m_thread is already assigned -- applyPriority() reads
                    // the native handle out of it.
                    std::lock_guard<std::mutex> priorityLock( m_priorityMutex );
                    if( m_priority != InheritPriority )
                    {
                        applyPriority( m_priority );
                    }
                }

                bool createdDispatcher = false;
                if( !m_data->dispatcher() )
                {
                    #if defined( _WIN32 )
                        m_data->setDispatcher( std::make_shared<GEventDispatcherWin32>() );
                    #elif defined( __linux__ )
                        m_data->setDispatcher( std::make_shared<GEventDispatcherLinux>() );
                    #else
                        m_data->setDispatcher( std::make_shared<GEventDispatcherDefault>() );
                    #endif
                    createdDispatcher = true;
                }

                started.emit();

                this->run();

                finished.emit();

                // Drain deferred deletes before letting go of the dispatcher, mirroring Qt's
                // QThreadPrivate::finish(), which calls sendPostedEvents(nullptr, DeferredDelete)
                // right after emitting finished() and before cleanup() destroys the dispatcher.
                // Without this, anything that called deleteLater() before the loop stopped is never
                // destroyed: the dispatcher's destructor can free the queued events but has no way
                // to free their receivers.
                if( auto disp = m_data->dispatcher() )
                {
                    disp->processDeferredDeletes();
                }

                if( createdDispatcher )
                {
                    // Just drop this thread's reference. Any other thread that is part-way through a
                    // call still holds its own strong reference from GThreadData::dispatcher(), so
                    // the dispatcher stays alive until that call finishes rather than being freed
                    // underneath it.
                    m_data->setDispatcher( nullptr );
                }

                {
                    // Under the same mutex setPriority() uses. This is the only place m_running
                    // becomes false, so a setPriority() holding the lock and seeing m_running == true
                    // knows this store has not happened yet and the OS thread is still alive -- which
                    // is what makes using the native handle there safe rather than merely likely.
                    std::lock_guard<std::mutex> priorityLock( m_priorityMutex );
                    m_running.store( false );
                }
                m_finished.store( true );
                {
                    std::lock_guard<std::mutex> lock( m_waitMutex );
                    m_waitCv.notify_all();
                }
                s_currentThread = nullptr;
            } );
    }

    //! Requests the thread's event loop to quit with return code 0. Thread-safe.
    void GThread::quit()
    {
        exit( 0 );
    }

    //! Requests the thread's event loop to exit with specified return code. Thread-safe.
    void GThread::exit
        (
        int returnCode  //!< Exit return code.
        )
    {
        m_exitCode.store( returnCode );
        m_exiting.store( true );
        auto dispatcher = m_data->dispatcher();
        if( dispatcher )
        {
            dispatcher->interrupt();
            dispatcher->wakeUp();
        }
    }

    //! Blocks until the thread has finished executing or timeout expires. Thread-safe. Returns
    //! true if thread finished, false if timeout occurred.
    bool GThread::wait
        (
        unsigned long time  //!< Maximum time to wait in milliseconds.
        )
    {
        if( m_finished.load() )
        {
            if( m_thread && m_thread->joinable() )
            {
                m_thread->join();
            }
            return true;
        }

        if( !m_thread || !m_thread->joinable() )
        {
            return true;
        }

        if( time == ULONG_MAX )
        {
            m_thread->join();
            return true;
        }
        else
        {
            std::unique_lock<std::mutex> lock( m_waitMutex );
            bool completed = m_waitCv.wait_for(
                lock, std::chrono::milliseconds( time ), [this]
                {
                    return m_finished.load();
                } );
            if( completed && m_thread->joinable() )
            {
                m_thread->join();
            }
            return completed;
        }
    }

    //! Checks if the thread is currently running. Thread-safe.
    bool GThread::isRunning() const
    {
        return m_running.load();
    }

    //! Checks if the thread has finished execution. Thread-safe.
    bool GThread::isFinished() const
    {
        return m_finished.load();
    }

    //! Sets the scheduling priority of this thread. Thread-safe.
    //!
    //! Only meaningful while the thread is running, as in Qt: there is no OS thread to act on
    //! before start(), and the value is deliberately not remembered for a later start() either.
    //! A call made when the thread is not running is rejected with a warning and changes nothing,
    //! so priority() will still report InheritPriority afterwards. To give a thread a priority
    //! from the outset, pass one to start() instead.
    //!
    //! What the OS does with the request varies, and a successful call does not promise the
    //! thread's scheduling actually changed. On Linux the default SCHED_OTHER policy reports a
    //! priority range of exactly one value, so every priority maps onto the same number and the
    //! call is accepted but has no effect; real prioritisation there needs a realtime policy and
    //! the privileges to select it. Qt behaves the same way. Windows applies all seven levels.
    void GThread::setPriority
        (
        Priority priority  //!< The priority to apply. InheritPriority is not accepted; rejected with a warning.
        )
    {
        if( priority == InheritPriority )
        {
            std::fprintf( stderr,
                "GThread::setPriority: InheritPriority cannot be set, only reported\n" );
            return;
        }

        std::lock_guard<std::mutex> lock( m_priorityMutex );

        // Qt refuses the same way. There is no OS thread to act on yet, and quietly stashing the
        // value for a future start() would promise a thread priority this class does not deliver.
        if( !m_running.load() || !m_thread )
        {
            std::fprintf( stderr,
                "GThread::setPriority: cannot set priority, thread is not running\n" );
            return;
        }

        m_priority = priority;
        applyPriority( priority );
    }

    //! Gets the scheduling priority of this thread. Thread-safe. Returns the priority last set on
    //! the running thread, or InheritPriority if the thread is not running or no priority has been
    //! set on this run.
    GThread::Priority GThread::priority() const
    {
        std::lock_guard<std::mutex> lock( m_priorityMutex );
        if( !m_running.load() )
        {
            return InheritPriority;
        }
        return m_priority;
    }

    //! Pushes a priority down to the OS thread.
    //!
    //! Split out from setPriority() only so the platform code sits in one place. The caller must
    //! hold m_priorityMutex and must already have established that the thread is running, because
    //! this dereferences m_thread and uses its native handle.
    void GThread::applyPriority
        (
        Priority priority  //!< The priority to apply. Never InheritPriority.
        )
    {
        #if defined( _WIN32 )
            int prio;
            switch( priority )
            {
            case IdlePriority:
                prio = THREAD_PRIORITY_IDLE;
                break;

            case LowestPriority:
                prio = THREAD_PRIORITY_LOWEST;
                break;

            case LowPriority:
                prio = THREAD_PRIORITY_BELOW_NORMAL;
                break;

            case NormalPriority:
                prio = THREAD_PRIORITY_NORMAL;
                break;

            case HighPriority:
                prio = THREAD_PRIORITY_ABOVE_NORMAL;
                break;

            case HighestPriority:
                prio = THREAD_PRIORITY_HIGHEST;
                break;

            case TimeCriticalPriority:
                prio = THREAD_PRIORITY_TIME_CRITICAL;
                break;

            default:
                return;
            }

            if( !SetThreadPriority( static_cast<HANDLE>( m_thread->native_handle() ), prio ) )
            {
                std::fprintf( stderr, "GThread::setPriority: failed to set thread priority\n" );
            }
        #elif defined( G_HAS_THREAD_PRIORITY_SCHEDULING )
            const pthread_t handle = m_thread->native_handle();

            int schedPolicy = 0;
            sched_param param {};
            if( pthread_getschedparam( handle, &schedPolicy, &param ) != 0 )
            {
                std::fprintf( stderr, "GThread::setPriority: cannot get scheduler parameters\n" );
                return;
            }

            int prio = 0;
            if( !calculateUnixPriority( priority, &schedPolicy, &prio ) )
            {
                std::fprintf( stderr,
                    "GThread::setPriority: cannot determine scheduler priority range\n" );
                return;
            }

            param.sched_priority = prio;
            const int status = pthread_setschedparam( handle, schedPolicy, &param );

            #ifdef SCHED_IDLE
                // Asking for SCHED_IDLE can be refused even where the constant exists, so fall back
                // to the lowest priority the thread's existing policy allows.
                //
                // Deviation from Qt, deliberately: qthread_unix.cpp tests `status == -1 && errno ==
                // EINVAL` here, but pthread_setschedparam returns the error number directly and does
                // not touch errno, so that branch can never be taken and the fallback is dead code in
                // Qt. Testing the return value is what actually makes it run.
                if( status == EINVAL && schedPolicy == SCHED_IDLE )
                {
                    if( pthread_getschedparam( handle, &schedPolicy, &param ) == 0 )
                    {
                        param.sched_priority = sched_get_priority_min( schedPolicy );
                        pthread_setschedparam( handle, schedPolicy, &param );
                    }
                }
            #else
                ( void )status;
            #endif
        #else
            // No priority scheduling on this platform; the value is recorded and nothing else.
            ( void )priority;
        #endif
    }

    //! Gets a pointer to the thread currently executing. Thread-safe.
    GThread* GThread::currentThread()
    {
        return s_currentThread;
    }

    //! Gets the event dispatcher for this thread. Thread-safe.
    //!
    //! Read-only by design. There is deliberately no setter: swapping a running thread's
    //! dispatcher raced against that thread's own start/finish lifecycle, which could delete a
    //! dispatcher an active exec()/processEvents() loop was still calling into. A thread creates
    //! and owns its dispatcher in start(); GCoreApplication supplies the main thread's.
    //!
    //! Returns a strong reference rather than a raw pointer, so the dispatcher cannot be destroyed
    //! by its owning thread finishing while the caller is still using it. Returns nullptr before
    //! start().
    std::shared_ptr<GAbstractEventDispatcher> GThread::eventDispatcher() const
    {
        return m_data->dispatcher();
    }

    //! Queues an arbitrary task to run on this thread's event loop.
    //!
    //! Always deferred to a later iteration of this thread's loop -- never run inline, even when
    //! post() is called from this thread itself. Implemented as a thin wrapper over
    //! GObject::dispatchMetaCall() targeting this GThread as both context and receiver, so it goes
    //! through the exact same queue, GMetaCallEvent, and lifetime handling as every other queued
    //! call in this library (removeEventsForReceiver() on destruction, processDeferredDeletes() on
    //! shutdown, etc.) rather than a second, parallel task queue. Returns true if the task was
    //! queued; false if this thread has no dispatcher yet (before start()/exec(), or after it has
    //! fully finished and released it), in which case the task is dropped rather than run.
    bool GThread::post
        (
        std::function<void()> task  //!< The callable to run on this thread. Ignored (returns false) if empty.
        )
    {
        if( !task )
        {
            return false;
        }
        // Explicit QueuedConnection (not Auto): post() must always defer, even when called from this
        // thread itself -- Auto would resolve to a same-thread call and run inline instead.
        return dispatchMetaCall( this, std::move( task ), G::QueuedConnection );
    }

    //! Starting point for thread execution. Can be overridden. Default calls exec().
    void GThread::run()
    {
        exec();
    }

    //! Enters the event loop and waits until exit() is called. Returns the exit code.
    int GThread::exec()
    {
        // Re-fetched each iteration, and held as a strong reference across processEvents() so the
        // dispatcher cannot be destroyed mid-call.
        auto dispatcher = m_data->dispatcher();
        while( !m_exiting.load() && dispatcher )
        {
            dispatcher->processEvents();
            dispatcher = m_data->dispatcher();
        }
        return m_exitCode.load();
    }
}
