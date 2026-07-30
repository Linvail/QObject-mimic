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
    m_thread.store(GThread::currentThread());
}

GObject::~GObject()
{
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

    GThread* t = m_thread.load();
    if (t)
    {
        GAbstractEventDispatcher* dispatcher = t->eventDispatcher();
        if (dispatcher)
        {
            dispatcher->removeEventsForReceiver(this);
        }
    }

    m_life.reset();
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

void GObject::moveToThread(GThread* thread) { m_thread.store(thread); }

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
    GThread* targetThread = m_thread.load();
    auto*    event        = new GDeferredDeleteEvent();
    if (targetThread && targetThread->eventDispatcher())
    {
        targetThread->eventDispatcher()->postEvent(this, static_cast<GEvent*>(event));
    }
    else
    {
        delete this;
    }
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
    int      timerId = s_nextTimerId.fetch_add(1);
    GThread* t       = m_thread.load();
    if (t && t->eventDispatcher())
    {
        t->eventDispatcher()->registerTimer(timerId, interval, this);
        return timerId;
    }
    return -1;
}

void GObject::killTimer(int id)
{
    GThread* t = m_thread.load();
    if (t && t->eventDispatcher())
    {
        t->eventDispatcher()->unregisterTimer(id);
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
        if (targetThread && targetThread->eventDispatcher())
        {
            targetThread->eventDispatcher()->postEvent(target, static_cast<GEvent*>(event));
        }
        else
        {
            delete event;
        }
    }
    else
    {
        slot();
    }
}
