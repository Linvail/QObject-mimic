#include "GCoreApplication.h"

#include "GAbstractEventDispatcher.h"
#include "GEventDispatcherDefault.h"
#if defined( _WIN32 )
    #include "GEventDispatcherWin32.h"
#elif defined( __linux__ )
    #include "GEventDispatcherLinux.h"
#endif
#include "GEvent.h"

GCoreApplication* GCoreApplication::s_instance = nullptr;

GCoreApplication::GCoreApplication
    (
    int& argc,
    char** argv
    )
    : GObject()
{
    ( void )argc;
    ( void )argv;
    s_instance = this;

    #if defined( _WIN32 )
        m_dispatcher = std::make_shared<GEventDispatcherWin32>();
    #elif defined( __linux__ )
        m_dispatcher = std::make_shared<GEventDispatcherLinux>();
    #else
        m_dispatcher = std::make_shared<GEventDispatcherDefault>();
    #endif
    m_mainThread = std::make_unique<GThread>();

    m_mainThread->m_data->setDispatcher( m_dispatcher );
    GThread::s_currentThread = m_mainThread.get();

    // Self-adopt, mirroring exactly what GThread::start()'s lambda does for a worker thread
    // (s_currentThread = this; this->moveToThread(this);). Without this, m_mainThread's own
    // GObject-inherited thread affinity (m_threadData, set via moveToThread) never gets pointed
    // at its own dispatcher-holding GThreadData (m_data) -- those are two separate fields that
    // only coincide once an object has been moved onto itself. Anything that targets the
    // GThread object directly (dispatchMetaCall(mainThreadPtr, ...), e.g. via GThread::post())
    // would silently fail to find a dispatcher without this, even though one exists.
    m_mainThread->moveToThread( m_mainThread.get() );

    this->moveToThread( m_mainThread.get() );
}

GCoreApplication::~GCoreApplication()
{
    this->moveToThread( nullptr );
    m_mainThread->m_data->setDispatcher( nullptr );
    GThread::s_currentThread = nullptr;
    s_instance = nullptr;
}

GCoreApplication* GCoreApplication::instance()
{
    return s_instance;
}

int GCoreApplication::exec()
{
    m_exiting.store( false );
    if( m_dispatcher )
    {
        while( !m_exiting.load() )
        {
            m_dispatcher->processEvents();
        }
    }
    return 0;
}

void GCoreApplication::quit()
{
    m_exiting.store( true );
    if( m_dispatcher )
    {
        m_dispatcher->interrupt();
        m_dispatcher->wakeUp();
    }
}
