#ifndef EVENTDISPATCHERLINUX_H
#define EVENTDISPATCHERLINUX_H

#include "EventDispatcherDefault.h"

namespace QtLikeSignal
{
    //! Linux concrete implementation of AbstractEventDispatcher.
    //!
    //! Inherits default cross-platform behavior and allows Linux epoll/POSIX event loop handling.
    //! All public methods are thread-safe.
    class EventDispatcherLinux : public EventDispatcherDefault
    {
    public:
        EventDispatcherLinux();

        virtual ~EventDispatcherLinux() override;

    };
}

#endif // EVENTDISPATCHERLINUX_H
