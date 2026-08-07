#include "GTimer.h"

namespace QtLikeSignal
{
    //! Constructs a timer.
    GTimer::GTimer()
        : GObject()
    {
    }

    //! Destroys the timer.
    GTimer::~GTimer()
    {
        // Matches QTimer::~QTimer(), which likewise stops a still-running timer. Note the consequence
        // Qt shares: killTimer() is thread-confined, so destroying an *active* timer from a thread
        // other than its own warns. That is a genuine misuse signal, not noise -- destroy the timer on
        // the thread it lives in. Nothing leaks either way: ~GObject() calls removeEventsForReceiver(),
        // which strips this object's timer registrations from the dispatcher regardless.
        if( mActive )
        {
            stop();
        }
    }

    //! Gets the timer interval in milliseconds.
    int GTimer::interval() const
    {
        return mInterval;
    }

    //! Sets the timer interval in milliseconds.
    void GTimer::setInterval
        (
        int aMsec  //!< Interval in milliseconds.
        )
    {
        mInterval = aMsec;
    }

    //! Checks if the timer is currently active (running).
    bool GTimer::isActive() const
    {
        return mActive;
    }

    //! Checks if the timer is single-shot.
    bool GTimer::isSingleShot() const
    {
        return mSingleShot;
    }

    //! Sets whether the timer is single-shot.
    void GTimer::setSingleShot
        (
        bool aSingleShot  //!< True for single-shot, false for periodic.
        )
    {
        mSingleShot = aSingleShot;
    }

    //! Gets the unique ID of the internal timer, or -1 if inactive.
    int GTimer::timerId() const
    {
        return mTimerId;
    }

    //! Starts or restarts the timer with specified interval in milliseconds.
    void GTimer::start
        (
        int aMsec  //!< Interval in milliseconds.
        )
    {
        mInterval = aMsec;
        start();
    }

    //! Starts or restarts the timer using the existing interval.
    //!
    //! **Must be called from this timer's own thread**; see start(int).
    void GTimer::start()
    {
        stop();
        mTimerId = startTimer( mInterval );
        mActive = ( mTimerId != -1 );
    }

    //! Stops the timer.
    //!
    //! **Must be called from this timer's own thread**, because it goes through
    //! GObject::killTimer(). Calling it from elsewhere warns and leaves the timer running.
    void GTimer::stop()
    {
        if( mActive && mTimerId != -1 )
        {
            killTimer( mTimerId );
            mTimerId = -1;
            mActive = false;
        }
    }

    //! Internal timer event handler.
    void GTimer::timerEvent
        (
        GTimerEvent* aEvent  //!< Timer event.
        )
    {
        if( !aEvent || aEvent->timerId() != mTimerId )
        {
            return;
        }

        // Stop before emitting, matching QTimer::timerEvent()'s ordering: a slot must not observe a
        // single-shot timer as still active, and emitting last means none of our own code touches this
        // object after user code has run.
        //
        // Note this narrows -- but does not eliminate -- the hazard of a directly-connected slot
        // deleting the timer: emit() is still executing inside the GSignal member of the object being
        // destroyed. Use deleteLater() from a timeout slot; deleting outright is not supported.
        if( mSingleShot )
        {
            stop();
        }

        timeout.emit();
    }
}
