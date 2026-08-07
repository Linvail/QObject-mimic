#include "GObject.h"

#include "GAbstractEventDispatcher.h"
#include "GEvent.h"
#include "GThread.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace QtLikeSignal
{
    //! One pending deferred call: the invoker to run, guarded by its own mutex.
    struct CallLaterNode
    {
        std::mutex mMutex;
        std::function<void()> mInvoker;
    };

    //! Process-wide registry of callLater invocations still waiting to run.
    //!
    //! Exists as a friend of GObject purely so it can name GObject's private GCallLaterKey /
    //! GCallLaterKeyHash types; a plain file-scope map could not. Not declared in any header.
    struct GCallLaterRegistry
    {
        static std::mutex sMutex;
        static std::unordered_map<GObject::GCallLaterKey,
            std::shared_ptr<CallLaterNode>,
            GObject::GCallLaterKeyHash>
        sPending;
    };

    std::mutex GCallLaterRegistry::sMutex;
    std::unordered_map<GObject::GCallLaterKey,
        std::shared_ptr<CallLaterNode>,
        GObject::GCallLaterKeyHash>
    GCallLaterRegistry::sPending;

    std::atomic<int> GObject::sNextTimerId { 1 };

    //! Constructs an object.
    GObject::GObject()
        : mLife( std::make_shared<int>( 0 ) )
    {
        GThread* current = GThread::currentThread();
        mThread.store( current );
        if( current )
        {
            std::shared_ptr<GThreadData> data = current->threadData();
            std::lock_guard<std::mutex> lock( mThreadDataMutex );
            mThreadData = std::move( data );
        }
    }

    //! Destroys the object and triggers all registered cleanup callbacks.
    GObject::~GObject()
    {
        // Invalidate the life token first. connect()/callLater() wrappers running on other threads
        // check objectLife().lock() before posting a call to this object; resetting mLife up front
        // shrinks the window in which such a wrapper can still observe this object as "alive" to
        // the check-then-post race itself, instead of the whole destructor body (which below runs
        // arbitrary user cleanup-callback code).
        mLife.reset();

        // Move the callbacks out from under mCleanupMutex before invoking any of them. Running them
        // while still holding the lock deadlocks on the non-recursive mutex if a callback calls
        // addCleanupCallback() on this same object. A callback registered during the loop below is
        // intentionally dropped -- this object is already being destroyed, so there is no later point
        // at which it could meaningfully run.
        std::vector<std::function<void()> > callbacksToRun;
        {
            std::lock_guard<std::mutex> lock( mCleanupMutex );
            callbacksToRun.swap( mCleanupCallbacks );
        }
        for( auto& cb : callbacksToRun )
        {
            cb();
        }

        {
            std::lock_guard<std::mutex> lock( GCallLaterRegistry::sMutex );
            auto& pending = GCallLaterRegistry::sPending;
            for( auto it = pending.begin(); it != pending.end();)
            {
                if( it->first.mContext == this )
                {
                    it = pending.erase( it );
                }
                else
                {
                    ++it;
                }
            }
        }

        std::shared_ptr<GThreadData> threadDataCopy;
        {
            std::lock_guard<std::mutex> lock( mThreadDataMutex );
            threadDataCopy = mThreadData;
        }
        if( threadDataCopy )
        {
            if( auto dispatcher = threadDataCopy->dispatcher() )
            {
                dispatcher->removeEventsForReceiver( this );
            }
        }
    }

    //! Internal helper to schedule or update a callLater deferred invocation.
    void GObject::scheduleCallLater
        (
        GObject* aContext,               //!< Target context object.
        const GCallLaterKey& aKey,       //!< Key identifying the deferred call.
        std::function<void()> aInvoker   //!< Callback executing the call.
        )
    {
        if( !aContext )
        {
            return;
        }

        std::shared_ptr<CallLaterNode> node;
        bool isNew = false;

        {
            std::lock_guard<std::mutex> lock( GCallLaterRegistry::sMutex );
            auto& pending = GCallLaterRegistry::sPending;
            auto it = pending.find( aKey );
            if( it != pending.end() )
            {
                node = it->second;
            }
            else
            {
                node = std::make_shared<CallLaterNode>();
                pending[aKey] = node;
                isNew = true;
            }
        }

        {
            std::lock_guard<std::mutex> nodeLock( node->mMutex );
            node->mInvoker = std::move( aInvoker );
        }

        if( isNew )
        {
            std::weak_ptr<int> weakLife = aContext->objectLife();
            auto metaCall = [aKey, node, weakLife]()
                {
                    std::function<void()> fnToRun;
                    {
                        std::lock_guard<std::mutex> lock( GCallLaterRegistry::sMutex );
                        GCallLaterRegistry::sPending.erase( aKey );
                    }
                    {
                        std::lock_guard<std::mutex> nodeLock( node->mMutex );
                        fnToRun = std::move( node->mInvoker );
                    }
                    if( fnToRun )
                    {
                        if( auto life = weakLife.lock() )
                        {
                            fnToRun();
                        }
                    }
                };

            if( !dispatchMetaCall( aContext, metaCall, G::QueuedConnection ) )
            {
                // The target has no dispatcher yet, so this call can never run. Drop the registry
                // entry we just created: leaving it behind is what made this failure permanent, since
                // every later callLater() for the same target would find it, take the "already
                // scheduled" branch above, and never dispatch again -- silently disabling that
                // (context, slot) pair for the rest of the object's life, even once a dispatcher
                // existed. Erasing lets the next call re-arm. This call is still lost; only a
                // retry queue could save it, which would need its own ownership rules.
                std::lock_guard<std::mutex> lock( GCallLaterRegistry::sMutex );
                GCallLaterRegistry::sPending.erase( aKey );
            }
        }
    }

    //! Gets the thread affinity of this object. Thread-safe.
    GThread* GObject::thread() const
    {
        return mThread.load();
    }

    //! Gets the thread data container holding this object's event dispatcher.
    //!
    //! Private: this is internal plumbing with no QObject equivalent -- Qt's
    //! QObjectPrivate::threadData is likewise not public API. It is the handle through which the
    //! dispatcher is reached, so exposing it hands out the machinery every other access-control
    //! decision in this class exists to protect. Returns nullptr if this object has no affinity.
    //! Thread-safe.
    std::shared_ptr<GThreadData> GObject::threadData() const
    {
        std::lock_guard<std::mutex> lock( mThreadDataMutex );
        return mThreadData;
    }

    //! Changes the thread affinity of this object.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, matching Qt's
    //! QObject::moveToThread() ("Current thread is not the object's thread. Cannot move to target
    //! thread"). Only the thread that currently owns an object may hand it to another; letting any
    //! thread re-home an object at will would race the owner's own use of it.
    //!
    //! Qt's one exception is reproduced: an object with *no* thread affinity yet may be adopted by
    //! the calling thread. That is what lets a freshly constructed object be moved onto a worker,
    //! and what lets GThread adopt itself when its run loop starts. Returns true if the object now
    //! lives in the requested thread (including when it already did); false if the move was
    //! refused, in which case the affinity is unchanged.
    bool GObject::moveToThread
        (
        GThread* aThread  //!< The new thread this object will live in; nullptr clears the affinity.
        )
    {
        GThread* const currentAffinity = mThread.load();
        GThread* const callerThread = GThread::currentThread();

        if( currentAffinity == aThread )
        {
            // Already there; nothing to do and nothing to refuse.
            return true;
        }

        // Transcribed from Qt's QObject::moveToThread(). The general rule is that only the thread that
        // owns an object may re-home it, with one exception: an object that has no affinity yet may be
        // adopted by the calling thread. That exception is what makes the two normal idioms work --
        // moving a freshly constructed object onto a worker, and GThread adopting itself once its run
        // loop starts -- while still rejecting one thread yanking another thread's live object away.
        const bool adoptingUnownedObject = ( currentAffinity == nullptr && aThread == callerThread )
        ;
        if( !adoptingUnownedObject && currentAffinity != callerThread )
        {
            std::fprintf( stderr,
                "GObject::moveToThread: current thread is not the object's thread; cannot "
                "move it to the target thread\n" );
            return false;
        }

        // Take any active timers off the outgoing dispatcher before the affinity changes. Qt documents
        // this behaviour ("all active timers for the object will be reset ... stopped in the current
        // thread and restarted, with the same interval, in the targetThread"); without it the timers
        // would keep firing on the thread the object just left, delivering timerEvent() somewhere it
        // no longer lives.
        std::vector<GAbstractEventDispatcher::TimerRegistration> timersToMove;
        {
            std::shared_ptr<GThreadData> oldData;
            {
                std::lock_guard<std::mutex> lock( mThreadDataMutex );
                oldData = mThreadData;
            }
            if( oldData )
            {
                if( auto oldDispatcher = oldData->dispatcher() )
                {
                    timersToMove = oldDispatcher->takeTimersForReceiver( this );
                }
            }
        }

        // Resolve the new thread's data before taking our own lock, and store it under the lock in
        // one atomic-looking step so concurrent readers of threadData() never see a half-updated or
        // torn shared_ptr. The lock is still needed even though writes are now single-threaded:
        // threadData() is read from other threads.
        std::shared_ptr<GThreadData> newData = aThread ? aThread->threadData() : nullptr;
        mThread.store( aThread );
        {
            std::lock_guard<std::mutex> lock( mThreadDataMutex );
            mThreadData = std::move( newData );
        }

        if( !timersToMove.empty() )
        {
            // Re-register on the destination thread rather than from here: registerTimer() must run
            // where the timer will be serviced. Qt solves it the same way, queueing the
            // re-registration with invokeMethod(..., Qt::QueuedConnection) so it lands on the new
            // thread. If this object is destroyed before the queued call runs, ~GObject() strips its
            // pending events from the dispatcher, so the call is dropped rather than dangling.
            dispatchMetaCall(
                this,
                [this, timersToMove]()
                {
                    if( auto tData = threadData() )
                    {
                        if( auto disp = tData->dispatcher() )
                        {
                            for( const auto& timer : timersToMove )
                            {
                                disp->registerTimer( timer.mTimerId, timer.mIntervalMs, this );
                            }
                        }
                    }
                },
                G::QueuedConnection );
        }

        return true;
    }

    //! Gets the object's descriptive name. Thread-safe.
    std::string GObject::objectName() const
    {
        std::lock_guard<std::mutex> lock( mNameMutex );
        return mObjectName;
    }

    //! Sets the object's descriptive name. Thread-safe.
    void GObject::setObjectName
        (
        const std::string& aName  //!< The new object name.
        )
    {
        std::lock_guard<std::mutex> lock( mNameMutex );
        mObjectName = aName;
    }

    //! Schedules this object for deletion in the event loop. Thread-safe.
    void GObject::deleteLater()
    {
        auto* event = new GDeferredDeleteEvent();
        if( auto tData = threadData() )
        {
            if( auto disp = tData->dispatcher() )
            {
                disp->postEvent( this, static_cast<GEvent*>( event ) );
                return;
            }
        }
        delete event;
        delete this;
    }

    //! Internal event dispatch plumbing; routes an event to its handler.
    //!
    //! Deliberately private and non-virtual: this is not an extension point. The event queue is
    //! the sole caller (see the friend declaration in the header), and the set of event types is
    //! closed. Override timerEvent() instead to react to timers. Returns true if the event was
    //! recognized and handled.
    bool GObject::event
        (
        GEvent* aEvent  //!< The event to handle.
        )
    {
        if( !aEvent )
        {
            return false;
        }

        switch( aEvent->type() )
        {
        case GEvent::Timer:
            timerEvent( static_cast<GTimerEvent*>( aEvent ) );
            return true;

        case GEvent::DeferredDelete:
            delete this;
            return true;

        case GEvent::MetaCall:
            static_cast<GMetaCallEvent*>( aEvent )->placeMetaCall();
            return true;

        default:
            // The only events that reach this queue are the three above, all posted by GObject's
            // own internals. Nothing can inject an arbitrary event for an arbitrary receiver, so
            // any other type is unreachable rather than something to hand to a user hook.
            return false;
        }
    }

    //! Handles timer events sent to this object.
    void GObject::timerEvent
        (
        GTimerEvent* aEvent  //!< The timer event containing the timer ID.
        )
    {
        ( void )aEvent;
    }

    //! Starts a timer for this object with the specified interval.
    //!
    //! **Not thread-safe: must be called from this object's own thread.** Timers are owned by the
    //! dispatcher of the thread the object lives in, and only that thread's event loop can deliver
    //! the resulting timerEvent(). Calling from any other thread is rejected with a warning on
    //! stderr and returns -1, matching Qt, whose QObject::startTimer() likewise refuses
    //! ("Timers cannot be started from another thread"). To start a timer for an object living in
    //! another thread, get onto that thread first -- for example with callLater(). Returns the
    //! unique timer ID, or -1 if the timer could not be started.
    int GObject::startTimer
        (
        int aInterval  //!< Interval in milliseconds.
        )
    {
        // Thread-confined, as in Qt. The timer lives in the dispatcher belonging to this object's
        // thread, and only that thread's event loop can ever deliver the resulting timerEvent().
        // Registering from elsewhere would either race that dispatcher's lifetime or quietly install
        // a timer whose events the caller is not positioned to receive, so refuse it outright rather
        // than doing something surprising.
        if( thread() != GThread::currentThread() )
        {
            std::fprintf( stderr,
                "GObject::startTimer: timers cannot be started from another thread\n" );
            return -1;
        }

        if( auto tData = threadData() )
        {
            if( auto disp = tData->dispatcher() )
            {
                // Only consume an id once the timer is actually going to be registered.
                const int timerId = sNextTimerId.fetch_add( 1 );
                disp->registerTimer( timerId, aInterval, this );
                return timerId;
            }
        }

        std::fprintf( stderr,
            "GObject::startTimer: this thread has no event dispatcher, so the timer cannot "
            "be started\n" );
        return -1;
    }

    //! Kills the timer with the specified ID.
    //!
    //! **Not thread-safe: must be called from this object's own thread**, for the same reason as
    //! startTimer(). Calls from another thread are rejected with a warning and do nothing.
    void GObject::killTimer
        (
        int aId  //!< The timer ID to stop.
        )
    {
        // Thread-confined for the same reason as startTimer().
        if( thread() != GThread::currentThread() )
        {
            std::fprintf( stderr,
                "GObject::killTimer: timers cannot be stopped from another thread\n" )
            ;
            return;
        }

        if( auto tData = threadData() )
        {
            if( auto disp = tData->dispatcher() )
            {
                disp->unregisterTimer( aId );
            }
        }
    }

    //! Registers a callback to be executed when this object is destroyed. Thread-safe.
    void GObject::addCleanupCallback
        (
        std::function<void()> aCallback  //!< The function to execute upon destruction.
        )
    {
        std::lock_guard<std::mutex> lock( mCleanupMutex );
        mCleanupCallbacks.push_back( std::move( aCallback ) );
    }

    //! Dispatches a metacall callback to the target object's event loop based on connection type.
    //! Thread-safe. Returns true if the slot ran (direct) or was queued successfully; false if it
    //! could not be delivered at all, which happens when the target has no thread affinity or its
    //! thread has no event dispatcher yet. Callers that track pending state must undo it when this
    //! returns false.
    bool GObject::dispatchMetaCall
        (
        GObject* aTarget,               //!< Target GObject.
        std::function<void()> aSlot,    //!< Callback function.
        G::ConnectionType aType         //!< Connection type.
        )
    {
        if( !aTarget )
        {
            return false;
        }

        GThread* targetThread = aTarget->thread();
        G::ConnectionType activeType = aType;
        if( activeType == G::AutoConnection )
        {
            GThread* currentThread = GThread::currentThread();
            if( currentThread == targetThread )
            {
                activeType = G::DirectConnection;
            }
            else
            {
                activeType = G::QueuedConnection;
            }
        }

        if( activeType == G::QueuedConnection )
        {
            auto* event = new GMetaCallEvent( aSlot );
            if( auto tData = aTarget->threadData() )
            {
                if( auto disp = tData->dispatcher() )
                {
                    disp->postEvent( aTarget, static_cast<GEvent*>( event ) );
                    return true;
                }
            }
            delete event;
            return false;
        }

        aSlot();
        return true;
    }
}
