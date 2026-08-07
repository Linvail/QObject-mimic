#ifndef GEVENTDISPATCHERDEFAULT_H
#define GEVENTDISPATCHERDEFAULT_H

#include "GAbstractEventDispatcher.h"
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace QtLikeSignal
{
    class GEvent;
    class GObject;

    //! Default cross-platform concrete implementation of GAbstractEventDispatcher.
    //!
    //! All public methods are thread-safe and can be invoked safely across threads.
    class GEventDispatcherDefault : public GAbstractEventDispatcher
    {
    public:
        GEventDispatcherDefault();

        virtual ~GEventDispatcherDefault() override;

        virtual bool processEvents() override;

        virtual void wakeUp() override;

        virtual void interrupt() override;

        virtual void processDeferredDeletes() override;

    protected:
        // Mirrors the access level GAbstractEventDispatcher gives these. The base class's access
        // already governs every call made through the GAbstractEventDispatcher* that
        // GThread::eventDispatcher() hands out, so this is belt-and-suspenders -- it closes the
        // remaining gap for a caller holding a GEventDispatcherDefault* directly.
        virtual void registerTimer
            (
            int timerId,
            int interval,
            GObject* object
            ) override;

        virtual bool unregisterTimer
            (
            int timerId
            ) override;

        virtual void postEvent
            (
            GObject* receiver,
            GEvent* event
            ) override;

        virtual void removeEventsForReceiver
            (
            GObject* receiver
            ) override;

        virtual std::vector<TimerRegistration> takeTimersForReceiver
            (
            GObject* receiver
            ) override;

        friend class GObject;

    private:
        //! One queued event together with the receiver it targets.
        struct EventPair
        {
            GObject* receiver;
            GEvent*  event;
        };

        //! One registered timer's schedule and target.
        struct TimerData
        {
            int timerId;
            int intervalMs;
            GObject*                              receiver;
            std::chrono::steady_clock::time_point nextFire;
        };

        std::deque<EventPair>   m_eventQueue;  //!< Events waiting to be dispatched.
        std::vector<TimerData>  m_timers;      //!< Every timer currently registered.
        std::mutex m_mutex;                    //!< Guards every member below.
        std::condition_variable m_cv;          //!< Wakes processEvents() out of its wait.
        std::atomic<bool>       m_interrupt { false };  //!< Set by interrupt() to stop the loop.
        // Set (under m_mutex) whenever a timer is registered or unregistered, so a processEvents()
        // call currently sleeping in wait_for() re-evaluates its wait deadline instead of sleeping
        // for the stale duration computed before the change.
        bool m_timersChanged { false };
        // Set (under m_mutex) by wakeUp() and consumed by processEvents() once it returns from
        // waiting. Needed because the wait is predicate-based: without a state change to observe, a
        // bare notify_all() from wakeUp() cannot end the wait.
        bool m_wakeUpRequested { false };
    };
}

#endif // GEVENTDISPATCHERDEFAULT_H
