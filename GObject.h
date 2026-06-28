#pragma once

#include "GGlobal.h"
#include <vector>
#include <functional>
#include <mutex>
#include <memory>

class GThread;
class GEvent;

/**
 * @brief Base class for all objects participating in the signal-slot mechanism.
 */
class GObject {
public:
    /**
     * @brief Constructs an object with the given parent.
     * @param parent The parent object.
     */
    GObject(GObject* parent = nullptr);

    /**
     * @brief Destroys the object and triggers all registered cleanup callbacks.
     */
    virtual ~GObject();

    /**
     * @brief Gets the thread affinity of this object.
     * @return A pointer to the thread this object lives in.
     */
    GThread* thread() const;

    /**
     * @brief Changes the thread affinity of this object.
     * @param thread The new thread this object will live in.
     */
    void moveToThread(GThread* thread);

    /**
     * @brief Handles custom events sent to this object.
     * @param event The event to handle.
     */
    virtual void customEvent(GEvent* event);

    /**
     * @brief Registers a callback to be executed when this object is destroyed.
     * @param callback The function to execute upon destruction.
     */
    void addCleanupCallback(std::function<void()> callback);

    /**
     * @brief Gets the weak pointer tracking the lifetime of this object.
     * @return A weak pointer to this object's life token.
     */
    std::weak_ptr<int> objectLife() const { return m_life; }

    /**
     * @brief Connects a signal to a slot on a receiver object.
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The member function to call when the signal is emitted.
     * @param type The type of connection.
     * @return A handle representing the connection.
     */
    template <typename Signal, typename Receiver, typename Slot>
    static G::ConnectionHandle connect(Signal& signal, Receiver* receiver, Slot slot, G::ConnectionType type = G::AutoConnection) {
        if (receiver) {
            return signal.connect(receiver, slot, type);
        }
        return {};
    }

private:
    std::shared_ptr<int> m_life;
    GThread* m_thread;
    GObject* m_parent;
    std::vector<std::function<void()>> m_cleanupCallbacks;
    std::mutex m_cleanupMutex;
};
