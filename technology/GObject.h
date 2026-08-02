#ifndef GOBJECT_H
#define GOBJECT_H

#include "GEvent.h"
#include "GGlobal.h"

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class GThread;
template <typename... Args> class GSignal;
class GAbstractEventDispatcher;
class GEventDispatcherDefault;
class GCoreApplication;

/**
 * @brief Per-thread state owning that thread's event dispatcher.
 *
 * Handed out by GObject::threadData()/GThread::threadData() as an opaque handle. The dispatcher is
 * held by shared_ptr and only ever reachable through dispatcher(), which hands back a *strong*
 * reference. That is what makes cross-thread use safe: a thread finishing can drop its dispatcher
 * at any moment, and an atomic raw pointer would only have made the pointer load safe, not the
 * object's lifetime -- the owning thread could free it between another thread's load and its call.
 * Holding a strong reference for the duration of the call keeps it alive until that caller is done.
 *
 * Access is private on purpose: a writable dispatcher handle would let outside code redirect a
 * running loop or drop a dispatcher still in use. Only the three classes that legitimately manage
 * a thread's lifecycle are granted access.
 */
struct GThreadData
{
private:
    /**
     * @brief Gets a strong reference to this thread's dispatcher.
     * @return Shared pointer to the dispatcher, or nullptr if none is installed. Thread-safe.
     */
    std::shared_ptr<GAbstractEventDispatcher> dispatcher() const
    {
        std::lock_guard<std::mutex> lock(m_dispatcherMutex);
        return m_dispatcher;
    }

    /**
     * @brief Installs or clears this thread's dispatcher.
     * @param dispatcher The dispatcher to install; pass nullptr to clear. Thread-safe.
     */
    void setDispatcher(std::shared_ptr<GAbstractEventDispatcher> dispatcher)
    {
        std::lock_guard<std::mutex> lock(m_dispatcherMutex);
        m_dispatcher = std::move(dispatcher);
    }

    mutable std::mutex m_dispatcherMutex;
    std::shared_ptr<GAbstractEventDispatcher> m_dispatcher;

    friend class GObject;
    friend class GThread;
    friend class GCoreApplication;
};

/**
 * @brief Base class for all objects participating in the signal-slot and event system.
 */
class GObject
{
public:
    /**
     * @brief Constructs an object.
     */
    GObject();

    /**
     * @brief Destroys the object and triggers all registered cleanup callbacks.
     */
    virtual ~GObject();

    /**
     * @brief GObject is neither copyable nor movable.
     *
     * These are already deleted implicitly, because the class holds std::mutex members -- but
     * only by accident. Stating it makes the guarantee survive refactoring: m_life is a
     * shared_ptr, so a copy would raise its use count and ~GObject()'s m_life.reset() would no
     * longer expire the token. Every connect()/callLater() wrapper's weakLife.lock() would keep
     * succeeding and invoke slots on a destroyed object -- a use-after-free reintroduced silently
     * by an unrelated change.
     */
    GObject(const GObject&) = delete;
    GObject& operator=(const GObject&) = delete;
    GObject(GObject&&) = delete;
    GObject& operator=(GObject&&) = delete;

    /**
     * @brief Gets the thread affinity of this object.
     * @return A pointer to the thread this object lives in. Thread-safe.
     */
    GThread* thread() const;

    /**
     * @brief Changes the thread affinity of this object.
     *
     * **Not thread-safe: must be called from this object's own thread**, matching Qt's
     * QObject::moveToThread() ("Current thread is not the object's thread. Cannot move to target
     * thread"). Only the thread that currently owns an object may hand it to another; letting any
     * thread re-home an object at will would race the owner's own use of it.
     *
     * Qt's one exception is reproduced: an object with *no* thread affinity yet may be adopted by
     * the calling thread. That is what lets a freshly constructed object be moved onto a worker,
     * and what lets GThread adopt itself when its run loop starts.
     * @param thread The new thread this object will live in; nullptr clears the affinity.
     * @return True if the object now lives in the requested thread (including when it already
     * did); false if the move was refused, in which case the affinity is unchanged.
     */
    bool moveToThread(GThread* thread);

    /**
     * @brief Gets the object's descriptive name.
     * @return The object name. Thread-safe.
     */
    std::string objectName() const;

    /**
     * @brief Sets the object's descriptive name.
     * @param name The new object name. Thread-safe.
     */
    void setObjectName(const std::string& name);

