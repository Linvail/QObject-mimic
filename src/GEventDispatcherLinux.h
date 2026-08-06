#ifndef GEVENTDISPATCHERLINUX_H
#define GEVENTDISPATCHERLINUX_H

#include "GEventDispatcherDefault.h"

namespace QtLikeSignal
{
    /**
     * @brief Linux concrete implementation of GAbstractEventDispatcher.
     *
     * Inherits default cross-platform behavior and allows Linux epoll/POSIX event loop handling.
     * All public methods are thread-safe.
     */
    class GEventDispatcherLinux : public GEventDispatcherDefault
    {
    public:
        /**
         * @brief Constructs a new GEventDispatcherLinux instance.
         */
        GEventDispatcherLinux();

        /**
         * @brief Destroys the GEventDispatcherLinux instance.
         */
        virtual ~GEventDispatcherLinux() override;

    };
}

#endif // GEVENTDISPATCHERLINUX_H
