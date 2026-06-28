#include "GCoreApplication.h"
#include "GAbstractEventDispatcher.h"
#include "GEvent.h"

GCoreApplication* GCoreApplication::s_instance = nullptr;

GCoreApplication::GCoreApplication(int& argc, char** argv) : GObject(nullptr) {
    s_instance = this;

    m_dispatcher = std::make_unique<GAbstractEventDispatcher>();
    m_mainThread = std::make_unique<GThread>();

    // Hijack the current thread as main thread
    m_mainThread->m_dispatcher = m_dispatcher.get();
    GThread::s_currentThread = m_mainThread.get();

    this->moveToThread(m_mainThread.get());
}

GCoreApplication::~GCoreApplication() {
    this->moveToThread(nullptr);
    m_mainThread->m_dispatcher = nullptr;
    GThread::s_currentThread = nullptr;
    s_instance = nullptr;
}

GCoreApplication* GCoreApplication::instance() {
    return s_instance;
}

void GCoreApplication::postEvent(GObject* receiver, GEvent* event) {
    if (!receiver) {
        delete event;
        return;
    }

    GThread* thread = receiver->thread();
    if (thread && thread->eventDispatcher()) {
        thread->eventDispatcher()->postEvent(receiver, event);
    } else {
        delete event;
    }
}

int GCoreApplication::exec() {
    if (m_dispatcher) {
        m_dispatcher->processEvents();
    }
    return 0;
}

void GCoreApplication::quit() {
    if (m_dispatcher) {
        m_dispatcher->interrupt();
    }
}