    /**
     * @brief Schedules this object for deletion in the event loop. Thread-safe.
     */
    void deleteLater();

    /**
     * @brief Handles timer events sent to this object.
     * @param event The timer event containing the timer ID.
     */
    virtual void timerEvent(GTimerEvent* event);

    /**
     * @brief Starts a timer for this object with the specified interval.
     *
     * **Not thread-safe: must be called from this object's own thread.** Timers are owned by the
     * dispatcher of the thread the object lives in, and only that thread's event loop can deliver
     * the resulting timerEvent(). Calling from any other thread is rejected with a warning on
     * stderr and returns -1, matching Qt, whose QObject::startTimer() likewise refuses
     * ("Timers cannot be started from another thread"). To start a timer for an object living in
     * another thread, get onto that thread first -- for example with callLater().
     * @param interval Interval in milliseconds.
     * @return Unique timer ID, or -1 if the timer could not be started.
     */
    int startTimer(int interval);

    /**
     * @brief Kills the timer with the specified ID.
     *
     * **Not thread-safe: must be called from this object's own thread**, for the same reason as
     * startTimer(). Calls from another thread are rejected with a warning and do nothing.
     * @param id The timer ID to stop.
     */
    void killTimer(int id);

    /**
     * @brief Registers a callback to be executed when this object is destroyed.
     * @param callback The function to execute upon destruction. Thread-safe.
     */
    void addCleanupCallback(std::function<void()> callback);

    /**
     * @brief Gets the weak pointer tracking the lifetime of this object.
     * @return A weak pointer to this object's life token. Thread-safe.
     */
    std::weak_ptr<int> objectLife() const
    {
        return m_life;
    }

