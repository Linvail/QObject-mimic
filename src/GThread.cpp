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

thread_local GThread* GThread::s_currentThread = nullptr;

#if !defined( _WIN32 ) && defined( G_HAS_THREAD_PRIORITY_SCHEDULING )

    namespace
    {

        /**
         * @brief Maps a GThread priority onto a scheduler policy and priority number.
         *
         * This is Qt's mapping from qthread_unix.cpp, including its deliberately coarse scaling: the
         * divisor is TimeCriticalPriority rather than the span between the lowest and highest values, so
         * the enum lands on the low end of the platform's range rather than spreading across it. Kept as
         * Qt has it so behaviour matches; the alternative would be a library that claims to mimic QThread
         * and then schedules differently.
         *
         * @param priority The GThread priority to convert.
         * @param schedPolicy In: the thread's current policy. Out: the policy to apply, which only
         * changes when IdlePriority selects SCHED_IDLE.
         * @param schedPriority Out: the priority number to apply under that policy.
         * @return True if a priority could be calculated; false if the platform would not report a range.
         */
        bool calculateUnixPriority
            (
            int priority,
            int* schedPolicy,
            int* schedPriority
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

            int prio = ( ( priority - lowestPriority ) * ( prioMax - prioMin ) / highestPriority ) +
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

GThread::GThread()
    : GObject()
{
    m_data = std::make_shared<GThreadData>();
}

GThread::~GThread()
{
    quit();
    wait();
}

void GThread::start
    (
    Priority priority
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

void GThread::quit()
{
    exit( 0 );
}

void GThread::exit
    (
    int returnCode
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

bool GThread::wait
    (
    unsigned long time
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

bool GThread::isRunning() const
{
    return m_running.load();
}

bool GThread::isFinished() const
{
    return m_finished.load();
}

void GThread::setPriority
    (
    Priority priority
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

GThread::Priority GThread::priority() const
{
    std::lock_guard<std::mutex> lock( m_priorityMutex );
    if( !m_running.load() )
    {
        return InheritPriority;
    }
    return m_priority;
}

void GThread::applyPriority
    (
    Priority priority
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

GThread* GThread::currentThread()
{
    return s_currentThread;
}

std::shared_ptr<GAbstractEventDispatcher> GThread::eventDispatcher() const
{
    return m_data->dispatcher();
}

bool GThread::post
    (
    std::function<void()> task
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

void GThread::run()
{
    exec();
}

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
