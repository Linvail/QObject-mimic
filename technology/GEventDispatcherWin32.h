#ifndef GEVENTDISPATCHERWIN32_H
#define GEVENTDISPATCHERWIN32_H

#include "GEventDispatcherDefault.h"

/**
 * @brief Win32 concrete implementation of GAbstractEventDispatcher.
 *
 * Inherits default cross-platform behavior and allows Win32-specific event loop handling.
 * All public methods are thread-safe.
 */
class GEventDispatcherWin32 : public GEventDispatcherDefault
{
public:
    /**
     * @brief Constructs a new GEventDispatcherWin32 instance.
     */
    GEventDispatcherWin32();

    /**
     * @brief Destroys the GEventDispatcherWin32 instance.
     */
    virtual ~GEventDispatcherWin32() override;
};

#endif // GEVENTDISPATCHERWIN32_H