    /**
     * @brief [Connect Overload 1]: Connects a signal to a non-overloaded member function slot.
     *
     * Why it exists: This is the primary overload for standard member functions. Because the target
     * slot is not overloaded, the compiler can directly deduce the `Slot` type without needing
     * explicit template resolution.
     * @tparam Signal The signal type.
     * @tparam Receiver The receiver object type (must derive from GObject).
     * @tparam Slot The member function pointer type.
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The member function to call when the signal is emitted.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename Signal, typename Receiver, typename Slot>
    static std::enable_if_t<G::MemberFunctionTraits<Slot>::is_member_function, G::ConnectionHandle>
    connect(Signal& signal,
            Receiver* receiver,
            Slot slot,
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 2]: Connects an overloaded void member function slot inherited from
     * a base class.
     *
     * Why it exists: If the target slot is overloaded, the compiler cannot deduce `Slot` in
     * Overload 1. When the overloaded slot is defined in a base class of the receiver, type
     * deduction fails. This overload explicitly resolves the base class pointer so you can connect
     * inherited overloaded methods seamlessly.
     * @tparam SignalArgs Parameter types of the signal used to select the slot overload.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Base class owning the member function slot.
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The member function pointer matching SignalArgs.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver, typename SlotClass>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                std::is_base_of<SlotClass, Receiver>::value &&
                                !std::is_same<SlotClass, Receiver>::value,
                            G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void (SlotClass::*slot)(SignalArgs...),
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 3]: Connects an overloaded const void member function slot inherited
     * from a base class.
     *
     * Why it exists: Similar to Overload 2, but specifically for const member functions. C++
     * requires separate template matching for const qualifiers on member function pointers.
     * @tparam SignalArgs Parameter types of the signal used to select the slot overload.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Base class owning the member function slot.
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The const member function pointer matching SignalArgs.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver, typename SlotClass>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                std::is_base_of<SlotClass, Receiver>::value &&
                                !std::is_same<SlotClass, Receiver>::value,
                            G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void (SlotClass::*slot)(SignalArgs...) const,
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 4]: Connects an overloaded non-void returning member function slot
     * inherited from a base class.
     *
     * Why it exists: If an overloaded inherited slot returns a value (e.g. `bool`), it won\'t match
     * the void-returning Overloads 2 and 3. This overload explicitly catches non-void slots from
     * base classes (the return value is safely discarded during emission).
     * @tparam SignalArgs Parameter types of the signal used to select the slot overload.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Base class owning the member function slot.
     * @tparam Ret Return type of the slot (discarded upon invocation).
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The member function pointer matching SignalArgs and returning Ret.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
    static std::enable_if_t<
        std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret (SlotClass::*slot)(SignalArgs...),
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 5]: Connects an overloaded non-void returning const member function
     * slot inherited from a base class.
     *
     * Why it exists: Similar to Overload 4, but specifically for const member functions.
     * @tparam SignalArgs Parameter types of the signal used to select the slot overload.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Base class owning the member function slot.
     * @tparam Ret Return type of the slot (discarded upon invocation).
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The const member function pointer matching SignalArgs and returning Ret.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
    static std::enable_if_t<
        std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret (SlotClass::*slot)(SignalArgs...) const,
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 6]: Connects an overloaded void member function slot defined
     * directly on the receiver.
     *
     * Why it exists: If the target slot is overloaded (e.g. `onEvent()` and `onEvent(int)`), the
     * compiler cannot deduce `Slot` in Overload 1. By using `G::NonDeduced<Receiver>`, this
     * overload forces the compiler to use `SignalArgs` from the signal to perfectly select the
     * right overload pointer.
     * @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
     * signature.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The member function pointer matching SignalArgs.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void (G::NonDeduced<Receiver>::*slot)(SignalArgs...),
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 7]: Connects an overloaded const void member function slot defined
     * directly on the receiver.
     *
     * Why it exists: Similar to Overload 6, but specifically matches const member functions.
     * @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
     * signature.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The const member function pointer matching SignalArgs.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void (G::NonDeduced<Receiver>::*slot)(SignalArgs...) const,
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 8]: Connects an overloaded non-void returning member function slot
     * defined directly on the receiver.
     *
     * Why it exists: If an overloaded slot returns a value (e.g. `bool`), it won\'t match the
     * void-returning Overload 6. This overload ensures connecting an overloaded method that returns
     * `Ret` compiles successfully.
     * @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
     * signature.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Ret Return type of the slot (discarded upon invocation).
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The member function pointer matching SignalArgs and returning Ret.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver, typename Ret>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                !std::is_same<Ret, void>::value,
                            G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret (G::NonDeduced<Receiver>::*slot)(SignalArgs...),
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 9]: Connects an overloaded non-void returning const member function
     * slot defined directly on the receiver.
     *
     * Why it exists: Similar to Overload 8, but specifically for const member functions.
     * @tparam SignalArgs Parameter types of the signal used to deduce and select the slot overload
     * signature.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Ret Return type of the slot (discarded upon invocation).
     * @param signal The signal to connect.
     * @param receiver The object receiving the signal.
     * @param slot The const member function pointer matching SignalArgs and returning Ret.
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename... SignalArgs, typename Receiver, typename Ret>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                !std::is_same<Ret, void>::value,
                            G::ConnectionHandle>
    connect(GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret (G::NonDeduced<Receiver>::*slot)(SignalArgs...) const,
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief [Connect Overload 10]: Connects a signal to a free function, lambda, or general
     * functor slot.
     *
     * Why it exists: This overload captures anything that is not a member function. It binds the
     * functor\'s lifetime and thread affinity to the provided `context` object.
     * @tparam Signal The signal type.
     * @tparam Functor The slot functor type.
     * @param signal The signal to connect.
     * @param context The GObject context defining thread affinity and lifetime.
     * @param slot The slot functor (lambda, std::function, etc.).
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename Signal, typename Functor>
    static std::enable_if_t<!G::MemberFunctionTraits<Functor>::is_member_function,
                            G::ConnectionHandle>
    connect(Signal& signal,
            GObject* context,
            Functor slot,
            G::ConnectionType type = G::AutoConnection);

    /**
     * @brief Disconnects a signal connection using a connection handle.
     * @param handle The handle to disconnect. Thread-safe.
     */
    static void disconnect(const G::ConnectionHandle& handle);

