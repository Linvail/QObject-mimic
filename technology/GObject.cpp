#include "GObject.h"

#include "GAbstractEventDispatcher.h"
#include "GEvent.h"
#include "GThread.h"

#include <algorithm>
#include <unordered_map>

struct CallLaterNode
{
    std::mutex mutex;
    std::function<void()> invoker;
};

/**
 * @brief Process-wide registry of callLater invocations still waiting to run.
 *
 * Exists as a friend of GObject purely so it can name GObject's private GCallLaterKey /
 * GCallLaterKeyHash types; a plain file-scope map could not. Not declared in any header.
 */
struct GCallLaterRegistry
{
    static std::mutex mutex;
    static std::unordered_map<GObject::GCallLaterKey,
                              std::shared_ptr<CallLaterNode>,
                              GObject::GCallLaterKeyHash>
        pending;
};

std::mutex GCallLaterRegistry::mutex;
std::unordered_map<GObject::GCallLaterKey,
                   std::shared_ptr<CallLaterNode>,
                   GObject::GCallLaterKeyHash>
    GCallLaterRegistry::pending;

std::atomic<int> GObject::s_nextTimerId{1};

GObject::GObject()
    : m_life(std::make_shared<int>(0))
{
    GThread* current = GThread::currentThread();
    m_thread.store(current);
    if (current)
    {
        std::shared_ptr<GThreadData> data = current->threadData();
        std::lock_guard<std::mutex> lock(m_threadDataMutex);
        m_threadData = std::move(data);
    }
}

