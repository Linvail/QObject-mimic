#pragma once

#include <memory>

namespace G {
    /**
     * @brief Specifies the type of a signal-slot connection.
     */
    enum ConnectionType {
        /**
         * @brief Automatically determines the connection type based on thread affinity.
         *
         * If the receiver lives in the thread that emits the signal, DirectConnection is used.
         * Otherwise, QueuedConnection is used.
         */
        AutoConnection,

        /**
         * @brief The slot is invoked immediately when the signal is emitted.
         *
         * The slot is executed in the signaling thread.
         */
        DirectConnection,

        /**
         * @brief The slot is invoked when control returns to the event loop of the receiver's thread.
         *
         * The slot is executed in the receiver's thread.
         */
        QueuedConnection
    };

    /**
     * @brief A handle representing a signal-slot connection.
     */
    struct ConnectionHandle {
        /**
         * @brief The unique ID of the connection.
         */
        int id = -1;

        /**
         * @brief Weak pointer to the signal's shared state.
         */
        std::weak_ptr<void> signalState;

        /**
         * @brief Checks if the handle is valid.
         * @return True if valid, false otherwise.
         */
        bool isValid() const { return id != -1 && !signalState.expired(); }

        /**
         * @brief Equality operator.
         * @param other The other handle to compare to.
         * @return True if equal, false otherwise.
         */
        bool operator==(const ConnectionHandle& other) const { return id == other.id; }

        /**
         * @brief Inequality operator.
         * @param other The other handle to compare to.
         * @return True if not equal, false otherwise.
         */
        bool operator!=(const ConnectionHandle& other) const { return id != other.id; }
    };
}
