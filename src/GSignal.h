#ifndef GSIGNAL_H
#define GSIGNAL_H

#include "GGlobal.h"
#include <boost/signals2.hpp>
#include <functional>

namespace QtLikeSignal
{
    //! Simple thread-safe signal class wrapping boost::signals2::signal. Args are the argument
    //! types passed when emitting the signal.
    template<typename ... Args>
    class GSignal
    {
    public:
        //! Constructs a new GSignal.
        GSignal() = default;

        //! Connects a callable slot to this signal. Thread-safe.
        G::ConnectionHandle connect
            (
            std::function<void( Args... )> slot  //!< The callable slot function.
            )
        {
            return m_signal.connect( slot );
        }

        //! Disconnects a connection by handle. Thread-safe.
        void disconnect
            (
            const G::ConnectionHandle& connection  //!< The connection handle to disconnect.
            )
        {
            connection.disconnect();
        }

        //! Emits the signal with the specified arguments. Thread-safe.
        void emit
            (
            Args... args  //!< Arguments to pass to all connected slots.
            )
        {
            m_signal( args ... );
        }

        //! Function call operator to emit the signal. Thread-safe.
        void operator()
            (
            Args... args  //!< Arguments to pass to all connected slots.
            )
        {
            m_signal( args ... );
        }

    private:
        boost::signals2::signal<void( Args... )> m_signal;
    };

    namespace G
    {
        //! Specialization of IsGSignal matching any GSignal<Args...>, so IsGSignal<T>::value is
        //! true precisely when T is a signal.
        template<typename ... Args>
        struct IsGSignal<GSignal<Args...> > : std::true_type
        {
        };
    }
}

#endif // GSIGNAL_H
