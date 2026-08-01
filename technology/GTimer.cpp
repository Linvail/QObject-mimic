#include "GTimer.h"

GTimer::GTimer()
    : GObject()
{
}

GTimer::~GTimer()
{
    stop();
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
    stop();
    m_interval = msec;
    m_timerId = startTimer(m_interval);
    m_active = (m_timerId != -1);
}

void GTimer::start()
{
    start(m_interval);
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
    if (event && event->timerId() == m_timerId)
    {
        timeout.emit();
        if (m_singleShot)
        {
            stop();
        }
    }
}
