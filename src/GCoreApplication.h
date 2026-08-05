#ifndef GCOREAPPLICATION_H
#define GCOREAPPLICATION_H

#include "GObject.h"
#include "GThread.h"
#include <memory>
#include <atomic>

class GAbstractEventDispatcher;
class GEvent;

/**
 * @brief Singleton class that manages the application's control flow and main event loop.
 */
class GCoreApplication : public GObject
{
public:
    /**
     * @brief Constructs the application object.
     * @param argc Argument count.
     * @param argv Argument vector.
     */
    GCoreApplication
        (
        int& argc,
        char** argv
        );

    /**
     * @brief Destroys the application object.
     */
    virtual ~GCoreApplication();

    /**
     * @brief Returns the global application instance.
     * @return A pointer to the GCoreApplication instance. Thread-safe.
     */
    static GCoreApplication* instance();

    /**
     * @brief Enters the main event loop and waits until quit() is called.
     * @return The exit code.
     */
    int exec();

    /**
     * @brief Tells the application to exit with return code 0. Thread-safe.
     */
    void quit();

private:
    static GCoreApplication* s_instance;

    std::unique_ptr<GThread>                  m_mainThread;
    // shared_ptr rather than unique_ptr: GThreadData hands out strong references, so a dispatcher
    // cannot be destroyed while another thread is part-way through a call into it.
    std::shared_ptr<GAbstractEventDispatcher> m_dispatcher;
    std::atomic<bool>                         m_exiting { false };
};

#endif // GCOREAPPLICATION_H