    /**
     * @brief [CallLater Overload 1]: Schedules a non-overloaded member function slot to run
     * deferred.
     *
     * Why it exists: This is the primary overload for standard member functions. Because the target
     * slot is not overloaded, the compiler can directly deduce the `Slot` type.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Slot Member function pointer type.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename Slot, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                G::MemberFunctionTraits<Slot>::is_member_function,
                            void>
    callLater(Receiver* receiver, Slot slot, Args&&... args);

    /**
     * @brief [CallLater Overload 2]: Schedules an overloaded void member function slot inherited
     * from a base class.
     *
     * Why it exists: If the target slot is overloaded and inherited from a base class, type
     * deduction fails. This overload explicitly resolves the base class pointer so you can defer
     * execution of inherited overloaded methods.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Class owning the member function slot.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename SlotClass, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                std::is_base_of<SlotClass, Receiver>::value &&
                                !std::is_same<SlotClass, Receiver>::value,
                            void>
    callLater(Receiver* receiver, void (SlotClass::*slot)(G::NonDeduced<Args>...), Args&&... args);

    /**
     * @brief [CallLater Overload 3]: Schedules an overloaded const void member function slot
     * inherited from a base class.
     *
     * Why it exists: Similar to Overload 2, but specifically for const member functions.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Class owning the member function slot.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Const member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename SlotClass, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                std::is_base_of<SlotClass, Receiver>::value &&
                                !std::is_same<SlotClass, Receiver>::value,
                            void>
    callLater(Receiver* receiver,
              void (SlotClass::*slot)(G::NonDeduced<Args>...) const,
              Args&&... args);

    /**
     * @brief [CallLater Overload 4]: Schedules an overloaded non-void returning member function
     * slot inherited from a base class.
     *
     * Why it exists: If an overloaded inherited slot returns a value, it won\'t match the
     * void-returning overloads. This overload explicitly catches non-void slots from base classes
     * (the return value is safely discarded upon invocation).
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Class owning the member function slot.
     * @tparam Ret Return type of the slot.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename SlotClass, typename Ret, typename... Args>
    static std::enable_if_t<
        std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        void>
    callLater(Receiver* receiver, Ret (SlotClass::*slot)(G::NonDeduced<Args>...), Args&&... args);

    /**
     * @brief [CallLater Overload 5]: Schedules an overloaded non-void returning const member
     * function slot inherited from a base class.
     *
     * Why it exists: Similar to Overload 4, but specifically for const member functions.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam SlotClass Class owning the member function slot.
     * @tparam Ret Return type of the slot.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Const member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename SlotClass, typename Ret, typename... Args>
    static std::enable_if_t<
        std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        void>
    callLater(Receiver* receiver,
              Ret (SlotClass::*slot)(G::NonDeduced<Args>...) const,
              Args&&... args);

    /**
     * @brief [CallLater Overload 6]: Schedules an overloaded void member function slot defined
     * directly on the receiver.
     *
     * Why it exists: If the target slot is overloaded, the compiler cannot deduce `Slot` in
     * Overload 1. Using `G::NonDeduced<Receiver>`, this overload forces the compiler to use the
     * passed `args` types to select the right overload.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>
    callLater(Receiver* receiver,
              void (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...),
              Args&&... args);

    /**
     * @brief [CallLater Overload 7]: Schedules an overloaded const void member function slot
     * defined directly on the receiver.
     *
     * Why it exists: Similar to Overload 6, but specifically for const member functions.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Const member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>
    callLater(Receiver* receiver,
              void (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...) const,
              Args&&... args);

    /**
     * @brief [CallLater Overload 8]: Schedules an overloaded non-void returning member function
     * slot defined directly on the receiver.
     *
     * Why it exists: If an overloaded slot returns a value, it won\'t match void-returning
     * Overload 6. This ensures deferring overloaded methods that return `Ret` compiles
     * successfully.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Ret Return type of the slot.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename Ret, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                !std::is_same<Ret, void>::value,
                            void>
    callLater(Receiver* receiver,
              Ret (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...),
              Args&&... args);

    /**
     * @brief [CallLater Overload 9]: Schedules an overloaded non-void returning const member
     * function slot defined directly on the receiver.
     *
     * Why it exists: Similar to Overload 8, but specifically for const member functions.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Ret Return type of the slot.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Const member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename Ret, typename... Args>
    static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                                !std::is_same<Ret, void>::value,
                            void>
    callLater(Receiver* receiver,
              Ret (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...) const,
              Args&&... args);

    /**
     * @brief [CallLater Overload 10]: Schedules a static or free function to run deferred.
     *
     * Why it exists: This overload captures static and free functions. It binds the function\'s
     * execution to the provided `context` object\'s thread loop.
     * @tparam Func Function pointer type.
     * @tparam Args Argument types.
     * @param context Target GObject defining thread affinity and lifetime.
     * @param func Function pointer.
     * @param args Arguments passed to function. Thread-safe.
     */
    template <typename Func, typename... Args>
    static std::enable_if_t<std::is_pointer<Func>::value &&
                                std::is_function<std::remove_pointer_t<Func>>::value,
                            void>
    callLater(GObject* context, Func func, Args&&... args);

