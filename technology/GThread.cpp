#include "GThread.h"

#include "GEventDispatcherDefault.h"
#if defined(_WIN32)
    #include "GEventDispatcherWin32.h"
#elif defined(__linux__)
    #include "GEventDispatcherLinux.h"
#endif

thread_local GThread* GThread::s_currentThread = nullptr;

GThread::GThread()
    : GObject()
{
    m_data = std::make_shared<GThreadData>();
}

GThread::~GThread()
{
    quit();
    wait();
}

void GThread::start()
{
    if (m_running.load())
    {
        return;
    }

    // If a previous run finished but the caller never called wait(), m_thread can still hold a
    // joinable std::thread. Overwriting m_thread below would destroy that std::thread while it
    // is still joinable, which calls std::terminate(). Join it first.
    if (m_thread && m_thread->joinable())
    {
        m_thread->join();
    }

    m_running.store(true);
    m_finished.store(false);
    m_exiting.store(false);

    m_thread = std::make_unique<std::thread>(
        [this]()
        {
            s_currentThread = this;
            this->moveToThread(this);

            if (!m_data->dispatcher.load())
            {
#if defined(_WIN32)
                m_data->dispatcher.store(new GEventDispatcherWin32());
#elif defined(__linux__)
                m_data->dispatcher.store(new GEventDispatcherLinux());
#else
                m_data->dispatcher.store(new GEventDispatcherDefault());
#endif
                m_data->ownsDispatcher = true;
            }

            started.emit();

            this->run();

            finished.emit();

            if (m_data->ownsDispatcher)
            {
                GAbstractEventDispatcher* disp = m_data->dispatcher.exchange(nullptr);
                delete disp;
                m_data->ownsDispatcher = false;
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

void GThread::quit()
{
    exit(0);
}

void GThread::exit(int returnCode)
{
    m_exitCode.store(returnCode);
    m_exiting.store(true);
    GAbstractEventDispatcher* dispatcher = m_data->dispatcher.load();
    if (dispatcher)
    {
        dispatcher->interrupt();
        dispatcher->wakeUp();
    }
}

bool GThread::wait(unsigned long time)
{
    if (m_finished.load())
    {
        if (m_thread && m_thread->joinable())
        {
            m_thread->join();
        }
        return true;
    }

    if (!m_thread || !m_thread->joinable())
    {
        return true;
    }

    if (time == ULONG_MAX)
    {
        m_thread->join();
        return true;
    }
    else
    {
        std::unique_lock<std::mutex> lock(m_waitMutex);
        bool completed = m_waitCv.wait_for(
            lock, std::chrono::milliseconds(time), [this] { return m_finished.load(); });
        if (completed && m_thread->joinable())
        {
            m_thread->join();
        }
        return completed;
    }
}

bool GThread::isRunning() const
{
    return m_running.load();
}

bool GThread::isFinished() const
{
    return m_finished.load();
}

GThread* GThread::currentThread()
{
    return s_currentThread;
}

GAbstractEventDispatcher* GThread::eventDispatcher() const
{
    return m_data->dispatcher.load();
}

void GThread::run()
{
    exec();
}

int GThread::exec()
{
    GAbstractEventDispatcher* dispatcher = m_data->dispatcher.load();
    while (!m_exiting.load() && dispatcher)
    {
        dispatcher->processEvents();
        dispatcher = m_data->dispatcher.load();
    }
    return m_exitCode.load();
}
