#ifndef THREAD_H
#define THREAD_H

#include "Object.h"
#include "Signal.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace QtLikeSignal
{
    class AbstractEventDispatcher;

    //! Manages a platform execution thread with an event loop.
    class Thread : public Object
    {
    public:
        Thread();

        virtual ~Thread() override;

        //! Scheduling priority of a thread, mirroring QThread::Priority.
        //!
        //! The numeric order is load-bearing, not cosmetic: the UNIX backend scales these values
        //! arithmetically onto whatever range the platform scheduler reports, so IdlePriority must
        //! stay lowest and TimeCriticalPriority highest, with no gaps introduced between them.
        //!
        //! InheritPriority means "whatever the thread that called start() was running at". It is the
        //! state a thread begins in and is reported for a thread that is not running. start() accepts
        //! it -- it is the default -- but setPriority() does not, because once a thread is running
        //! there is no operation that corresponds to re-inheriting.

        enum Priority
        {
            IdlePriority,

            LowestPriority,
            LowPriority,
            NormalPriority,
            HighPriority,
            HighestPriority,

            TimeCriticalPriority,

            InheritPriority
        };
        void start
            (
            Priority aPriority = InheritPriority
            );

        void quit();

        void exit
            (
            int aReturnCode = 0
            );

        bool wait
            (
            unsigned long aTime = ULONG_MAX
            );

        bool isRunning() const;

        bool isFinished() const;

        void setPriority
            (
            Priority aPriority
            );

        Priority priority() const;

        static Thread* currentThread();

        std::shared_ptr<AbstractEventDispatcher> eventDispatcher() const;

        bool post
            (
            std::function<void()> aTask
            );

        //! Signal emitted when the thread starts running.
        Signal<> started;

        //! Signal emitted when the thread finishes execution.
        Signal<> finished;

        //! Creates and starts a Thread executing the specified function. Function is the callable
        //! type and Args its argument types. Thread-safe.
        template <typename Function, typename ... Args>
        static Thread* create( Function&& aF, Args&&... aArgs );

    protected:
        virtual void run();

        int exec();

    private:
        //! Gets the thread's internal data container holding the event dispatcher.
        //!
        //! Private for the same reason as Object::threadData(): it is the handle onto the dispatcher
        //! plumbing, not API. Object reaches it when adopting a thread's affinity.
        std::shared_ptr<ThreadData> threadData() const
        {
            return mData;
        }

        void applyPriority
            (
            Priority aPriority
            );

        std::unique_ptr<std::thread> mThread;      //!< The underlying OS thread, once started.
        std::shared_ptr<ThreadData> mData;        //!< This thread's dispatcher-holding data.
        std::atomic<bool> mRunning { false };      //!< True while the OS thread is executing.
        std::atomic<bool> mFinished { false };     //!< True once the OS thread has finished.
        std::atomic<bool> mExiting { false };      //!< Set by exit()/quit() to stop exec()'s loop.
        std::atomic<int> mExitCode { 0 };          //!< Return code passed to exit(), reported by exec().
        mutable std::mutex mWaitMutex;             //!< Guards mWaitCv's predicate.
        std::condition_variable mWaitCv;           //!< Notified when the thread finishes, for wait().

        //! Guards mPriority and every use of mThread's native handle.
        //!
        //! Not merely protecting the enum. The run body clears mRunning while holding this mutex, so
        //! a setPriority() that has observed mRunning == true under the same lock is guaranteed the
        //! OS thread has not yet reached the end of its body -- without that, the handle could be
        //! touched after the thread had exited.
        mutable std::mutex mPriorityMutex;
        Priority mPriority { InheritPriority };  //!< Priority applied to the current/most recent run.

        static thread_local Thread* sCurrentThread;  //!< The Thread running on this OS thread, if any.
        friend class CoreApplication;
        //! Grants Object access to threadData() when adopting or releasing thread affinity.
        friend class Object;
    };

    //! Creates and starts a Thread executing the specified function.
    //!
    //! The wrapping FuncThread subclass exists purely so create() can hand back a plain Thread*
    //! without requiring callers to declare their own subclass just to run a callable.
    template <typename Function, typename ... Args>
    Thread* Thread::create
        (
        Function&& aF,      //!< Function to execute.
        Args&&... aArgs      //!< Arguments to pass.
        )
    {
        auto task = std::bind( std::forward<Function>( aF ), std::forward<Args>( aArgs )... );

        //! Adapts an arbitrary bound callable into a Thread by running it from run().
        class FuncThread : public Thread
        {
        public:
            FuncThread
                (
                std::function<void()> aFn
                )
                : mFn( std::move( aFn ) )
            {
            }

        protected:
            virtual void run() override
            {
                if( mFn )
                {
                    mFn();
                }
            }

        private:
            std::function<void()> mFn;
        };

        auto* threadObj = new FuncThread( task );
        threadObj->start();
        return threadObj;
    }
}

#endif // THREAD_H