    /**
     * @brief [CallLater Overload 11]: Schedules a GSignal emission to run deferred.
     *
     * Why it exists: Specifically allows `callLater` to queue a signal emission
     * (`signal.emit(args...)`) on a target thread instead of executing a function.
     * @tparam SignalArgs Signal parameter types.
     * @tparam Args Argument types.
     * @param context Target GObject defining thread affinity and lifetime.
     * @param signal GSignal instance to emit.
     * @param args Arguments passed to signal. Thread-safe.
     */
    template <typename... SignalArgs, typename... Args>
    static void callLater(GObject* context, GSignal<SignalArgs...>& signal, Args&&... args);

    /**
     * @brief [CallLater Overload 12]: Fallback overload producing a compile-time error for
     * unsupported targets (e.g., lambdas).
     *
     * Why it exists: `callLater` relies on hashing the target address for deduplication. Lambdas
     * cannot be reliably hashed, so this overload intentionally catches lambdas and triggers a
     * `static_assert` or SFINAE error.
     * @tparam Target Callable target type.
     * @tparam Args Argument types.
     * @param context Target GObject context.
     * @param target Unsupported callable object (e.g. lambda).
     * @param args Arguments.
     */
    template <typename Target, typename... Args>
    static std::enable_if_t<!G::MemberFunctionTraits<Target>::is_member_function &&
                                !(std::is_pointer<Target>::value &&
                                  std::is_function<std::remove_pointer_t<Target>>::value) &&
                                !G::IsGSignal<std::decay_t<Target>>::value,
                            void>
    callLater(GObject* context, Target&& target, Args&&... args);

private:
    /**
     * @brief Key identifying a deduplicated deferred call.
     *
     * Implementation detail of callLater()'s per-cycle deduplication; not part of the API.
     */
    struct GCallLaterKey
    {
        /** @brief Target context GObject. */
        GObject* context{nullptr};
        /** @brief Type hash code of the callable target. */
        size_t typeHash{0};
        /** @brief Size of the callable target representation in bytes. */
        size_t targetSize{0};
        /** @brief Binary payload representing the callable target. */
        std::array<uint8_t, 32> targetBytes{};

        /**
         * @brief Compares two keys for equality.
         * @param other Key to compare.
         * @return True if equal.
         */
        bool operator==(const GCallLaterKey& other) const
        {
            if (context != other.context || typeHash != other.typeHash ||
                targetSize != other.targetSize)
            {
                return false;
            }
            return std::memcmp(targetBytes.data(), other.targetBytes.data(), targetSize) == 0;
        }
    };

    /**
     * @brief Hash functor for GCallLaterKey.
     */
    struct GCallLaterKeyHash
    {
        /**
         * @brief Computes hash value for key.
         * @param key Key to hash.
         * @return Hash code.
         */
        size_t operator()(const GCallLaterKey& key) const
        {
            size_t h = std::hash<GObject*>()(key.context) ^ (key.typeHash << 1);
            for (size_t i = 0; i < key.targetSize; ++i)
            {
                h = h * 31 + key.targetBytes[i];
            }
            return h;
        }
    };

    /**
     * @brief Internal helper to schedule or update a callLater deferred invocation.
     * @param context Target context object.
     * @param key Key identifying the deferred call.
     * @param invoker Callback executing the call.
     */
    static void
    scheduleCallLater(GObject* context, const GCallLaterKey& key, std::function<void()> invoker);

    /**
     * @brief Gets the thread data container holding this object's event dispatcher.
     *
     * Private: this is internal plumbing with no QObject equivalent -- Qt's
     * QObjectPrivate::threadData is likewise not public API. It is the handle through which the
     * dispatcher is reached, so exposing it hands out the machinery every other access-control
     * decision in this class exists to protect.
     * @return A shared pointer to the thread data, or nullptr if this object has no affinity.
     * Thread-safe.
     */
    std::shared_ptr<GThreadData> threadData() const;

    /**
     * @brief Internal event dispatch plumbing; routes an event to its handler.
     *
     * Deliberately private and non-virtual: this is not an extension point. The event queue is
     * the sole caller (see the friend declaration below), and the set of event types is closed.
     * Override timerEvent() instead to react to timers.
     * @param event The event to handle.
     * @return True if the event was recognized and handled.
     */
    bool event(GEvent* event);

