#include "GObject.h"
#include "GThread.h"
#include "GEvent.h"
#include "GAbstractEventDispatcher.h"

GObject::GObject(GObject* parent) : m_parent(parent) {
    m_thread = GThread::currentThread();
}

GObject::~GObject() {
    {
        std::lock_guard<std::mutex> lock(m_cleanupMutex);
        for (auto& cb : m_cleanupCallbacks) {
            cb();
        }
    }

    if (m_thread) {
        GAbstractEventDispatcher* dispatcher = m_thread->eventDispatcher();
        if (dispatcher) {
            dispatcher->removeEventsForReceiver(this);
        }
    }
}

GThread* GObject::thread() const {
    return m_thread;
}

void GObject::moveToThread(GThread* thread) {
    m_thread = thread;
}

void GObject::customEvent(GEvent* event) {
    if (event->type() == GEvent::MetaCall) {
        auto* metaEvent = static_cast<GMetaCallEvent*>(event);
        metaEvent->placeMetaCall();
    }
}

void GObject::addCleanupCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_cleanupMutex);
    m_cleanupCallbacks.push_back(std::move(callback));
}
