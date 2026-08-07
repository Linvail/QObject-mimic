#ifndef GEVENTDISPATCHERLINUX_H
#define GEVENTDISPATCHERLINUX_H

#include "GEventDispatcherDefault.h"

namespace QtLikeSignal
{
    //! Linux concrete implementation of GAbstractEventDispatcher.
    //!
    //! Inherits default cross-platform behavior and allows Linux epoll/POSIX event loop handling.
    //! All public methods are thread-safe.
    class GEventDispatcherLinux : public GEventDispatcherDefault
    {
    public:
        GEventDispatcherLinux();

        virtual ~GEventDispatcherLinux() override;

    };
}

#endif // GEVENTDISPATCHERLINUX_H
