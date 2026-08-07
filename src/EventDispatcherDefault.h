#ifndef EVENTDISPATCHERDEFAULT_H
#define EVENTDISPATCHERDEFAULT_H

#include "AbstractEventDispatcher.h"
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace QtLikeSignal
{
    class Event;
    class Object;

    //! Default cross-platform concrete implementation of AbstractEventDispatcher.
    //!
    //! All public methods are thread-safe and can be invoked safely across threads.
    class EventDispatcherDefault : public AbstractEventDispatcher
    {
    public:
        EventDispatcherDefault();

        virtual ~EventDispatcherDefault() override;

        virtual bool processEvents() override;

        virtual void wakeUp() override;

        virtual void interrupt() override;

        virtual void processDeferredDeletes() override;

    protected:
        // Mirrors the access level AbstractEventDispatcher gives these. The base class's access
        // already governs every call made through the AbstractEventDispatcher* that
        // Thread::eventDispatcher() hands out, so this is belt-and-suspenders -- it closes the
        // remaining gap for a caller holding a EventDispatcherDefault* directly.
        virtual void registerTimer
            (
            int aTimerId,
            int aInterval,
            Object* aObject
            ) override;

        virtual bool unregisterTimer
            (
            int aTimerId
            ) override;

        virtual void postEvent
            (
            Object* aReceiver,
            Event* aEvent
            ) override;

        virtual void removeEventsForReceiver
            (
            Object* aReceiver
            ) override;

        virtual std::vector<TimerRegistration> takeTimersForReceiver
            (
            Object* aReceiver
            ) override;

        friend class Object;

    private:
        //! One queued event together with the receiver it targets.
        struct EventPair
        {
            Object* mReceiver;
            Event*  mEvent;
        };

        //! One registered timer's schedule and target.
        struct TimerData
        {
            int mTimerId;
            int mIntervalMs;
            Object*                              mReceiver;
            std::chrono::steady_clock::time_point mNextFire;
        };

        std::deque<EventPair>   mEventQueue;  //!< Events waiting to be dispatched.
        std::vector<TimerData>  mTimers;      //!< Every timer currently registered.
        std::mutex mMutex;                    //!< Guards every member below.
        std::condition_variable mCv;          //!< Wakes processEvents() out of its wait.
        std::atomic<bool>       mInterrupt { false };  //!< Set by interrupt() to stop the loop.
        // Set (under mMutex) whenever a timer is registered or unregistered, so a processEvents()
        // call currently sleeping in wait_for() re-evaluates its wait deadline instead of sleeping
        // for the stale duration computed before the change.
        bool mTimersChanged { false };
        // Set (under mMutex) by wakeUp() and consumed by processEvents() once it returns from
        // waiting. Needed because the wait is predicate-based: without a state change to observe, a
        // bare notify_all() from wakeUp() cannot end the wait.
        bool mWakeUpRequested { false };
    };
}

#endif // EVENTDISPATCHERDEFAULT_H
