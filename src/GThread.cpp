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
    thread_local GThread* GThread::sCurrentThread = nullptr;

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
                int aPriority,          //!< The GThread priority to convert.
                int* aSchedPolicy,      //!< In: the thread's current policy. Out: the policy to apply, which
                                        //!< only changes when IdlePriority selects SCHED_IDLE.
                int* aSchedPriority     //!< Out: the priority number to apply under that policy.
                )
            {
                #ifdef SCHED_IDLE
                    if( aPriority == GThread::IdlePriority )
                    {
                        *aSchedPolicy = SCHED_IDLE;
                        *aSchedPriority = 0;
                        return true;
                    }
                    const int lowestPriority = GThread::LowestPriority;
                #else
                    const int lowestPriority = GThread::IdlePriority;
                #endif
                const int highestPriority = GThread::TimeCriticalPriority;

                const int prioMin = sched_get_priority_min( *aSchedPolicy );
                const int prioMax = sched_get_priority_max( *aSchedPolicy );
                if( prioMin == -1 || prioMax == -1 )
                {
                    return false;
                }

                int prio = ( ( aPriority - lowestPriority ) * ( prioMax - prioMin ) /
                    highestPriority
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

                *aSchedPriority = prio;
                return true;
            }

        } // namespace

    #endif

    //! Constructs a new thread object.
    GThread::GThread()
        : GObject()
    {
        mData = std::make_shared<GThreadData>();
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
        Priority aPriority  //!< Priority for the new thread. InheritPriority, the default, keeps the
                            //!< creating thread's priority and preserves the behaviour of the
                            //!< no-argument call.
        )
    {
        if( mRunning.load() )
        {
            return;
        }

        // If a previous run finished but the caller never called wait(), mThread can still hold a
        // joinable std::thread. Overwriting mThread below would destroy that std::thread while it
        // is still joinable, which calls std::terminate(). Join it first.
        if( mThread && mThread->joinable() )
        {
            mThread->join();
        }

        // Held across the std::thread construction so setPriority() can never observe mRunning ==
        // true while mThread is still the previous run's object (or null). A run body that finishes
        // before this scope ends simply waits for the lock at its tail.
        std::lock_guard<std::mutex> startLock( mPriorityMutex );

        mRunning.store( true );
        mFinished.store( false );
        mExiting.store( false );
        // Each run starts from what start() was given, never from what the previous run ended at: a
        // priority set on an earlier run said nothing about this one, and reporting the stale value
        // would be a lie about a thread that never got it.
        mPriority = aPriority;

        mThread = std::make_unique<std::thread>(
            [this]()
            {
                sCurrentThread = this;
                this->moveToThread( this );

                {
                    // First thing the new thread does, so started() and the whole of run() happen at
                    // the requested priority. It blocks here until start() releases the lock, which
                    // is also what guarantees mThread is already assigned -- applyPriority() reads
                    // the native handle out of it.
                    std::lock_guard<std::mutex> priorityLock( mPriorityMutex );
                    if( mPriority != InheritPriority )
                    {
                        applyPriority( mPriority );
                    }
                }

                bool createdDispatcher = false;
                if( !mData->dispatcher() )
                {
                    #if defined( _WIN32 )
                        mData->setDispatcher( std::make_shared<GEventDispatcherWin32>() );
                    #elif defined( __linux__ )
                        mData->setDispatcher( std::make_shared<GEventDispatcherLinux>() );
                    #else
                        mData->setDispatcher( std::make_shared<GEventDispatcherDefault>() );
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
                if( auto disp = mData->dispatcher() )
                {
                    disp->processDeferredDeletes();
                }

                if( createdDispatcher )
                {
                    // Just drop this thread's reference. Any other thread that is part-way through a
                    // call still holds its own strong reference from GThreadData::dispatcher(), so
                    // the dispatcher stays alive until that call finishes rather than being freed
                    // underneath it.
                    mData->setDispatcher( nullptr );
                }

                {
                    // Under the same mutex setPriority() uses. This is the only place mRunning
                    // becomes false, so a setPriority() holding the lock and seeing mRunning == true
                    // knows this store has not happened yet and the OS thread is still alive -- which
                    // is what makes using the native handle there safe rather than merely likely.
                    std::lock_guard<std::mutex> priorityLock( mPriorityMutex );
                    mRunning.store( false );
                }
                mFinished.store( true );
                {
                    std::lock_guard<std::mutex> lock( mWaitMutex );
                    mWaitCv.notify_all();
                }
                sCurrentThread = nullptr;
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
        int aReturnCode  //!< Exit return code.
        )
    {
        mExitCode.store( aReturnCode );
        mExiting.store( true );
        auto dispatcher = mData->dispatcher();
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
        unsigned long aTime  //!< Maximum time to wait in milliseconds.
        )
    {
        if( mFinished.load() )
        {
            if( mThread && mThread->joinable() )
            {
                mThread->join();
            }
            return true;
        }

        if( !mThread || !mThread->joinable() )
        {
            return true;
        }

        if( aTime == ULONG_MAX )
        {
            mThread->join();
            return true;
        }
        else
        {
            std::unique_lock<std::mutex> lock( mWaitMutex );
            bool completed = mWaitCv.wait_for(
                lock, std::chrono::milliseconds( aTime ), [this]
                {
                    return mFinished.load();
                } );
            if( completed && mThread->joinable() )
            {
                mThread->join();
            }
            return completed;
        }
    }

    //! Checks if the thread is currently running. Thread-safe.
    bool GThread::isRunning() const
    {
        return mRunning.load();
    }

    //! Checks if the thread has finished execution. Thread-safe.
    bool GThread::isFinished() const
    {
        return mFinished.load();
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
        Priority aPriority  //!< The priority to apply. InheritPriority is not accepted; rejected with a warning.
        )
    {
        if( aPriority == InheritPriority )
        {
            std::fprintf( stderr,
                "GThread::setPriority: InheritPriority cannot be set, only reported\n" );
            return;
        }

        std::lock_guard<std::mutex> lock( mPriorityMutex );

        // Qt refuses the same way. There is no OS thread to act on yet, and quietly stashing the
        // value for a future start() would promise a thread priority this class does not deliver.
        if( !mRunning.load() || !mThread )
        {
            std::fprintf( stderr,
                "GThread::setPriority: cannot set priority, thread is not running\n" );
            return;
        }

        mPriority = aPriority;
        applyPriority( aPriority );
    }

    //! Gets the scheduling priority of this thread. Thread-safe. Returns the priority last set on
    //! the running thread, or InheritPriority if the thread is not running or no priority has been
    //! set on this run.
    GThread::Priority GThread::priority() const
    {
        std::lock_guard<std::mutex> lock( mPriorityMutex );
        if( !mRunning.load() )
        {
            return InheritPriority;
        }
        return mPriority;
    }

    //! Pushes a priority down to the OS thread.
    //!
    //! Split out from setPriority() only so the platform code sits in one place. The caller must
    //! hold mPriorityMutex and must already have established that the thread is running, because
    //! this dereferences mThread and uses its native handle.
    void GThread::applyPriority
        (
        Priority aPriority  //!< The priority to apply. Never InheritPriority.
        )
    {
        #if defined( _WIN32 )
            int prio;
            switch( aPriority )
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

            if( !SetThreadPriority( static_cast<HANDLE>( mThread->native_handle() ), prio ) )
            {
                std::fprintf( stderr, "GThread::setPriority: failed to set thread priority\n" );
            }
        #elif defined( G_HAS_THREAD_PRIORITY_SCHEDULING )
            const pthread_t handle = mThread->native_handle();

            int schedPolicy = 0;
            sched_param param {};
            if( pthread_getschedparam( handle, &schedPolicy, &param ) != 0 )
            {
                std::fprintf( stderr, "GThread::setPriority: cannot get scheduler parameters\n" );
                return;
            }

            int prio = 0;
            if( !calculateUnixPriority( aPriority, &schedPolicy, &prio ) )
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
            ( void )aPriority;
        #endif
    }

    //! Gets a pointer to the thread currently executing. Thread-safe.
    GThread* GThread::currentThread()
    {
        return sCurrentThread;
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
        return mData->dispatcher();
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
        std::function<void()> aTask  //!< The callable to run on this thread. Ignored (returns false) if empty.
        )
    {
        if( !aTask )
        {
            return false;
        }
        // Explicit QueuedConnection (not Auto): post() must always defer, even when called from this
        // thread itself -- Auto would resolve to a same-thread call and run inline instead.
        return dispatchMetaCall( this, std::move( aTask ), ConnectionType::QueuedConnection );
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
        auto dispatcher = mData->dispatcher();
        while( !mExiting.load() && dispatcher )
        {
            dispatcher->processEvents();
            dispatcher = mData->dispatcher();
        }
        return mExitCode.load();
    }
}
