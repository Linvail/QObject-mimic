#ifndef COREAPPLICATION_H
#define COREAPPLICATION_H

#include "Object.h"
#include "Thread.h"
#include <memory>
#include <atomic>

namespace QtLikeSignal
{
    class AbstractEventDispatcher;
    class Event;

    //! Singleton class that manages the application's control flow and main event loop.
    class CoreApplication : public Object
    {
    public:
        CoreApplication
            (
            int& aArgc,
            char** aArgv
            );

        virtual ~CoreApplication();

        static CoreApplication* instance();

        int exec();

        void quit();

    private:
        static CoreApplication* sInstance;  //!< The process-wide application instance.

        std::unique_ptr<Thread>                  mMainThread;  //!< The application's main thread.
        // shared_ptr rather than unique_ptr: ThreadData hands out strong references, so a dispatcher
        // cannot be destroyed while another thread is part-way through a call into it.
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;  //!< The main thread's event dispatcher.
        std::atomic<bool>                         mExiting { false };  //!< Set by quit() to stop exec()'s loop.
    };
}

#endif // COREAPPLICATION_H
