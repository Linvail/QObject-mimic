#pragma once

#include <functional>
#include <utility>

/**
 * @brief Base class for all events in the event loop.
 */
class GEvent {
public:
    /**
     * @brief The core event types.
     */
    enum Type {
        None = 0,
        MetaCall = 1,
        User = 1000
    };

    /**
     * @brief Constructs an event of the specified type.
     * @param type The type of the event.
     */
    GEvent(Type type) : m_type(type) {}
    
    /**
     * @brief Virtual destructor.
     */
    virtual ~GEvent() = default;

    /**
     * @brief Gets the type of the event.
     * @return The event type.
     */
    Type type() const { return m_type; }

private:
    Type m_type;
};

/**
 * @brief An event that encapsulates a function call across threads.
 */
class GMetaCallEvent : public GEvent {
public:
    /**
     * @brief Constructs a metacall event with the given callback.
     * @param callback The function to execute.
     */
    GMetaCallEvent(std::function<void()> callback)
        : GEvent(MetaCall), m_callback(std::move(callback)) {}

    /**
     * @brief Executes the stored function call.
     */
    void placeMetaCall() const {
        if (m_callback) {
            m_callback();
        }
    }

private:
    std::function<void()> m_callback;
};
