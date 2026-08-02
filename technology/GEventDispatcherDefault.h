#ifndef GEVENTDISPATCHERDEFAULT_H
#define GEVENTDISPATCHERDEFAULT_H

#include "GAbstractEventDispatcher.h"
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

class GEvent;
class GObject;

/**
 * @brief Default cross-platform concrete implementation of GAbstractEventDispatcher.
 *
 * All public methods are thread-safe and can be invoked safely across threads.
 */
class GEventDispatcherDefault : public GAbstractEventDispatcher
{
public:
    /**
     * @brief Constructs a new default event dispatcher.
     */
    GEventDispatcherDefault();

    /**
     * @brief Destroys the default event dispatcher and frees pending events.
     */
    virtual ~GEventDispatcherDefault() override;

    /**
     * @brief Processes pending events and expired timers once without an internal infinite loop.
     * @return True if any events or timers were processed, false otherwise.
     *
     * Note: Thread-safe. Called by thread event loop.
     */
    virtual bool processEvents() override;

    /**
     * @brief Wakes up the event loop if waiting.
     *
     * Note: Thread-safe.
     */
    virtual void wakeUp() override;

    /**
     * @brief Interrupts processEvents execution.
     *
     * Note: Thread-safe.
     */
    virtual void interrupt() override;

    /**
     * @brief Dispatches any pending deferred-delete events, destroying their receivers.
     *
     * Note: Thread-safe. Intended to run on the dispatcher's own thread.
     */
    virtual void processDeferredDeletes() override;

protected:
    // Mirrors the access level GAbstractEventDispatcher gives these. The base class's access
    // already governs every call made through the GAbstractEventDispatcher* that
    // GThread::eventDispatcher() hands out, so this is belt-and-suspenders -- it closes the
    // remaining gap for a caller holding a GEventDispatcherDefault* directly.

    /**
     * @brief Registers a timer for a target object.
     * @param timerId Unique timer identifier.
     * @param interval Interval in milliseconds.
     * @param object Target object to receive GTimerEvent.
     *
     * Note: Thread-safe.
     */
    virtual void registerTimer(int timerId, int interval, GObject* object) override;

    /**
     * @brief Unregisters a timer by ID.
     * @param timerId Unique timer identifier.
     * @return True if timer was found and removed, false otherwise.
     *
     * Note: Thread-safe.
     */
    virtual bool unregisterTimer(int timerId) override;

    /**
     * @brief Thread-safely posts an event to the dispatcher's queue.
     * @param receiver The target object receiving the event.
     * @param event The event to be dispatched.
     *
     * Note: Thread-safe.
     */
    virtual void postEvent(GObject* receiver, GEvent* event) override;

    /**
     * @brief Removes and deletes all pending events for the specified receiver.
     * @param receiver The target receiver object.
     *
     * Note: Thread-safe.
     */
    virtual void removeEventsForReceiver(GObject* receiver) override;

    /**
     * @brief Unregisters the receiver's timers and returns them for re-registration elsewhere.
     * @param receiver The receiver whose timers should be taken.
     * @return The removed registrations; empty if the receiver had none.
     *
     * Note: Thread-safe.
     */
    virtual std::vector<TimerRegistration> takeTimersForReceiver(GObject* receiver) override;

    friend class GObject;

private:
    struct EventPair
    {
        GObject* receiver;
        GEvent*  event;
    };

    struct TimerData
    {
        int                                   timerId;
        int                                   intervalMs;
        GObject*                              receiver;
        std::chrono::steady_clock::time_point nextFire;
    };

    std::deque<EventPair>   m_eventQueue;
    std::vector<TimerData>  m_timers;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool>       m_interrupt{ false };
    // Set (under m_mutex) whenever a timer is registered or unregistered, so a processEvents()
    // call currently sleeping in wait_for() re-evaluates its wait deadline instead of sleeping
    // for the stale duration computed before the change.
    bool                    m_timersChanged{ false };
    // Set (under m_mutex) by wakeUp() and consumed by processEvents() once it returns from
    // waiting. Needed because the wait is predicate-based: without a state change to observe, a
    // bare notify_all() from wakeUp() cannot end the wait.
    bool                    m_wakeUpRequested{ false };
};

#endif // GEVENTDISPATCHERDEFAULT_H
