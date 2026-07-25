#include "GThread.h"
#include "GEventDispatcherDefault.h"
#if defined(_WIN32)
#include "GEventDispatcherWin32.h"
#elif defined(__linux__)
#include "GEventDispatcherLinux.h"
#endif

thread_local GThread* GThread::s_currentThread = nullptr;

GThread::GThread(GObject* parent) : GObject(parent) {
}

GThread::~GThread() {
    quit();
    wait();
}

void GThread::start() {
    if (m_running.load()) {
        return;
    }

    m_running.store(true);
    m_finished.store(false);
    m_exiting.store(false);

    m_thread = std::make_unique<std::thread>([this]() {
        s_currentThread = this;
        this->moveToThread(this);

        if (!m_dispatcher.load()) {
#if defined(_WIN32)
            m_dispatcher.store(new GEventDispatcherWin32());
#elif defined(__linux__)
            m_dispatcher.store(new GEventDispatcherLinux());
#else
            m_dispatcher.store(new GEventDispatcherDefault());
#endif
            m_ownsDispatcher = true;
        }

        started.emit();

        this->run();

        finished.emit();

        if (m_ownsDispatcher) {
            GAbstractEventDispatcher* disp = m_dispatcher.exchange(nullptr);
            delete disp;
            m_ownsDispatcher = false;
        }

        m_running.store(false);
        m_finished.store(true);
        {
            std::lock_guard<std::mutex> lock(m_waitMutex);
            m_waitCv.notify_all();
        }
        s_currentThread = nullptr;
    });
}

void GThread::quit() {
    exit(0);
}

void GThread::exit(int returnCode) {
    m_exitCode.store(returnCode);
    m_exiting.store(true);
    GAbstractEventDispatcher* dispatcher = m_dispatcher.load();
    if (dispatcher) {
        dispatcher->interrupt();
        dispatcher->wakeUp();
    }
}

bool GThread::wait(unsigned long time) {
    if (m_finished.load()) {
        if (m_thread && m_thread->joinable()) {
            m_thread->join();
        }
        return true;
    }

    if (!m_thread || !m_thread->joinable()) {
        return true;
    }

    if (time == ULONG_MAX) {
        m_thread->join();
        return true;
    } else {
        std::unique_lock<std::mutex> lock(m_waitMutex);
        bool completed = m_waitCv.wait_for(lock, std::chrono::milliseconds(time), [this] {
            return m_finished.load();
        });
        if (completed && m_thread->joinable()) {
            m_thread->join();
        }
        return completed;
    }
}

bool GThread::isRunning() const {
    return m_running.load();
}

bool GThread::isFinished() const {
    return m_finished.load();
}

GThread* GThread::currentThread() {
    return s_currentThread;
}

GAbstractEventDispatcher* GThread::eventDispatcher() const {
    return m_dispatcher.load();
}

void GThread::setEventDispatcher(GAbstractEventDispatcher* dispatcher) {
    if (m_ownsDispatcher) {
        GAbstractEventDispatcher* oldDisp = m_dispatcher.exchange(dispatcher);
        delete oldDisp;
        m_ownsDispatcher = false;
    } else {
        m_dispatcher.store(dispatcher);
    }
}

void GThread::run() {
    exec();
}

int GThread::exec() {
    GAbstractEventDispatcher* dispatcher = m_dispatcher.load();
    while (!m_exiting.load() && dispatcher) {
        dispatcher->processEvents();
        dispatcher = m_dispatcher.load();
    }
    return m_exitCode.load();
}
