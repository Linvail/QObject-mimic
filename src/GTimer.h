#ifndef GTIMER_H
#define GTIMER_H

#include "GObject.h"
#include "GSignal.h"

/**
 * @brief High-level timer class providing repetitive and single-shot timers.
 *
 * **No part of this class is thread-safe**, exactly as with Qt's QTimer. A GTimer must be used
 * from the thread it lives in: start()/stop() are thread-confined because they go through
 * GObject::startTimer()/killTimer(), and timeout is emitted by that thread's event loop. To drive
 * a timer that lives in another thread, hop onto it first, e.g.
 * `GObject::callLater(&timer, &GTimer::start, 50)`.
 */
class GTimer : public GObject
{
public:
    /**
     * @brief Constructs a timer.
     */
    GTimer();

    /**
     * @brief Destroys the timer.
     */
    virtual ~GTimer() override;

    /**
     * @brief Gets the timer interval in milliseconds.
     * @return Interval in milliseconds.
     */
    int interval() const;

    /**
     * @brief Sets the timer interval in milliseconds.
     * @param msec Interval in milliseconds.
     */
    void setInterval
        (
        int msec
        );

    /**
     * @brief Checks if the timer is currently active (running).
     * @return True if active.
     */
    bool isActive() const;

    /**
     * @brief Checks if the timer is single-shot.
     * @return True if single-shot.
     */
    bool isSingleShot() const;

    /**
     * @brief Sets whether the timer is single-shot.
     * @param singleShot True for single-shot, false for periodic.
     */
    void setSingleShot
        (
        bool singleShot
        );

    /**
     * @brief Gets the unique ID of the internal timer.
     * @return Unique timer ID, or -1 if inactive.
     */
    int timerId() const;

    /**
     * @brief Starts or restarts the timer with specified interval in milliseconds.
     *
     * **Must be called from this timer's own thread**, because it goes through
     * GObject::startTimer(); see that function for why. Same rule as Qt's QTimer, whose start()
     * is likewise a plain forward to QObject::startTimer(). To start a timer that lives in
     * another thread, hop onto that thread first, e.g.
     * `GObject::callLater(&timer, &GTimer::start, 50)`.
     * @param msec Interval in milliseconds.
     */
    void start
        (
        int msec
        );

    /**
     * @brief Starts or restarts the timer using the existing interval.
     *
     * **Must be called from this timer's own thread**; see start(int).
     */
    void start();

    /**
     * @brief Stops the timer.
     *
     * **Must be called from this timer's own thread**, because it goes through
     * GObject::killTimer(). Calling it from elsewhere warns and leaves the timer running.
     */
    void stop();

    /**
     * @brief Signal emitted when the timer expires.
     */
    GSignal<> timeout;
    /**
     * @brief Fires a single-shot timer executing a functor after specified delay.
     * @tparam Functor Callable slot type.
     * @param msec Delay in milliseconds.
     * @param functor Slot function to execute.
     */
    template <typename Functor> static void singleShot
        (
        int msec,
        Functor functor
        );

    /**
     * @brief Fires a single-shot timer executing a functor in context object's thread.
     * @tparam Functor Callable slot type.
     * @param msec Delay in milliseconds.
     * @param context Target context GObject.
     * @param functor Slot function to execute.
     */
    template <typename Functor>
    static void singleShot
        (
        int msec,
        const GObject* context,
        Functor functor
        );

    /**
     * @brief Fires a single-shot timer executing a member function on receiver object.
     * @tparam Receiver Receiver object type.
     * @tparam MemberFunc Member function pointer type.
     * @param msec Delay in milliseconds.
     * @param receiver Target receiver object.
     * @param method Member function pointer to execute.
     */
    template <typename Receiver, typename MemberFunc>
    static void singleShot
        (
        int msec,
        const Receiver* receiver,
        MemberFunc method
        );

protected:
    /**
     * @brief Internal timer event handler.
     * @param event Timer event.
     */
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
    int m_interval { 0 };
    int m_timerId { -1 };
    bool m_singleShot { false };
    bool m_active { false };
};

template <typename Functor> void GTimer::singleShot
    (
    int msec,
    Functor functor
    )
{
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

template <typename Functor>
void GTimer::singleShot
    (
    int msec,
    const GObject* context,
    Functor functor
    )
{
    if( !context )
    {
        return;
    }
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

        /**
         * @brief Registers the timer. Must run on this helper's own thread.
         *
         * Public only so callLater() can target it; it is not part of any API.
         */
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

template <typename Receiver, typename MemberFunc>
void GTimer::singleShot
    (
    int msec,
    const Receiver* receiver,
    MemberFunc method
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

#endif // GTIMER_H
