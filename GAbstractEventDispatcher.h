#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

class GEvent;
class GObject;

/**
 * @brief Manages the event queue and dispatches events to their respective receivers.
 */
class GAbstractEventDispatcher {
public:
    /**
     * @brief Constructs an event dispatcher.
     */
    GAbstractEventDispatcher();

    /**
     * @brief Destroys the event dispatcher and cleans up pending events.
     */
    ~GAbstractEventDispatcher();

    /**
     * @brief Starts the event loop, blocking and processing events until interrupted.
     */
    void processEvents();

    /**
     * @brief Thread-safely posts an event to the dispatcher's queue.
     * @param receiver The object that will receive the event.
     * @param event The event to be processed.
     */
    void postEvent(GObject* receiver, GEvent* event);

    /**
     * @brief Removes and deletes all pending events for the specified receiver.
     * @param receiver The receiver whose events should be removed.
     */
    void removeEventsForReceiver(GObject* receiver);

    /**
     * @brief Interrupts the event loop, causing processEvents() to return.
     */
    void interrupt();

private:
    struct EventPair {
        GObject* receiver;
        GEvent* event;
    };

    std::deque<EventPair> m_eventQueue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_interrupt{false};
};
