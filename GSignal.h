#pragma once

#include "GGlobal.h"
#include "GEvent.h"
#include "GCoreApplication.h"
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <algorithm>

/**
 * @brief A template class representing a signal that can be emitted to trigger connected slots.
 * @tparam Args The types of arguments the signal passes to its connected slots.
 */
template <typename... Args>
class GSignal {
public:
    /**
     * @brief Internal structure representing a connection to a receiver.
     */
    struct Connection {
        int id;
        GObject* receiver;
        std::function<void(Args...)> slot;
        G::ConnectionType type;
    };

    /**
     * @brief Shared state of the signal to ensure thread-safe destruction and disconnection.
     */
    struct State {
        std::vector<Connection> connections;
        std::mutex mutex;
        int nextId = 0;
    };

    /**
     * @brief Constructs a new signal.
     */
    GSignal() : m_state(std::make_shared<State>()) {}

    /**
     * @brief Destroys the signal, safely clearing all connections.
     */
    ~GSignal() {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->connections.clear();
    }

    /**
     * @brief Connects this signal to a member function of a receiver object.
     * @tparam Receiver The type of the receiver object.
     * @param receiver The object that will receive the signal.
     * @param slotFunc The member function to call when the signal is emitted.
     * @param type The type of connection to establish.
     */
    template <typename Receiver>
    void connect(Receiver* receiver, void (Receiver::*slotFunc)(Args...), G::ConnectionType type = G::AutoConnection) {
        if (!receiver) return;

        int id;
        std::shared_ptr<State> state = m_state;

        auto slotLambda = [receiver, slotFunc](Args... args) {
            (receiver->*slotFunc)(args...);
        };

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            id = state->nextId++;
            state->connections.push_back({id, receiver, slotLambda, type});
        }

        std::weak_ptr<State> weakState = state;
        receiver->addCleanupCallback([weakState, id]() {
            if (auto s = weakState.lock()) {
                std::lock_guard<std::mutex> stateLock(s->mutex);
                auto it = std::remove_if(s->connections.begin(), s->connections.end(),
                    [id](const Connection& c) { return c.id == id; });
                if (it != s->connections.end()) {
                    s->connections.erase(it, s->connections.end());
                }
            }
        });
    }

    /**
     * @brief Emits the signal by calling the function call operator.
     * @param args The arguments to pass to the connected slots.
     */
    void operator()(Args... args) {
        emit(args...);
    }

    /**
     * @brief Emits the signal, invoking all connected slots with the provided arguments.
     * @param args The arguments to pass to the connected slots.
     */
    void emit(Args... args) {
        std::vector<Connection> connectionsCopy;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            connectionsCopy = m_state->connections;
        }

        for (const auto& conn : connectionsCopy) {
            G::ConnectionType activeType = conn.type;

            if (activeType == G::AutoConnection) {
                GThread* currentThread = GThread::currentThread();
                GThread* receiverThread = conn.receiver ? conn.receiver->thread() : nullptr;

                if (currentThread == receiverThread) {
                    activeType = G::DirectConnection;
                } else {
                    activeType = G::QueuedConnection;
                }
            }

            if (activeType == G::QueuedConnection) {
                auto slot = conn.slot;
                auto boundSlot = [slot, args...]() {
                    slot(args...);
                };

                auto* event = new GMetaCallEvent(boundSlot);
                GCoreApplication::postEvent(conn.receiver, event);
            } else {
                conn.slot(args...);
            }
        }
    }

private:
    std::shared_ptr<State> m_state;
};
