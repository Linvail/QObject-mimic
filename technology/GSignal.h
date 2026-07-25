#pragma once

#include "GGlobal.h"
#include <boost/signals2.hpp>
#include <functional>

/**
 * @brief Simple thread-safe signal class wrapping boost::signals2::signal.
 * @tparam Args Argument types passed when emitting the signal.
 */
template <typename... Args>
class GSignal {
public:
    /**
     * @brief Constructs a new GSignal.
     */
    GSignal() = default;

    /**
     * @brief Connects a callable slot to this signal.
     * @param slot The callable slot function.
     * @return Connection handle. Thread-safe.
     */
    G::ConnectionHandle connect(std::function<void(Args...)> slot) {
        return m_signal.connect(slot);
    }

    /**
     * @brief Disconnects a connection by handle.
     * @param connection The connection handle to disconnect. Thread-safe.
     */
    void disconnect(const G::ConnectionHandle& connection) {
        connection.disconnect();
    }

    /**
     * @brief Emits the signal with the specified arguments.
     * @param args Arguments to pass to all connected slots. Thread-safe.
     */
    void emit(Args... args) {
        m_signal(args...);
    }

    /**
     * @brief Function call operator to emit the signal.
     * @param args Arguments to pass to all connected slots. Thread-safe.
     */
    void operator()(Args... args) {
        m_signal(args...);
    }

private:
    boost::signals2::signal<void(Args...)> m_signal;
};
