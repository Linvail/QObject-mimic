#include "GCoreApplication.h"

#include "GAbstractEventDispatcher.h"
#include "GEventDispatcherDefault.h"
#if defined( _WIN32 )
    #include "GEventDispatcherWin32.h"
#elif defined( __linux__ )
    #include "GEventDispatcherLinux.h"
#endif
#include "GEvent.h"

namespace QtLikeSignal
{
    GCoreApplication* GCoreApplication::sInstance = nullptr;

    //! Constructs the application object.
    GCoreApplication::GCoreApplication
        (
        int& aArgc,     //!< Argument count.
        char** aArgv    //!< Argument vector.
        )
        : GObject()
    {
        ( void )aArgc;
        ( void )aArgv;
        sInstance = this;

        #if defined( _WIN32 )
            mDispatcher = std::make_shared<GEventDispatcherWin32>();
        #elif defined( __linux__ )
            mDispatcher = std::make_shared<GEventDispatcherLinux>();
        #else
            mDispatcher = std::make_shared<GEventDispatcherDefault>();
        #endif
        mMainThread = std::make_unique<GThread>();

        mMainThread->mData->setDispatcher( mDispatcher );
        GThread::sCurrentThread = mMainThread.get();

        // Self-adopt, mirroring exactly what GThread::start()'s lambda does for a worker thread
        // (sCurrentThread = this; this->moveToThread(this);). Without this, mMainThread's own
        // GObject-inherited thread affinity (mThreadData, set via moveToThread) never gets pointed
        // at its own dispatcher-holding GThreadData (mData) -- those are two separate fields that
        // only coincide once an object has been moved onto itself. Anything that targets the
        // GThread object directly (dispatchMetaCall(mainThreadPtr, ...), e.g. via GThread::post())
        // would silently fail to find a dispatcher without this, even though one exists.
        mMainThread->moveToThread( mMainThread.get() );

        this->moveToThread( mMainThread.get() );
    }

    //! Destroys the application object.
    GCoreApplication::~GCoreApplication()
    {
        this->moveToThread( nullptr );
        mMainThread->mData->setDispatcher( nullptr );
        GThread::sCurrentThread = nullptr;
        sInstance = nullptr;
    }

    //! Returns the global application instance. Thread-safe.
    GCoreApplication* GCoreApplication::instance()
    {
        return sInstance;
    }

    //! Enters the main event loop and waits until quit() is called. Returns the exit code.
    int GCoreApplication::exec()
    {
        mExiting.store( false );
        if( mDispatcher )
        {
            while( !mExiting.load() )
            {
                mDispatcher->processEvents();
            }
        }
        return 0;
    }

    //! Tells the application to exit with return code 0. Thread-safe.
    void GCoreApplication::quit()
    {
        mExiting.store( true );
        if( mDispatcher )
        {
            mDispatcher->interrupt();
            mDispatcher->wakeUp();
        }
    }
}
