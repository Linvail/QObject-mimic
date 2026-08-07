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
        ConnectionHandle connect
            (
            std::function<void( Args... )> aSlot  //!< The callable slot function.
            )
        {
            return mSignal.connect( aSlot );
        }

        //! Disconnects a connection by handle. Thread-safe.
        void disconnect
            (
            const ConnectionHandle& aConnection  //!< The connection handle to disconnect.
            )
        {
            aConnection.disconnect();
        }

        //! Emits the signal with the specified arguments. Thread-safe.
        void emit
            (
            Args... aArgs  //!< Arguments to pass to all connected slots.
            )
        {
            mSignal( aArgs ... );
        }

        //! Function call operator to emit the signal. Thread-safe.
        void operator()
            (
            Args... aArgs  //!< Arguments to pass to all connected slots.
            )
        {
            mSignal( aArgs ... );
        }

    private:
        boost::signals2::signal<void( Args... )> mSignal;
    };

    //! Specialization of IsGSignal matching any GSignal<Args...>, so IsGSignal<T>::value is
    //! true precisely when T is a signal.
    template<typename ... Args>
    struct IsGSignal<GSignal<Args...> > : std::true_type
    {
    };
}

#endif // GSIGNAL_H