    /**
     * @brief Dispatches a metacall callback to the target object's event loop based on connection
     * type.
     * @param target Target GObject.
     * @param slot Callback function.
     * @param type Connection type. Thread-safe.
     * @return True if the slot ran (direct) or was queued successfully; false if it could not be
     * delivered at all, which happens when the target has no thread affinity or its thread has no
     * event dispatcher yet. Callers that track pending state must undo it when this returns false.
     */
    static bool
    dispatchMetaCall(GObject* target, std::function<void()> slot, G::ConnectionType type);

    /** @brief Grants the event queue access to event(), which it alone invokes. */
    friend class GEventDispatcherDefault;

    /**
     * @brief Grants the callLater pending-call registry (defined in GObject.cpp) the ability to
     * name the private GCallLaterKey/GCallLaterKeyHash types its map is keyed on.
     */
    friend struct GCallLaterRegistry;

    std::shared_ptr<int> m_life;
    std::shared_ptr<GThreadData> m_threadData;
    mutable std::mutex m_threadDataMutex;
    std::atomic<GThread*> m_thread{nullptr};
    std::string m_objectName;
    mutable std::mutex m_nameMutex;
    std::vector<std::function<void()>> m_cleanupCallbacks;
    std::mutex m_cleanupMutex;
    static std::atomic<int> s_nextTimerId;
};

