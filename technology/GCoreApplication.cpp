#include "GCoreApplication.h"
#include "GAbstractEventDispatcher.h"
#include "GEventDispatcherDefault.h"
#if defined(_WIN32)
#include "GEventDispatcherWin32.h"
#elif defined(__linux__)
#include "GEventDispatcherLinux.h"
#endif
#include "GEvent.h"

GCoreApplication* GCoreApplication::s_instance = nullptr;

GCoreApplication::GCoreApplication(int& argc, char** argv) : GObject(nullptr) {
    (void)argc;
    (void)argv;
    s_instance = this;

#if defined(_WIN32)
    m_dispatcher = std::make_unique<GEventDispatcherWin32>();
#elif defined(__linux__)
    m_dispatcher = std::make_unique<GEventDispatcherLinux>();
#else
    m_dispatcher = std::make_unique<GEventDispatcherDefault>();
#endif
    m_mainThread = std::make_unique<GThread>();

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
    m_exiting.store(false);
    if (m_dispatcher) {
        while (!m_exiting.load()) {
            m_dispatcher->processEvents();
        }
    }
    return 0;
}

void GCoreApplication::quit() {
    m_exiting.store(true);
    if (m_dispatcher) {
        m_dispatcher->interrupt();
        m_dispatcher->wakeUp();
    }
}
