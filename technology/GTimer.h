#pragma once

#include "GObject.h"
#include "GSignal.h"

/**
 * @brief High-level timer class providing repetitive and single-shot timers.
 */
class GTimer : public GObject
{
public:
    /**
     * @brief Constructs a timer with optional parent.
     * @param parent Parent object.
     */
    GTimer(GObject* parent = nullptr);

    /**
     * @brief Destroys the timer.
     */
    virtual ~GTimer() override;

    /**
     * @brief Gets the timer interval in milliseconds.
     * @return Interval in milliseconds. Thread-safe.
     */
    int interval() const;

    /**
     * @brief Sets the timer interval in milliseconds.
     * @param msec Interval in milliseconds. Thread-safe.
     */
    void setInterval(int msec);

    /**
     * @brief Checks if the timer is currently active (running).
     * @return True if active. Thread-safe.
     */
    bool isActive() const;

    /**
     * @brief Checks if the timer is single-shot.
     * @return True if single-shot. Thread-safe.
     */
    bool isSingleShot() const;

    /**
     * @brief Sets whether the timer is single-shot.
     * @param singleShot True for single-shot, false for periodic. Thread-safe.
     */
    void setSingleShot(bool singleShot);

    /**
     * @brief Gets the unique ID of the internal timer.
     * @return Unique timer ID, or -1 if inactive. Thread-safe.
     */
    int timerId() const;

    /**
     * @brief Starts or restarts the timer with specified interval in milliseconds.
     * @param msec Interval in milliseconds. Thread-safe.
     */
    void start(int msec);

    /**
     * @brief Starts or restarts the timer using the existing interval. Thread-safe.
     */
    void start();

    /**
     * @brief Stops the timer. Thread-safe.
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
     * @param functor Slot function to execute. Thread-safe.
     */
    template<typename Functor>
    static void singleShot(int msec, Functor functor);

    /**
     * @brief Fires a single-shot timer executing a functor in context object's thread.
     * @tparam Functor Callable slot type.
     * @param msec Delay in milliseconds.
     * @param context Target context GObject.
     * @param functor Slot function to execute. Thread-safe.
     */
    template<typename Functor>
    static void singleShot(int msec, const GObject* context, Functor functor);

    /**
     * @brief Fires a single-shot timer executing a member function on receiver object.
     * @tparam Receiver Receiver object type.
     * @tparam MemberFunc Member function pointer type.
     * @param msec Delay in milliseconds.
     * @param receiver Target receiver object.
     * @param method Member function pointer to execute. Thread-safe.
     */
    template<typename Receiver, typename MemberFunc>
    static void singleShot(int msec, const Receiver* receiver, MemberFunc method);

protected:
    /**
     * @brief Internal timer event handler.
     * @param event Timer event.
     */
    virtual void timerEvent(GTimerEvent* event) override;

private:
    int  m_interval{ 0 };
    int  m_timerId{ -1 };
    bool m_singleShot{ false };
    bool m_active{ false };
};

template<typename Functor>
void GTimer::singleShot(int msec, Functor functor)
{
    class GSingleShotHelper : public GObject
    {
    public:
        GSingleShotHelper(int ms, Functor fn)
        : m_fn(std::move(fn))
        {
            m_id = startTimer(ms);
        }

    protected:
        virtual void timerEvent(GTimerEvent* event) override
        {
            if (event->timerId() == m_id)
            {
                m_fn();
                deleteLater();
            }
        }

    public:
        int timerId() const { return m_id; }

    private:
        Functor m_fn;
        int     m_id{ -1 };
    };
    auto* helper = new GSingleShotHelper(msec, functor);
    if (helper->timerId() == -1)
    {
        delete helper;
    }
}

template<typename Functor>
void GTimer::singleShot(int msec, const GObject* context, Functor functor)
{
    if (!context)
    {
        return;
    }
    class GSingleShotContextHelper : public GObject
    {
    public:
        GSingleShotContextHelper(GObject* ctx, int ms, Functor fn)
        : m_fn(std::move(fn))
        {
            if (ctx)
            {
                this->moveToThread(ctx->thread());
            }
            m_id = startTimer(ms);
        }

    protected:
        virtual void timerEvent(GTimerEvent* event) override
        {
            if (event->timerId() == m_id)
            {
                m_fn();
                deleteLater();
            }
        }

    public:
        int timerId() const { return m_id; }

    private:
        Functor m_fn;
        int     m_id{ -1 };
    };
    auto* helper = new GSingleShotContextHelper(const_cast<GObject*>(context), msec, functor);
    if (helper->timerId() == -1)
    {
        delete helper;
    }
}

template<typename Receiver, typename MemberFunc>
void GTimer::singleShot(int msec, const Receiver* receiver, MemberFunc method)
{
    if (!receiver)
    {
        return;
    }
    auto bound = [receiver, method]() { (const_cast<Receiver*>(receiver)->*method)(); };
    singleShot(msec, static_cast<const GObject*>(receiver), bound);
}
