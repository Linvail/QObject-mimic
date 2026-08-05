#ifndef GEVENT_H
#define GEVENT_H

#include <functional>
#include <utility>

/**
 * @brief Base class for all events in the event loop.
 *
 * The event set is closed: these three types are the only ones the queue ever carries, and all of
 * them are posted by GObject's own internals. There is deliberately no user/custom event type and
 * no way to post an arbitrary event for an arbitrary receiver -- this is a queued signal-slot
 * mechanism, not a general event system.
 */
class GEvent
{
public:
    /**
     * @brief The core event types.
     */
    enum Type
    {
        MetaCall       = 1,
        Timer          = 2,
        DeferredDelete = 3
    };
    /**
     * @brief Virtual destructor.
     */
    virtual ~GEvent() = default;

    /**
     * @brief Gets the type of the event.
     * @return The event type. Thread-safe.
     */
    Type type() const
    {
        return m_type;
    }

protected:
    /**
     * @brief Constructs an event of the specified type.
     *
     * Protected: GEvent is only ever instantiated through one of the concrete subclasses below.
     * @param type The type of the event.
     */
    GEvent
        (
        Type type
        )
        : m_type( type )
    {
    }

private:
    Type m_type;
};

/**
 * @brief An event that encapsulates a function call across threads.
 *
 * Entirely internal: it wraps an arbitrary callable, so both creating one and firing one are
 * restricted to GObject, which is the only code that queues or dispatches metacalls.
 */
class GMetaCallEvent : public GEvent
{
private:
    /**
     * @brief Constructs a metacall event with the given callback.
     * @param callback The function to execute.
     */
    GMetaCallEvent
        (
        std::function<void()> callback
        )
        : GEvent( MetaCall )
        , m_callback( std::move( callback ) )
    {
    }

    /**
     * @brief Executes the stored function call.
     */
    void placeMetaCall() const
    {
        if( m_callback )
        {
            m_callback();
        }
    }

    std::function<void()> m_callback;

    friend class GObject;
};

/**
 * @brief Event sent when a timer expires.
 */
class GTimerEvent : public GEvent
{
public:
    /**
     * @brief Constructs a timer event with a given timer ID.
     *
     * Left public, unlike the other two event types: timerEvent() is a supported override point,
     * so synthesizing a GTimerEvent to drive an override directly (as tests do) is legitimate.
     * Constructing one grants no privileged capability -- it cannot be posted to any queue.
     * @param timerId The unique identifier of the expired timer.
     */
    GTimerEvent
        (
        int timerId
        )
        : GEvent( Timer )
        , m_timerId( timerId )
    {
    }

    /**
     * @brief Gets the timer ID associated with this event.
     * @return The timer ID. Thread-safe.
     */
    int timerId() const
    {
        return m_timerId;
    }

private:
    int m_timerId;
};

/**
 * @brief Event sent to delete an object asynchronously.
 *
 * Internal: only GObject::deleteLater() creates one. Delivering this event destroys the receiver,
 * so it must not be constructible by outside code.
 */
class GDeferredDeleteEvent : public GEvent
{
private:
    /**
     * @brief Constructs a deferred delete event.
     */
    GDeferredDeleteEvent()
        : GEvent( DeferredDelete )
    {
    }

    friend class GObject;
};

#endif  // GEVENT_H