template <typename Signal, typename Receiver, typename Slot>
std::enable_if_t<G::MemberFunctionTraits<Slot>::is_member_function, G::ConnectionHandle>
GObject::connect(Signal& signal, Receiver* receiver, Slot slot, G::ConnectionType type)
{
    using SlotClass = typename G::MemberFunctionTraits<Slot>::class_type;

    static_assert(
        std::is_base_of<GObject, Receiver>::value, "Receiver must be an instance of GObject.");
    static_assert(G::MemberFunctionTraits<Slot>::is_member_function,
                  "Slot must be a member function pointer.");
    static_assert(std::is_base_of<SlotClass, Receiver>::value,
                  "Slot must be a member function of Receiver or one of its base classes.");

    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](auto&&... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver, typename SlotClass>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value,
                 G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 void (SlotClass::*slot)(SignalArgs...),
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver, typename SlotClass>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value,
                 G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 void (SlotClass::*slot)(SignalArgs...) const,
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
                 G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 Ret (SlotClass::*slot)(SignalArgs...),
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
                 G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 Ret (SlotClass::*slot)(SignalArgs...) const,
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 void (G::NonDeduced<Receiver>::*slot)(SignalArgs...),
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 void (G::NonDeduced<Receiver>::*slot)(SignalArgs...) const,
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver, typename Ret>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value,
                 G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 Ret (G::NonDeduced<Receiver>::*slot)(SignalArgs...),
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename... SignalArgs, typename Receiver, typename Ret>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value,
                 G::ConnectionHandle>
GObject::connect(GSignal<SignalArgs...>& signal,
                 Receiver* receiver,
                 Ret (G::NonDeduced<Receiver>::*slot)(SignalArgs...) const,
                 G::ConnectionType type)
{
    if (!receiver)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](SignalArgs... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename Signal, typename Functor>
std::enable_if_t<!G::MemberFunctionTraits<Functor>::is_member_function, G::ConnectionHandle>
GObject::connect(Signal& signal, GObject* context, Functor slot, G::ConnectionType type)
{
    if (!context)
    {
        return {};
    }

    std::weak_ptr<int> weakLife = context->objectLife();

    auto wrapper = [weakLife, context, slot, type](auto&&... args)
    {
        auto life = weakLife.lock();
        if (!life)
        {
            return;
        }

        auto boundSlot = [weakLife, slot, args...]()
        {
            if (auto lifeCheck = weakLife.lock())
            {
                slot(args...);
            }
        };

        dispatchMetaCall(context, boundSlot, type);
    };

    return signal.connect(wrapper);
}

inline void GObject::disconnect(const G::ConnectionHandle& handle)
{
    handle.disconnect();
}

template <typename Receiver, typename Slot, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     G::MemberFunctionTraits<Slot>::is_member_function,
                 void>
GObject::callLater(Receiver* receiver, Slot slot, Args&&... args)
{
    using SlotClass = typename G::MemberFunctionTraits<Slot>::class_type;

    static_assert(
        std::is_base_of<GObject, Receiver>::value, "Receiver must be an instance of GObject.");
    static_assert(G::MemberFunctionTraits<Slot>::is_member_function,
                  "Slot must be a member function pointer.");
    static_assert(std::is_base_of<SlotClass, Receiver>::value,
                  "Slot must be a member function of Receiver or one of its base classes.");
    static_assert(std::is_invocable_v<Slot, Receiver*, Args...>,
                  "Arguments do not match the parameters of the member function.");

    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(Slot).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename SlotClass, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value,
                 void>
GObject::callLater(Receiver* receiver,
                   void (SlotClass::*slot)(G::NonDeduced<Args>...),
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(void (SlotClass::*)(Args...)).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename SlotClass, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value,
                 void>
GObject::callLater(Receiver* receiver,
                   void (SlotClass::*slot)(G::NonDeduced<Args>...) const,
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(void (SlotClass::*)(Args...) const).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename SlotClass, typename Ret, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
                 void>
GObject::callLater(Receiver* receiver,
                   Ret (SlotClass::*slot)(G::NonDeduced<Args>...),
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(Ret (SlotClass::*)(Args...)).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename SlotClass, typename Ret, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
                     std::is_base_of<SlotClass, Receiver>::value &&
                     !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
                 void>
GObject::callLater(Receiver* receiver,
                   Ret (SlotClass::*slot)(G::NonDeduced<Args>...) const,
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(Ret (SlotClass::*)(Args...) const).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>
GObject::callLater(Receiver* receiver,
                   void (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...),
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(void (Receiver::*)(Args...)).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>
GObject::callLater(Receiver* receiver,
                   void (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...) const,
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(void (Receiver::*)(Args...) const).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename Ret, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value, void>
GObject::callLater(Receiver* receiver,
                   Ret (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...),
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(Ret (Receiver::*)(Args...)).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Receiver, typename Ret, typename... Args>
std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value, void>
GObject::callLater(Receiver* receiver,
                   Ret (G::NonDeduced<Receiver>::*slot)(G::NonDeduced<Args>...) const,
                   Args&&... args)
{
    if (!receiver)
    {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(Ret (Receiver::*)(Args...) const).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([receiver, slot](auto&&... a)
                   { (receiver->*slot)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Func, typename... Args>
std::enable_if_t<std::is_pointer<Func>::value &&
                     std::is_function<std::remove_pointer_t<Func>>::value,
                 void>
GObject::callLater(GObject* context, Func func, Args&&... args)
{
    static_assert(std::is_invocable_v<Func, Args...>,
                  "Arguments do not match the parameters of the function.");

    if (!context || !func)
    {
        return;
    }

    GCallLaterKey key;
    key.context = context;
    key.typeHash = typeid(Func).hash_code();
    key.targetSize = sizeof(func);
    static_assert(sizeof(func) <= 32, "Function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &func, sizeof(func));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [func, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([func](auto&&... a) { (*func)(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(context, key, invoker);
}

template <typename... SignalArgs, typename... Args>
void GObject::callLater(GObject* context, GSignal<SignalArgs...>& signal, Args&&... args)
{
    static_assert(std::is_invocable_v<GSignal<SignalArgs...>, Args...>,
                  "Arguments do not match the parameters of the signal.");

    if (!context)
    {
        return;
    }

    GSignal<SignalArgs...>* sigPtr = &signal;

    GCallLaterKey key;
    key.context = context;
    key.typeHash = typeid(GSignal<SignalArgs...>).hash_code();
    key.targetSize = sizeof(sigPtr);
    static_assert(sizeof(sigPtr) <= 32, "Signal pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &sigPtr, sizeof(sigPtr));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [sigPtr, tupleArgs = std::move(tupleArgs)]() mutable
    {
        std::apply([sigPtr](auto&&... a) { sigPtr->emit(std::forward<decltype(a)>(a)...); },
                   std::move(tupleArgs));
    };

    scheduleCallLater(context, key, invoker);
}

template <typename Target, typename... Args>
std::enable_if_t<!G::MemberFunctionTraits<Target>::is_member_function &&
                     !(std::is_pointer<Target>::value &&
                       std::is_function<std::remove_pointer_t<Target>>::value) &&
                     !G::IsGSignal<std::decay_t<Target>>::value,
                 void>
GObject::callLater(GObject* context, Target&& target, Args&&... args)
{
    (void)context;
    (void)target;
    static_assert(
        sizeof(Target) == 0, "Lambdas and general functors are not allowed in callLater.");
}

#endif // GOBJECT_H
