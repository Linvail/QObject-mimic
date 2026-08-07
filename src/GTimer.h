#ifndef GTIMER_H
#define GTIMER_H

#include "GObject.h"
#include "GSignal.h"

namespace QtLikeSignal
{
    //! High-level timer class providing repetitive and single-shot timers.
    //!
    //! **No part of this class is thread-safe**, exactly as with Qt's QTimer. A GTimer must be used
    //! from the thread it lives in: start()/stop() are thread-confined because they go through
    //! GObject::startTimer()/killTimer(), and timeout is emitted by that thread's event loop. To drive
    //! a timer that lives in another thread, hop onto it first, e.g.
    //! `GObject::callLater(&timer, &GTimer::start, 50)`.
    class GTimer : public GObject
    {
    public:
        GTimer();

        virtual ~GTimer() override;

        int interval() const;

        void setInterval
            (
            int msec
            );

        bool isActive() const;

        bool isSingleShot() const;

        void setSingleShot
            (
            bool singleShot
            );

        int timerId() const;

        //! Starts or restarts the timer with specified interval in milliseconds.
        //!
        //! **Must be called from this timer's own thread**, because it goes through
        //! GObject::startTimer(); see that function for why. Same rule as Qt's QTimer, whose start()
        //! is likewise a plain forward to QObject::startTimer(). To start a timer that lives in
        //! another thread, hop onto that thread first, e.g.
        //! `GObject::callLater(&timer, &GTimer::start, 50)`.
        void start
            (
            int msec  //!< Interval in milliseconds.
            );

        void start();

        void stop();

        //! Signal emitted when the timer expires.
        GSignal<> timeout;
        //! Fires a single-shot timer executing a functor after specified delay. Functor is the
        //! callable slot type.
        template <typename Functor> static void singleShot
            (
            int msec,
            Functor functor
            );

        //! Fires a single-shot timer executing a functor in context object's thread. Functor is
        //! the callable slot type.
        template <typename Functor>
        static void singleShot
            (
            int msec,
            const GObject* context,
            Functor functor
            );

        //! Fires a single-shot timer executing a member function on receiver object. Receiver is
        //! the receiver object type and MemberFunc the member function pointer type.
        template <typename Receiver, typename MemberFunc>
        static void singleShot
            (
            int msec,
            const Receiver* receiver,
            MemberFunc method
            );

    protected:
        virtual void timerEvent
            (
            GTimerEvent* event
            ) override;

    private:
        // Deliberately unsynchronised, matching QTimer, which has no locking of any kind. Every
        // member here is only ever touched from the timer's own thread: start()/stop() are
        // thread-confined because they go through GObject::startTimer()/killTimer(), and timerEvent()
        // is delivered by that same thread's event loop. Adding a mutex would only paper over misuse
        // that the thread-confinement rules already forbid.
        int m_interval { 0 };        //!< The configured interval, in milliseconds.
        int m_timerId { -1 };        //!< The underlying GObject timer id, or -1 if inactive.
        bool m_singleShot { false }; //!< True if the timer stops itself after firing once.
        bool m_active { false };     //!< True while the timer is running.
    };

    //! Fires a single-shot timer executing a functor after specified delay.
    template <typename Functor> void GTimer::singleShot
        (
        int msec,        //!< Delay in milliseconds.
        Functor functor  //!< Slot function to execute.
        )
    {
        //! Self-deleting helper that fires functor once when its timer expires.
        class GSingleShotHelper : public GObject
        {
        public:
            GSingleShotHelper
                (
                int ms,
                Functor fn
                )
                : m_fn( std::move( fn ) )
            {
                m_id = startTimer( ms );
            }

        protected:
            virtual void timerEvent
                (
                GTimerEvent* event
                ) override
            {
                if( event->timerId() == m_id )
                {
                    m_fn();
                    deleteLater();
                }
            }

        public:
            int timerId() const
            {
                return m_id;
            }

        private:
            Functor m_fn;
            int m_id { -1 };
        };
        auto* helper = new GSingleShotHelper( msec, functor );
        if( helper->timerId() == -1 )
        {
            delete helper;
        }
    }

    //! Fires a single-shot timer executing a functor in context object's thread.
    template <typename Functor>
    void GTimer::singleShot
        (
        int msec,                  //!< Delay in milliseconds.
        const GObject* context,    //!< Target context GObject.
        Functor functor            //!< Slot function to execute.
        )
    {
        if( !context )
        {
            return;
        }
        //! Self-deleting helper that arms itself on context's thread and fires functor once.
        class GSingleShotContextHelper : public GObject
        {
        public:
            GSingleShotContextHelper
                (
                int ms,
                Functor fn
                )
                : m_fn( std::move( fn ) )
                , m_interval( ms )
            {
            }

            //! Registers the timer. Must run on this helper's own thread.
            //!
            //! Public only so callLater() can target it; it is not part of any API.
            void arm()
            {
                m_id = startTimer( m_interval );
                if( m_id == -1 )
                {
                    // Nothing will ever fire, so reclaim the helper rather than leaking it.
                    delete this;
                }
            }

        protected:
            virtual void timerEvent
                (
                GTimerEvent* event
                ) override
            {
                if( event->timerId() == m_id )
                {
                    m_fn();
                    deleteLater();
                }
            }

        private:
            Functor m_fn;
            int m_interval { 0 };
            int m_id { -1 };
        };

        auto*    helper = new GSingleShotContextHelper( msec, functor );
        GObject* ctx    = const_cast<GObject*>( context );

        // startTimer() is thread-confined, so the timer has to be registered on the thread that will
        // deliver its events -- not on whichever thread happens to call singleShot(). This mirrors
        // Qt's QSingleShotTimer::startTimerForReceiver(), which arms directly when the receiver is on
        // the current thread and otherwise moves itself to the receiver's thread and posts an event
        // to start the timer there.
        //
        // The helper was constructed here, so its thread() is this thread.
        if( helper->thread() == ctx->thread() )
        {
            helper->arm(); // may delete itself; do not touch `helper` afterwards
        }
        else
        {
            helper->moveToThread( ctx->thread() );
            GObject::callLater( helper, &GSingleShotContextHelper::arm );
        }
    }

    //! Fires a single-shot timer executing a member function on receiver object.
    template <typename Receiver, typename MemberFunc>
    void GTimer::singleShot
        (
        int msec,                    //!< Delay in milliseconds.
        const Receiver* receiver,    //!< Target receiver object.
        MemberFunc method            //!< Member function pointer to execute.
        )
    {
        if( !receiver )
        {
            return;
        }
        auto bound = [receiver, method]()
            {
                ( const_cast<Receiver*>( receiver )->*method )();
            };
        singleShot( msec, static_cast<const GObject*>( receiver ), bound );
    }
}

#endif // GTIMER_H
