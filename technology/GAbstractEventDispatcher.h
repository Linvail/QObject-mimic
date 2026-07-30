#ifndef GABSTRACTEVENTDISPATCHER_H
#define GABSTRACTEVENTDISPATCHER_H

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

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
};

#endif // GABSTRACTEVENTDISPATCHER_H
