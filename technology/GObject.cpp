#include "GObject.h"
#include "GThread.h"
#include "GEvent.h"
#include "GAbstractEventDispatcher.h"
#include <algorithm>
#include <unordered_map>

struct CallLaterNode
{
    std::mutex            mutex;
    std::function<void()> invoker;
};

static std::mutex s_callLaterMutex;
static std::unordered_map<GObject::GCallLaterKey,
                          std::shared_ptr<CallLaterNode>,
                          GObject::GCallLaterKeyHash>
    s_pendingCallLaters;

std::atomic<int> GObject::s_nextTimerId{ 1 };

GObject::GObject(GObject* parent)
: m_life(std::make_shared<int>(0))
, m_parent(parent)
{
    GThread* current = GThread::currentThread();
    m_thread.store(current);
    if (current)
    {
        std::shared_ptr<GThreadData> data = current->threadData();
        std::lock_guard<std::mutex>  lock(m_threadDataMutex);
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

    {
        std::lock_guard<std::mutex> lock(m_cleanupMutex);
        for (auto& cb : m_cleanupCallbacks)
        {
            cb();
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_callLaterMutex);
        for (auto it = s_pendingCallLaters.begin(); it != s_pendingCallLaters.end();)
        {
            if (it->first.context == this)
            {
                it = s_pendingCallLaters.erase(it);
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

void GObject::scheduleCallLater(GObject*              context,
                                const GCallLaterKey&  key,
                                std::function<void()> invoker)
{
    if (!context)
    {
        return;
    }

    std::shared_ptr<CallLaterNode> node;
    bool                           isNew = false;

    {
        std::lock_guard<std::mutex> lock(s_callLaterMutex);
        auto                        it = s_pendingCallLaters.find(key);
        if (it != s_pendingCallLaters.end())
        {
            node = it->second;
        }
        else
        {
            node                     = std::make_shared<CallLaterNode>();
            s_pendingCallLaters[key] = node;
            isNew                    = true;
        }
    }

    {
        std::lock_guard<std::mutex> nodeLock(node->mutex);
        node->invoker = std::move(invoker);
    }

    if (isNew)
    {
        std::weak_ptr<int> weakLife = context->objectLife();
        auto               metaCall = [key, node, weakLife]()
        {
            std::function<void()> fnToRun;
            {
                std::lock_guard<std::mutex> lock(s_callLaterMutex);
                s_pendingCallLaters.erase(key);
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


GThread* GObject::thread() const { return m_thread.load(); }

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

void GObject::installEventFilter(GObject* filterObj)
{
    if (!filterObj)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_eventFilterMutex);
    if (std::find(m_eventFilters.begin(), m_eventFilters.end(), filterObj) == m_eventFilters.end())
    {
        m_eventFilters.push_back(filterObj);
    }
}

void GObject::removeEventFilter(GObject* filterObj)
{
    if (!filterObj)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_eventFilterMutex);
    m_eventFilters.erase(std::remove(m_eventFilters.begin(), m_eventFilters.end(), filterObj),
                         m_eventFilters.end());
}

bool GObject::eventFilter(GObject* watched, GEvent* event)
{
    (void) watched;
    (void) event;
    return false;
}

bool GObject::event(GEvent* event)
{
    if (!event)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_eventFilterMutex);
        for (GObject* filter : m_eventFilters)
        {
            if (filter && filter->eventFilter(this, event))
            {
                return true;
            }
        }
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
            customEvent(event);
            return true;

        default:
            customEvent(event);
            return true;
    }
}

void GObject::customEvent(GEvent* event)
{
    if (event->type() == GEvent::MetaCall)
    {
        auto* metaEvent = static_cast<GMetaCallEvent*>(event);
        metaEvent->placeMetaCall();
    }
}

void GObject::timerEvent(GTimerEvent* event) { (void) event; }

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

    GThread*          targetThread = target->thread();
    G::ConnectionType activeType   = type;
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
