#ifndef EVENTDISPATCHERWIN32_H
#define EVENTDISPATCHERWIN32_H

#include "EventDispatcherDefault.h"

namespace QtLikeSignal
{
    //! Win32 concrete implementation of AbstractEventDispatcher.
    //!
    //! Inherits default cross-platform behavior and allows Win32-specific event loop handling.
    //! All public methods are thread-safe.
    class EventDispatcherWin32 : public EventDispatcherDefault
    {
    public:
        EventDispatcherWin32();

        virtual ~EventDispatcherWin32() override;

    };
}

#endif // EVENTDISPATCHERWIN32_H
