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

            bool createdDispatcher = false;
            if (!m_data->dispatcher())
            {
#if defined(_WIN32)
                m_data->setDispatcher(std::make_shared<GEventDispatcherWin32>());
#elif defined(__linux__)
                m_data->setDispatcher(std::make_shared<GEventDispatcherLinux>());
#else
                m_data->setDispatcher(std::make_shared<GEventDispatcherDefault>());
#endif
                createdDispatcher = true;
            }

            started.emit();

            this->run();

            finished.emit();

            // Drain deferred deletes before letting go of the dispatcher, mirroring Qt's
            // QThreadPrivate::finish(), which calls sendPostedEvents(nullptr, DeferredDelete)
            // right after emitting finished() and before cleanup() destroys the dispatcher.
            // Without this, anything that called deleteLater() before the loop stopped is never
            // destroyed: the dispatcher's destructor can free the queued events but has no way
            // to free their receivers.
            if (auto disp = m_data->dispatcher())
            {
                disp->processDeferredDeletes();
            }

            if (createdDispatcher)
            {
                // Just drop this thread's reference. Any other thread that is part-way through a
                // call still holds its own strong reference from GThreadData::dispatcher(), so
                // the dispatcher stays alive until that call finishes rather than being freed
                // underneath it.
                m_data->setDispatcher(nullptr);
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
    auto dispatcher = m_data->dispatcher();
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

std::shared_ptr<GAbstractEventDispatcher> GThread::eventDispatcher() const
{
    return m_data->dispatcher();
}

bool GThread::post(std::function<void()> task)
{
    if (!task)
    {
        return false;
    }
    // Explicit QueuedConnection (not Auto): post() must always defer, even when called from this
    // thread itself -- Auto would resolve to a same-thread call and run inline instead.
    return dispatchMetaCall(this, std::move(task), G::QueuedConnection);
}

void GThread::run()
{
    exec();
}

int GThread::exec()
{
    // Re-fetched each iteration, and held as a strong reference across processEvents() so the
    // dispatcher cannot be destroyed mid-call.
    auto dispatcher = m_data->dispatcher();
    while (!m_exiting.load() && dispatcher)
    {
        dispatcher->processEvents();
        dispatcher = m_data->dispatcher();
    }
    return m_exitCode.load();
}
