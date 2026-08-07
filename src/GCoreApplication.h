#ifndef GCOREAPPLICATION_H
#define GCOREAPPLICATION_H

#include "GObject.h"
#include "GThread.h"
#include <memory>
#include <atomic>

namespace QtLikeSignal
{
    class GAbstractEventDispatcher;
    class GEvent;

    //! Singleton class that manages the application's control flow and main event loop.
    class GCoreApplication : public GObject
    {
    public:
        GCoreApplication
            (
            int& argc,
            char** argv
            );

        virtual ~GCoreApplication();

        static GCoreApplication* instance();

        int exec();

        void quit();

    private:
        static GCoreApplication* s_instance;  //!< The process-wide application instance.

        std::unique_ptr<GThread>                  m_mainThread;  //!< The application's main thread.
        // shared_ptr rather than unique_ptr: GThreadData hands out strong references, so a dispatcher
        // cannot be destroyed while another thread is part-way through a call into it.
        std::shared_ptr<GAbstractEventDispatcher> m_dispatcher;  //!< The main thread's event dispatcher.
        std::atomic<bool>                         m_exiting { false };  //!< Set by quit() to stop exec()'s loop.
    };
}

#endif // GCOREAPPLICATION_H
