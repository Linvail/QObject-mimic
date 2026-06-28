#include "GAbstractEventDispatcher.h"
#include "GEvent.h"
#include "GObject.h"
#include <algorithm>

GAbstractEventDispatcher::GAbstractEventDispatcher() = default;

GAbstractEventDispatcher::~GAbstractEventDispatcher() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_eventQueue.empty()) {
        delete m_eventQueue.front().event;
        m_eventQueue.pop_front();
    }
}

void GAbstractEventDispatcher::processEvents() {
    while (!m_interrupt) {
        EventPair ep;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_eventQueue.empty() || m_interrupt; });

            if (m_interrupt) {
                break;
            }

            ep = m_eventQueue.front();
            m_eventQueue.pop_front();
        }

        if (ep.receiver && ep.event) {
            ep.receiver->customEvent(ep.event);
            delete ep.event;
        }
    }
    m_interrupt = false;
}

void GAbstractEventDispatcher::postEvent(GObject* receiver, GEvent* event) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_eventQueue.push_back({receiver, event});
    }
    m_cv.notify_one();
}

/**
 * @brief Removes and deletes all pending events for the specified receiver.
 * @param receiver The receiver whose events should be removed.
 */
void GAbstractEventDispatcher::removeEventsForReceiver(GObject* receiver) {
    if (!receiver) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::remove_if(m_eventQueue.begin(), m_eventQueue.end(),
        [receiver](const EventPair& ep) {
            if (ep.receiver == receiver) {
                delete ep.event;
                return true;
            }
            return false;
        });
    m_eventQueue.erase(it, m_eventQueue.end());
}

void GAbstractEventDispatcher::interrupt() {
    m_interrupt = true;
    m_cv.notify_all();
}
