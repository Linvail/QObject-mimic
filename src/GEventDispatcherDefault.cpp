#include "GEventDispatcherDefault.h"
#include "GEvent.h"
#include "GObject.h"
#include <algorithm>
#include <unordered_set>

namespace QtLikeSignal
{
    GEventDispatcherDefault::GEventDispatcherDefault() = default;

    GEventDispatcherDefault::~GEventDispatcherDefault()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        while( !m_eventQueue.empty() )
        {
            delete m_eventQueue.front().event;
            m_eventQueue.pop_front();
        }
    }

    bool GEventDispatcherDefault::processEvents()
    {
        if( m_interrupt )
        {
            return false;
        }

        std::vector<EventPair>    eventsToProcess;
        std::vector<EventPair>    timerEventsToProcess;
        std::chrono::milliseconds maxWait { 100 };

        {
            std::unique_lock<std::mutex> lock( m_mutex );

            auto now = std::chrono::steady_clock::now();

            // Collect expired timers
            for( auto& t : m_timers )
            {
                if( now >= t.nextFire )
                {
                    timerEventsToProcess.push_back( { t.receiver, new GTimerEvent( t.timerId ) } );
                    t.nextFire = now + std::chrono::milliseconds( t.intervalMs );
                }
            }

            // Determine wait time for next timer if no events present
            if( m_eventQueue.empty() && timerEventsToProcess.empty() )
            {
                if( !m_timers.empty() )
                {
                    auto minFire = m_timers.front().nextFire;
                    for( const auto& t : m_timers )
                    {
                        if( t.nextFire < minFire )
                        {
                            minFire = t.nextFire;
                        }
                    }
                    if( minFire > now )
                    {
                        maxWait = std::chrono::duration_cast<std::chrono::milliseconds>( minFire -
                            now )
                        ;
                    }
                    else
                    {
                        maxWait = std::chrono::milliseconds( 0 );
                    }
                }

                // Clear the change flag right before waiting, still holding m_mutex, so any
                // registerTimer()/unregisterTimer() call that runs concurrently is guaranteed to
                // either land before this point (already reflected in maxWait above) or after
                // (blocked on m_mutex until we release it inside wait_for, then setting the flag and
                // notifying) -- there is no window where a change can be lost.
                m_timersChanged = false;

                auto wakeCondition = [this]
                    {
                        return !m_eventQueue.empty() || m_interrupt || m_timersChanged
                               || m_wakeUpRequested;
                    };

                if( m_timers.empty() )
                {
                    // Nothing is scheduled, so there is no deadline to poll for: block until
                    // something actually happens rather than waking ten times a second forever.
                    // Every state this predicate tests is changed under m_mutex by a caller that
                    // then notifies (postEvent, registerTimer, unregisterTimer, interrupt, wakeUp),
                    // so there is no wakeup to miss.
                    m_cv.wait( lock, wakeCondition );
                }
                else
                {
                    m_cv.wait_for( lock, maxWait, wakeCondition );
                }

                // wakeUp() is a one-shot "return from the wait now" request; consume it so a later
                // processEvents() call does not treat it as still pending.
                m_wakeUpRequested = false;
            }

            if( m_interrupt )
            {
                // timerEventsToProcess may already hold heap-allocated GTimerEvent objects collected
                // above; they were never handed off to the dispatch loop below, so free them here to
                // avoid leaking them.
                for( auto& ep : timerEventsToProcess )
                {
                    delete ep.event;
                }
                return false;
            }

            // Drain current queued events
            while( !m_eventQueue.empty() )
            {
                eventsToProcess.push_back( m_eventQueue.front() );
                m_eventQueue.pop_front();
            }
        }

        bool processedAny = false;

        // Tracks receivers that were deleted via a GDeferredDeleteEvent processed earlier in this
        // same batch. Both eventsToProcess and timerEventsToProcess are snapshots drained/collected
        // before dispatch begins, so removeEventsForReceiver() (called from ~GObject()) cannot strip
        // a receiver's remaining entries out of these local vectors -- without this guard, a later
        // entry for the same (now-deleted) receiver would be a use-after-free.
        std::unordered_set<GObject*> deletedReceivers;

        // Dispatch queued events
        for( const auto& ep : eventsToProcess )
        {
            if( !ep.receiver || !ep.event )
            {
                delete ep.event;
                continue;
            }
            if( deletedReceivers.count( ep.receiver ) )
            {
                delete ep.event;
                continue;
            }

            const bool isDeferredDelete = ( ep.event->type() == GEvent::DeferredDelete );
            ep.receiver->event( ep.event );
            if( isDeferredDelete )
            {
                deletedReceivers.insert( ep.receiver );
            }
            delete ep.event;
            processedAny = true;
        }

        // Dispatch timer events
        for( const auto& ep : timerEventsToProcess )
        {
            if( !ep.receiver || !ep.event )
            {
                delete ep.event;
                continue;
            }
            if( deletedReceivers.count( ep.receiver ) )
            {
                delete ep.event;
                continue;
            }

            ep.receiver->event( ep.event );
            delete ep.event;
            processedAny = true;
        }

        return processedAny;
    }

    void GEventDispatcherDefault::registerTimer
        (
        int timerId,
        int interval,
        GObject* object
        )
    {
        if( !object || interval < 0 )
        {
            return;
        }

        std::lock_guard<std::mutex> lock( m_mutex );
        auto now = std::chrono::steady_clock::now();
        TimerData td;
        td.timerId    = timerId;
        td.intervalMs = interval;
        td.receiver   = object;
        td.nextFire   = now + std::chrono::milliseconds( interval );

        for( auto& t : m_timers )
        {
            if( t.timerId == timerId )
            {
                t             = td;
                m_timersChanged = true;
                m_cv.notify_all();
                return;
            }
        }
        m_timers.push_back( td );
        m_timersChanged = true;
        m_cv.notify_all();
    }

    bool GEventDispatcherDefault::unregisterTimer
        (
        int timerId
        )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = std::remove_if( m_timers.begin(),
            m_timers.end(),
            [timerId]( const TimerData& td )
            {
                return td.timerId == timerId;
            } );
        if( it != m_timers.end() )
        {
            m_timers.erase( it, m_timers.end() );
            m_timersChanged = true;
            m_cv.notify_all();
            return true;
        }
        return false;
    }

    void GEventDispatcherDefault::postEvent
        (
        GObject* receiver,
        GEvent* event
        )
    {
        if( !receiver || !event )
        {
            delete event;
            return;
        }

        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_eventQueue.push_back( { receiver, event } );
        }
        m_cv.notify_all();
    }

    void GEventDispatcherDefault::removeEventsForReceiver
        (
        GObject* receiver
        )
    {
        if( !receiver )
        {
            return;
        }

        std::lock_guard<std::mutex> lock( m_mutex );

        auto itQueue = std::remove_if( m_eventQueue.begin(),
            m_eventQueue.end(),
            [receiver]( const EventPair& ep )
            {
                if( ep.receiver == receiver )
                {
                    delete ep.event;
                    return true;
                }
                return false;
            } );
        m_eventQueue.erase( itQueue, m_eventQueue.end() );

        auto itTimer
            = std::remove_if( m_timers.begin(),
            m_timers.end(),
            [receiver]( const TimerData& td )
            {
                return td.receiver == receiver;
            } );
        m_timers.erase( itTimer, m_timers.end() );
    }

    std::vector<GAbstractEventDispatcher::TimerRegistration>GEventDispatcherDefault::
    takeTimersForReceiver
        (
        GObject* receiver
        )
    {
        std::vector<TimerRegistration> taken;
        if( !receiver )
        {
            return taken;
        }

        std::lock_guard<std::mutex> lock( m_mutex );

        auto it = std::remove_if( m_timers.begin(),
            m_timers.end(),
            [receiver, &taken]( const TimerData& td )
            {
                if( td.receiver != receiver )
                {
                    return false;
                }
                taken.push_back( { td.timerId, td.intervalMs } );
                return true;
            } );
        if( it != m_timers.end() )
        {
            m_timers.erase( it, m_timers.end() );
            // The wait deadline was computed from a timer list that no longer holds these entries.
            m_timersChanged = true;
            m_cv.notify_all();
        }

        return taken;
    }

    void GEventDispatcherDefault::processDeferredDeletes()
    {
        // Destroying an object can queue further deferred deletes -- a cleanup callback may
        // deleteLater() something else -- so keep draining until none remain, as Qt does rather than
        // capping the number of passes.
        //
        // Receivers already destroyed in an earlier pass are tracked for the same reason
        // processEvents() tracks them: two deleteLater() calls on one object queue two events, and
        // dispatching the second after the first has destroyed it would be a use-after-free.
        std::unordered_set<GObject*> deletedReceivers;

        for(;;)
        {
            std::vector<EventPair> deferredDeletes;
            {
                std::lock_guard<std::mutex> lock( m_mutex );
                for( auto it = m_eventQueue.begin(); it != m_eventQueue.end();)
                {
                    if( it->event && it->event->type() == GEvent::DeferredDelete )
                    {
                        deferredDeletes.push_back(*it );
                        it = m_eventQueue.erase( it );
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            if( deferredDeletes.empty() )
            {
                break;
            }

            // Dispatch with m_mutex released: ~GObject() calls removeEventsForReceiver(), which takes
            // the same non-recursive mutex and would otherwise deadlock.
            for( const auto& ep : deferredDeletes )
            {
                if( ep.receiver && deletedReceivers.insert( ep.receiver ).second )
                {
                    ep.receiver->event( ep.event );
                }
                delete ep.event;
            }
        }
    }

    void GEventDispatcherDefault::wakeUp()
    {
        // The flag must be set under m_mutex, not just notified. processEvents() waits on a
        // predicate, so a bare notify_all() is a no-op unless some state the predicate tests has
        // changed -- previously wakeUp() only ever "worked" because the wait was capped at 100ms and
        // would have returned on its own anyway.
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_wakeUpRequested = true;
        }
        m_cv.notify_all();
    }

    void GEventDispatcherDefault::interrupt()
    {
        // Taking m_mutex here is what makes the unbounded wait in processEvents() safe. Setting the
        // atomic without the lock leaves a lost-wakeup window: a waiter that has already evaluated
        // its predicate as false, but has not yet atomically released the lock and blocked, would
        // miss both the flag and the notification. That was survivable while the wait was capped at
        // 100ms; with no cap it would hang forever. Blocking on m_mutex here means this can only
        // land either fully before the predicate check or after the waiter is genuinely blocked.
        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_interrupt = true;
        }
        m_cv.notify_all();
    }
}
