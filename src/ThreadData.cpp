#include "ThreadData.hpp"

namespace QtLikeSignal
{
    //! Gets a strong reference to this thread's dispatcher, or nullptr if none is installed.
    //! Thread-safe.
    std::shared_ptr<AbstractEventDispatcher> ThreadData::dispatcher() const
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        return mDispatcher;
    }

    //! Installs or clears this thread's dispatcher. Thread-safe.
    void ThreadData::setDispatcher
        (
        std::shared_ptr<AbstractEventDispatcher> aDispatcher    //!< Dispatcher to install; nullptr clears it.
        )
    {
        std::lock_guard<std::mutex> lock( mDispatcherMutex );
        mDispatcher = std::move( aDispatcher );
    }
}
