#include "GTimer.h"

GTimer::GTimer()
    : GObject()
{
}

GTimer::~GTimer()
{
    // Matches QTimer::~QTimer(), which likewise stops a still-running timer. Note the consequence
    // Qt shares: killTimer() is thread-confined, so destroying an *active* timer from a thread
    // other than its own warns. That is a genuine misuse signal, not noise -- destroy the timer on
    // the thread it lives in. Nothing leaks either way: ~GObject() calls removeEventsForReceiver(),
    // which strips this object's timer registrations from the dispatcher regardless.
    if (m_active)
    {
        stop();
    }
}

int GTimer::interval() const
{
    return m_interval;
}

void GTimer::setInterval(int msec)
{
    m_interval = msec;
}

bool GTimer::isActive() const
{
    return m_active;
}

bool GTimer::isSingleShot() const
{
    return m_singleShot;
}

void GTimer::setSingleShot(bool singleShot)
{
    m_singleShot = singleShot;
}

int GTimer::timerId() const
{
    return m_timerId;
}

void GTimer::start(int msec)
{
    m_interval = msec;
    start();
}

void GTimer::start()
{
    stop();
    m_timerId = startTimer(m_interval);
    m_active = (m_timerId != -1);
}

void GTimer::stop()
{
    if (m_active && m_timerId != -1)
    {
        killTimer(m_timerId);
        m_timerId = -1;
        m_active = false;
    }
}

void GTimer::timerEvent(GTimerEvent* event)
{
    if (!event || event->timerId() != m_timerId)
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
    if (m_singleShot)
    {
        stop();
    }

    timeout.emit();
}
