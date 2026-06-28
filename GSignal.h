#pragma once

#include "GGlobal.h"
#include "GEvent.h"
#include "GCoreApplication.h"
#include "GObject.h"
#include "GThread.h"
#include <boost/signals2.hpp>
#include <memory>

/**
 * @brief A signal class wrapping boost::signals2::signal that supports thread-safe connections and lifetime tracking.
 * @tparam Args The argument types passed by the signal.
 */
template <typename... Args>
class GSignal : public boost::signals2::signal<void(Args...)> {
public:
    /**
     * @brief Constructs a new GSignal.
     */
    GSignal() = default;

    /**
     * @brief Connects this signal to a member function of a receiver object with lifetime and thread checks.
     * @tparam Receiver The type of the receiver object.
     * @param receiver The object that will receive the signal.
     * @param slotFunc The member function to call when the signal is emitted.
     * @param type The type of connection to establish.
     * @return A handle representing the connection.
     */
    template <typename Receiver>
    boost::signals2::connection connect(Receiver* receiver, void (Receiver::*slotFunc)(Args...), G::ConnectionType type = G::AutoConnection) {
        if (!receiver) {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();
        GThread* receiverThread = receiver->thread();

        auto wrapper = [weakLife, receiver, slotFunc, receiverThread, type](Args... args) {
            // Check if receiver is still alive
            auto life = weakLife.lock();
            if (!life) {
                return;
            }

            G::ConnectionType activeType = type;
            if (activeType == G::AutoConnection) {
                GThread* currentThread = GThread::currentThread();
                if (currentThread == receiverThread) {
                    activeType = G::DirectConnection;
                } else {
                    activeType = G::QueuedConnection;
                }
            }

            if (activeType == G::QueuedConnection) {
                // Post the event to the receiver's thread event loop
                auto boundSlot = [weakLife, receiver, slotFunc, args...]() {
                    if (auto lifeCheck = weakLife.lock()) {
                        (receiver->*slotFunc)(args...);
                    }
                };
                auto* event = new GMetaCallEvent(boundSlot);
                GCoreApplication::postEvent(receiver, event);
            } else {
                // Execute directly in the current thread
                (receiver->*slotFunc)(args...);
            }
        };

        return boost::signals2::signal<void(Args...)>::connect(wrapper);
    }

    /**
     * @brief Disconnects a signal-slot connection using the provided handle.
     * @param connection The handle representing the connection to remove.
     */
    void disconnect(const boost::signals2::connection& connection) {
        connection.disconnect();
    }

    /**
     * @brief Emits the signal by invoking all connected slots.
     * @param args The arguments to pass to the slots.
     */
    void emit(Args... args) {
        boost::signals2::signal<void(Args...)>::operator()(args...);
    }
};
