#include "GEventDispatcherDefault.h"
#include "GEvent.h"
#include "GObject.h"
#include <algorithm>

GEventDispatcherDefault::GEventDispatcherDefault() = default;

GEventDispatcherDefault::~GEventDispatcherDefault() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_eventQueue.empty()) {
        delete m_eventQueue.front().event;
        m_eventQueue.pop_front();
    }
}

bool GEventDispatcherDefault::processEvents() {
    if (m_interrupt) {
        return false;
    }

    std::vector<EventPair> eventsToProcess;
    std::vector<EventPair> timerEventsToProcess;
    std::chrono::milliseconds maxWait{100};

    {
        std::unique_lock<std::mutex> lock(m_mutex);

        auto now = std::chrono::steady_clock::now();

        // Collect expired timers
        for (auto& t : m_timers) {
            if (now >= t.nextFire) {
                timerEventsToProcess.push_back({t.receiver, new GTimerEvent(t.timerId)});
                t.nextFire = now + std::chrono::milliseconds(t.intervalMs);
            }
        }

        // Determine wait time for next timer if no events present
        if (m_eventQueue.empty() && timerEventsToProcess.empty()) {
            if (!m_timers.empty()) {
                auto minFire = m_timers.front().nextFire;
                for (const auto& t : m_timers) {
                    if (t.nextFire < minFire) {
                        minFire = t.nextFire;
                    }
                }
                if (minFire > now) {
                    maxWait = std::chrono::duration_cast<std::chrono::milliseconds>(minFire - now);
                } else {
                    maxWait = std::chrono::milliseconds(0);
                }
            }

            m_cv.wait_for(lock, maxWait, [this] {
                return !m_eventQueue.empty() || m_interrupt;
            });
        }

        if (m_interrupt) {
            return false;
        }

        // Drain current queued events
        while (!m_eventQueue.empty()) {
            eventsToProcess.push_back(m_eventQueue.front());
            m_eventQueue.pop_front();
        }
    }

    bool processedAny = false;

    // Dispatch queued events
    for (const auto& ep : eventsToProcess) {
        if (ep.receiver && ep.event) {
            ep.receiver->event(ep.event);
            delete ep.event;
            processedAny = true;
        }
    }

    // Dispatch timer events
    for (const auto& ep : timerEventsToProcess) {
        if (ep.receiver && ep.event) {
            ep.receiver->event(ep.event);
            delete ep.event;
            processedAny = true;
        }
    }

    return processedAny;
}

void GEventDispatcherDefault::registerTimer(int timerId, int interval, GObject* object) {
    if (!object || interval < 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    TimerData td;
    td.timerId = timerId;
    td.intervalMs = interval;
    td.receiver = object;
    td.nextFire = now + std::chrono::milliseconds(interval);

    for (auto& t : m_timers) {
        if (t.timerId == timerId) {
            t = td;
            m_cv.notify_all();
            return;
        }
    }
    m_timers.push_back(td);
    m_cv.notify_all();
}

bool GEventDispatcherDefault::unregisterTimer(int timerId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::remove_if(m_timers.begin(), m_timers.end(), [timerId](const TimerData& td) {
        return td.timerId == timerId;
    });
    if (it != m_timers.end()) {
        m_timers.erase(it, m_timers.end());
        return true;
    }
    return false;
}

void GEventDispatcherDefault::postEvent(GObject* receiver, GEvent* event) {
    if (!receiver || !event) {
        delete event;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_eventQueue.push_back({receiver, event});
    }
    m_cv.notify_all();
}

void GEventDispatcherDefault::removeEventsForReceiver(GObject* receiver) {
    if (!receiver) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto itQueue = std::remove_if(m_eventQueue.begin(), m_eventQueue.end(), [receiver](const EventPair& ep) {
        if (ep.receiver == receiver) {
            delete ep.event;
            return true;
        }
        return false;
    });
    m_eventQueue.erase(itQueue, m_eventQueue.end());

    auto itTimer = std::remove_if(m_timers.begin(), m_timers.end(), [receiver](const TimerData& td) {
        return td.receiver == receiver;
    });
    m_timers.erase(itTimer, m_timers.end());
}

void GEventDispatcherDefault::wakeUp() {
    m_cv.notify_all();
}

void GEventDispatcherDefault::interrupt() {
    m_interrupt = true;
    m_cv.notify_all();
}
