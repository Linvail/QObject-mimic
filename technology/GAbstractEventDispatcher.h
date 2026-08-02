#ifndef GABSTRACTEVENTDISPATCHER_H
#define GABSTRACTEVENTDISPATCHER_H

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <vector>

class GEvent;
class GObject;

/**
 * @brief Abstract base class for event dispatchers managing event queues and timer dispatching.
 *
 * All public methods must be thread-safe as they can be invoked across threads.
 */
class GAbstractEventDispatcher
{
public:
    /**
     * @brief A timer registration, as handed between dispatchers when an object changes thread.
     */
    struct TimerRegistration
    {
        /** @brief The timer's unique id, preserved across the move. */
        int timerId;
        /** @brief The interval the timer was registered with, in milliseconds. */
        int intervalMs;
    };

    /**
     * @brief Constructs an event dispatcher.
     */
    GAbstractEventDispatcher();

    /**
     * @brief Destroys the event dispatcher and cleans up pending events.
     */
    virtual ~GAbstractEventDispatcher();

    /**
     * @brief Processes pending events and expired timers once, without infinite looping.
     * @return True if events were processed, false otherwise.
     *
     * Note: Thread-safe invocation within the event loop of the owning thread.
     */
    virtual bool processEvents() = 0;

    /**
     * @brief Wakes up the event loop if it is waiting for events.
     *
     * Note: Thread-safe.
     */
    virtual void wakeUp() = 0;

    /**
     * @brief Interrupts the event loop execution.
     *
     * Note: Thread-safe.
     */
    virtual void interrupt() = 0;

    /**
     * @brief Dispatches any pending deferred-delete events, destroying their receivers.
     *
     * Called when an event loop is shutting down, before the dispatcher itself goes away. Without
     * it, every object that called deleteLater() before the loop stopped is leaked: the
     * destructor can free the queued events but has no way to free the objects they target.
     * Mirrors Qt, which drains DeferredDelete in QThreadPrivate::finish() for the same reason.
     *
     * Public rather than protected because, like processEvents()/wakeUp()/interrupt(), it drives
     * the loop as a whole and cannot be aimed at a particular receiver.
     *
     * Note: Thread-safe, but intended to run on the dispatcher's own thread -- it destroys
     * objects, and their destructors expect to run there.
     */
    virtual void processDeferredDeletes() = 0;

protected:
    // The methods below all target a *specific* receiver object, so exposing them publicly would
    // let any caller inject events or timers on another object's behalf. They exist solely for
    // GObject's own internals (deleteLater(), startTimer()/killTimer(), dispatchMetaCall(), and
    // ~GObject()), which reach them through the friend declaration at the end of this class.
    // processEvents()/wakeUp()/interrupt() stay public: they drive or stop the loop as a whole
    // and cannot be aimed at a particular object.

    /**
     * @brief Registers a timer for the given object.
     * @param timerId Unique timer identifier.
     * @param interval Interval in milliseconds.
     * @param object Target object to receive GTimerEvent.
     *
     * Note: Thread-safe.
     */
    virtual void registerTimer(int timerId, int interval, GObject* object) = 0;

    /**
     * @brief Unregisters a timer by ID.
     * @param timerId Unique timer identifier.
     * @return True if the timer was found and removed.
     *
     * Note: Thread-safe.
     */
    virtual bool unregisterTimer(int timerId) = 0;

    /**
     * @brief Thread-safely posts an event to the dispatcher's queue.
     * @param receiver The object that will receive the event.
     * @param event The event to be processed.
     *
     * Note: Thread-safe.
     */
    virtual void postEvent(GObject* receiver, GEvent* event) = 0;

    /**
     * @brief Removes and deletes all pending events for the specified receiver.
     * @param receiver The receiver whose events should be removed.
     *
     * Note: Thread-safe.
     */
    virtual void removeEventsForReceiver(GObject* receiver) = 0;

    /**
     * @brief Unregisters the receiver's timers and returns them so they can be re-registered.
     *
     * Used by GObject::moveToThread() to carry active timers across to the destination thread's
     * dispatcher. The ids are handed back rather than released, so the same timer id stays valid
     * after the move and a GTimer's cached id still matches the events it receives -- the same
     * reason Qt notes "do not release our timer ids back to the pool" when it does this.
     * @param receiver The receiver whose timers should be taken.
     * @return The removed registrations; empty if the receiver had none.
     *
     * Note: Thread-safe.
     */
    virtual std::vector<TimerRegistration> takeTimersForReceiver(GObject* receiver) = 0;

    friend class GObject;
};

#endif // GABSTRACTEVENTDISPATCHER_H
