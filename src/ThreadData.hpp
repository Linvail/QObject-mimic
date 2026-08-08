#ifndef QT_LIKE_SIGNAL_THREADDATA_HPP
#define QT_LIKE_SIGNAL_THREADDATA_HPP

#include "AbstractEventDispatcher.h"

#include <memory>

namespace QtLikeSignal
{
    //! Per-thread state owning that thread's event dispatcher.
    //!
    //! Handed out by Object::threadData()/Thread::threadData() as an opaque handle. The dispatcher is
    //! held by shared_ptr and only ever reachable through dispatcher(), which hands back a *strong*
    //! reference. That is what makes cross-thread use safe: a thread finishing can drop its dispatcher
    //! at any moment, and an atomic raw pointer would only have made the pointer load safe, not the
    //! object's lifetime -- the owning thread could free it between another thread's load and its call.
    //! Holding a strong reference for the duration of the call keeps it alive until that caller is done.
    //!
    //! Access is private on purpose: a writable dispatcher handle would let outside code redirect a
    //! running loop or drop a dispatcher still in use. Only the three classes that legitimately manage
    //! a thread's lifecycle are granted access.
    struct ThreadData
    {
    private:
        std::shared_ptr<AbstractEventDispatcher> dispatcher() const;

        void setDispatcher
            (
            std::shared_ptr<AbstractEventDispatcher> aDispatcher
            );

        mutable std::mutex mDispatcherMutex;                      //!< Guards mDispatcher.
        std::shared_ptr<AbstractEventDispatcher> mDispatcher;    //!< This thread's dispatcher, if any.

        friend class Object;
        friend class Thread;
        friend class CoreApplication;
    };
}

#endif // QT_LIKE_SIGNAL_THREADDATA_HPP