GObject::~GObject()
{
    // Invalidate the life token first. connect()/callLater() wrappers running on other threads
    // check objectLife().lock() before posting a call to this object; resetting m_life up front
    // shrinks the window in which such a wrapper can still observe this object as "alive" to
    // the check-then-post race itself, instead of the whole destructor body (which below runs
    // arbitrary user cleanup-callback code).
    m_life.reset();

    // Move the callbacks out from under m_cleanupMutex before invoking any of them. Running them
    // while still holding the lock deadlocks on the non-recursive mutex if a callback calls
    // addCleanupCallback() on this same object. A callback registered during the loop below is
    // intentionally dropped -- this object is already being destroyed, so there is no later point
    // at which it could meaningfully run.
    std::vector<std::function<void()>> callbacksToRun;
    {
        std::lock_guard<std::mutex> lock(m_cleanupMutex);
        callbacksToRun.swap(m_cleanupCallbacks);
    }
    for (auto& cb : callbacksToRun)
    {
        cb();
    }

    {
        std::lock_guard<std::mutex> lock(GCallLaterRegistry::mutex);
        auto& pending = GCallLaterRegistry::pending;
        for (auto it = pending.begin(); it != pending.end();)
        {
            if (it->first.context == this)
            {
                it = pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::shared_ptr<GThreadData> threadDataCopy;
    {
        std::lock_guard<std::mutex> lock(m_threadDataMutex);
        threadDataCopy = m_threadData;
    }
    if (threadDataCopy)
    {
        if (GAbstractEventDispatcher* dispatcher = threadDataCopy->dispatcher.load())
        {
            dispatcher->removeEventsForReceiver(this);
        }
    }
}

void GObject::scheduleCallLater(GObject* context,
                                const GCallLaterKey& key,
                                std::function<void()> invoker)
{
    if (!context)
    {
        return;
    }

    std::shared_ptr<CallLaterNode> node;
    bool isNew = false;

    {
        std::lock_guard<std::mutex> lock(GCallLaterRegistry::mutex);
        auto& pending = GCallLaterRegistry::pending;
        auto it = pending.find(key);
        if (it != pending.end())
        {
            node = it->second;
        }
        else
        {
            node = std::make_shared<CallLaterNode>();
            pending[key] = node;
            isNew = true;
        }
    }

    {
        std::lock_guard<std::mutex> nodeLock(node->mutex);
        node->invoker = std::move(invoker);
    }

    if (isNew)
    {
        std::weak_ptr<int> weakLife = context->objectLife();
        auto metaCall = [key, node, weakLife]()
        {
            std::function<void()> fnToRun;
            {
                std::lock_guard<std::mutex> lock(GCallLaterRegistry::mutex);
                GCallLaterRegistry::pending.erase(key);
            }
            {
                std::lock_guard<std::mutex> nodeLock(node->mutex);
                fnToRun = std::move(node->invoker);
            }
            if (fnToRun)
            {
                if (auto life = weakLife.lock())
                {
                    fnToRun();
                }
            }
        };

        dispatchMetaCall(context, metaCall, G::QueuedConnection);
    }
}

GThread* GObject::thread() const
{
    return m_thread.load();
}

std::shared_ptr<GThreadData> GObject::threadData() const
{
    std::lock_guard<std::mutex> lock(m_threadDataMutex);
    return m_threadData;
}

void GObject::moveToThread(GThread* thread)
{
    // Resolve the new thread's data before taking our own lock, and store it under the lock in
    // one atomic-looking step so concurrent readers of threadData() never see a half-updated or
    // torn shared_ptr.
    std::shared_ptr<GThreadData> newData = thread ? thread->threadData() : nullptr;
    m_thread.store(thread);

    std::lock_guard<std::mutex> lock(m_threadDataMutex);
    m_threadData = std::move(newData);
}

std::string GObject::objectName() const
{
    std::lock_guard<std::mutex> lock(m_nameMutex);
    return m_objectName;
}

void GObject::setObjectName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_nameMutex);
    m_objectName = name;
}

void GObject::deleteLater()
{
    auto* event = new GDeferredDeleteEvent();
    if (auto tData = threadData())
    {
        if (auto disp = tData->dispatcher.load())
        {
            disp->postEvent(this, static_cast<GEvent*>(event));
            return;
        }
    }
    delete event;
    delete this;
}

bool GObject::event(GEvent* event)
{
    if (!event)
    {
        return false;
    }

    switch (event->type())
    {
    case GEvent::Timer:
        timerEvent(static_cast<GTimerEvent*>(event));
        return true;

    case GEvent::DeferredDelete:
        delete this;
        return true;

    case GEvent::MetaCall:
        static_cast<GMetaCallEvent*>(event)->placeMetaCall();
        return true;

    default:
        // The only events that reach this queue are the three above, all posted by GObject's
        // own internals. Nothing can inject an arbitrary event for an arbitrary receiver, so
        // any other type is unreachable rather than something to hand to a user hook.
        return false;
    }
}

void GObject::timerEvent(GTimerEvent* event)
{
    (void)event;
}

int GObject::startTimer(int interval)
{
    int timerId = s_nextTimerId.fetch_add(1);
    if (auto tData = threadData())
    {
        if (auto disp = tData->dispatcher.load())
        {
            disp->registerTimer(timerId, interval, this);
            return timerId;
        }
    }
    return -1;
}

void GObject::killTimer(int id)
{
    if (auto tData = threadData())
    {
        if (auto disp = tData->dispatcher.load())
        {
            disp->unregisterTimer(id);
        }
    }
}

void GObject::addCleanupCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_cleanupMutex);
    m_cleanupCallbacks.push_back(std::move(callback));
}

void GObject::dispatchMetaCall(GObject* target, std::function<void()> slot, G::ConnectionType type)
{
    if (!target)
    {
        return;
    }

    GThread* targetThread = target->thread();
    G::ConnectionType activeType = type;
    if (activeType == G::AutoConnection)
    {
        GThread* currentThread = GThread::currentThread();
        if (currentThread == targetThread)
        {
            activeType = G::DirectConnection;
        }
        else
        {
            activeType = G::QueuedConnection;
        }
    }

    if (activeType == G::QueuedConnection)
    {
        auto* event = new GMetaCallEvent(slot);
        if (auto tData = target->threadData())
        {
            if (auto disp = tData->dispatcher.load())
            {
                disp->postEvent(target, static_cast<GEvent*>(event));
                return;
            }
        }
        delete event;
    }
    else
    {
        slot();
    }
}
