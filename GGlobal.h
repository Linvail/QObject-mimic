#pragma once

#include <memory>
#include <boost/signals2.hpp>

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
    using ConnectionHandle = boost::signals2::connection;
}
