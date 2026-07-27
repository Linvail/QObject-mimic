#pragma once

#include "GGlobal.h"
#include "GEvent.h"
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <memory>
#include <atomic>
#include <type_traits>
#include <array>
#include <cstring>
#include <tuple>
#include <utility>

class GThread;
template <typename... Args> class GSignal;

/**
 * @brief Base class for all objects participating in the signal-slot and event system.
 */
class GObject {
public:
    /**
     * @brief Constructs an object with the given parent.
     * @param parent The parent object.
     */
    GObject(GObject* parent = nullptr);

    /**
     * @brief Destroys the object and triggers all registered cleanup callbacks.
     */
    virtual ~GObject();

    /**
     * @brief Gets the thread affinity of this object.
     * @return A pointer to the thread this object lives in. Thread-safe.
     */
    GThread* thread() const;

    /**
     * @brief Changes the thread affinity of this object.
     * @param thread The new thread this object will live in. Thread-safe.
     */
    void moveToThread(GThread* thread);

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
     * @brief Installs an event filter object on this object.
     * @param filterObj The object that will filter events. Thread-safe.
     */
    void installEventFilter(GObject* filterObj);

    /**
     * @brief Removes an event filter object from this object.
     * @param filterObj The event filter object to remove. Thread-safe.
     */
    void removeEventFilter(GObject* filterObj);

    /**
     * @brief Filters events if this object has been installed as an event filter.
     * @param watched The object being watched.
     * @param event The event to filter.
     * @return True if the event was filtered (consumed), false otherwise.
     */
    virtual bool eventFilter(GObject* watched, GEvent* event);

    /**
     * @brief Main event handler entry point.
     * @param event The event to handle.
     * @return True if the event was recognized and handled.
     */
    virtual bool event(GEvent* event);

    /**
     * @brief Handles custom events sent to this object.
     * @param event The event to handle.
     */
    virtual void customEvent(GEvent* event);

    /**
     * @brief Handles timer events sent to this object.
     * @param event The timer event containing the timer ID.
     */
    virtual void timerEvent(GTimerEvent* event);

    /**
     * @brief Starts a timer for this object with the specified interval.
     * @param interval Interval in milliseconds.
     * @return Unique timer ID. Thread-safe.
     */
    int startTimer(int interval);

    /**
     * @brief Kills the timer with the specified ID.
     * @param id The timer ID to stop. Thread-safe.
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
    std::weak_ptr<int> objectLife() const { return m_life; }

    /**
     * @brief Dispatches a metacall callback to the target object's event loop based on connection type.
     * @param target Target GObject.
     * @param slot Callback function.
     * @param type Connection type. Thread-safe.
     */
    static void dispatchMetaCall(GObject* target, std::function<void()> slot, G::ConnectionType type);

    /**
     * @brief Connects a signal to a member function slot on a receiver object.
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
    connect(Signal& signal, Receiver* receiver, Slot slot, G::ConnectionType type = G::AutoConnection);

    /**
     * @brief Connects a signal to a general functor or lambda slot under a receiver context.
     * @tparam Signal The signal type.
     * @tparam Functor The slot functor type.
     * @param signal The signal to connect.
     * @param context The GObject context that defines the target thread and lifetime.
     * @param slot The slot functor (lambda, functor, std::function, etc.).
     * @param type The type of connection.
     * @return A handle representing the connection. Thread-safe.
     */
    template <typename Signal, typename Functor>
    static std::enable_if_t<!G::MemberFunctionTraits<Functor>::is_member_function, G::ConnectionHandle>
    connect(Signal& signal, GObject* context, Functor slot, G::ConnectionType type = G::AutoConnection);

    /**
     * @brief Disconnects a signal connection using a connection handle.
     * @param handle The handle to disconnect. Thread-safe.
     */
    static void disconnect(const G::ConnectionHandle& handle);

    /**
     * @brief Schedules a member function slot to run deferred in the receiver's event loop with deduplication.
     * @tparam Receiver Receiver object type (must derive from GObject).
     * @tparam Slot Member function pointer type.
     * @tparam Args Argument types.
     * @param receiver Target object receiving the call.
     * @param slot Member function pointer.
     * @param args Arguments passed to slot. Thread-safe.
     */
    template <typename Receiver, typename Slot, typename... Args>
    static std::enable_if_t<
        std::is_base_of<GObject, Receiver>::value &&
        G::MemberFunctionTraits<Slot>::is_member_function,
        void
    > callLater(Receiver* receiver, Slot slot, Args&&... args);

    /**
     * @brief Schedules a static or free function to run deferred in the context's event loop with deduplication.
     * @tparam Func Function pointer type.
     * @tparam Args Argument types.
     * @param context Target GObject defining thread affinity and lifetime.
     * @param func Function pointer.
     * @param args Arguments passed to function. Thread-safe.
     */
    template <typename Func, typename... Args>
    static std::enable_if_t<
        std::is_pointer<Func>::value && std::is_function<std::remove_pointer_t<Func>>::value,
        void
    > callLater(GObject* context, Func func, Args&&... args);

    /**
     * @brief Schedules a GSignal emission to run deferred in the context's event loop with deduplication.
     * @tparam SignalArgs Signal parameter types.
     * @tparam Args Argument types.
     * @param context Target GObject defining thread affinity and lifetime.
     * @param signal GSignal instance to emit.
     * @param args Arguments passed to signal. Thread-safe.
     */
    template <typename... SignalArgs, typename... Args>
    static void callLater(GObject* context, GSignal<SignalArgs...>& signal, Args&&... args);

    /**
     * @brief Fallback overload that produces a compile-time error if a lambda or unsupported functor is passed.
     * @tparam Target Callable target type.
     * @tparam Args Argument types.
     * @param context Target GObject context.
     * @param target Unsupported callable object (e.g. lambda).
     * @param args Arguments.
     */
    template <typename Target, typename... Args>
    static std::enable_if_t<
        !G::MemberFunctionTraits<Target>::is_member_function &&
        !(std::is_pointer<Target>::value && std::is_function<std::remove_pointer_t<Target>>::value) &&
        !G::IsGSignal<std::decay_t<Target>>::value,
        void
    > callLater(GObject* context, Target&& target, Args&&... args);

    /**
     * @brief Key identifying a deduplicated deferred call.
     */
    struct GCallLaterKey {
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
        bool operator==(const GCallLaterKey& other) const {
            if (context != other.context || typeHash != other.typeHash || targetSize != other.targetSize) {
                return false;
            }
            return std::memcmp(targetBytes.data(), other.targetBytes.data(), targetSize) == 0;
        }
    };

    /**
     * @brief Hash functor for GCallLaterKey.
     */
    struct GCallLaterKeyHash {
        /**
         * @brief Computes hash value for key.
         * @param key Key to hash.
         * @return Hash code.
         */
        size_t operator()(const GCallLaterKey& key) const {
            size_t h = std::hash<GObject*>()(key.context) ^ (key.typeHash << 1);
            for (size_t i = 0; i < key.targetSize; ++i) {
                h = h * 31 + key.targetBytes[i];
            }
            return h;
        }
    };

private:
    /**
     * @brief Internal helper to schedule or update a callLater deferred invocation.
     * @param context Target context object.
     * @param key Key identifying the deferred call.
     * @param invoker Callback executing the call.
     */
    static void scheduleCallLater(GObject* context, const GCallLaterKey& key, std::function<void()> invoker);


    std::shared_ptr<int> m_life;
    std::atomic<GThread*> m_thread{nullptr};
    GObject* m_parent;
    std::string m_objectName;
    mutable std::mutex m_nameMutex;
    std::vector<GObject*> m_eventFilters;
    std::mutex m_eventFilterMutex;
    std::vector<std::function<void()>> m_cleanupCallbacks;
    std::mutex m_cleanupMutex;
    static std::atomic<int> s_nextTimerId;
};

template <typename Signal, typename Receiver, typename Slot>
std::enable_if_t<G::MemberFunctionTraits<Slot>::is_member_function, G::ConnectionHandle>
GObject::connect(Signal& signal, Receiver* receiver, Slot slot, G::ConnectionType type) {
    using SlotClass = typename G::MemberFunctionTraits<Slot>::class_type;

    static_assert(std::is_base_of<GObject, Receiver>::value,
                  "Receiver must be an instance of GObject.");
    static_assert(G::MemberFunctionTraits<Slot>::is_member_function,
                  "Slot must be a member function pointer.");
    static_assert(std::is_base_of<SlotClass, Receiver>::value,
                  "Slot must be a member function of Receiver or one of its base classes.");

    if (!receiver) {
        return {};
    }

    std::weak_ptr<int> weakLife = receiver->objectLife();

    auto wrapper = [weakLife, receiver, slot, type](auto&&... args) {
        auto life = weakLife.lock();
        if (!life) {
            return;
        }

        auto boundSlot = [weakLife, receiver, slot, args...]() {
            if (auto lifeCheck = weakLife.lock()) {
                (receiver->*slot)(args...);
            }
        };

        dispatchMetaCall(receiver, boundSlot, type);
    };

    return signal.connect(wrapper);
}

template <typename Signal, typename Functor>
std::enable_if_t<!G::MemberFunctionTraits<Functor>::is_member_function, G::ConnectionHandle>
GObject::connect(Signal& signal, GObject* context, Functor slot, G::ConnectionType type) {
    if (!context) {
        return {};
    }

    std::weak_ptr<int> weakLife = context->objectLife();

    auto wrapper = [weakLife, context, slot, type](auto&&... args) {
        auto life = weakLife.lock();
        if (!life) {
            return;
        }

        auto boundSlot = [weakLife, slot, args...]() {
            if (auto lifeCheck = weakLife.lock()) {
                slot(args...);
            }
        };

        dispatchMetaCall(context, boundSlot, type);
    };

    return signal.connect(wrapper);
}

inline void GObject::disconnect(const G::ConnectionHandle& handle) {
    handle.disconnect();
}

template <typename Receiver, typename Slot, typename... Args>
std::enable_if_t<
    std::is_base_of<GObject, Receiver>::value &&
    G::MemberFunctionTraits<Slot>::is_member_function,
    void
> GObject::callLater(Receiver* receiver, Slot slot, Args&&... args) {
    using SlotClass = typename G::MemberFunctionTraits<Slot>::class_type;

    static_assert(std::is_base_of<GObject, Receiver>::value,
                  "Receiver must be an instance of GObject.");
    static_assert(G::MemberFunctionTraits<Slot>::is_member_function,
                  "Slot must be a member function pointer.");
    static_assert(std::is_base_of<SlotClass, Receiver>::value,
                  "Slot must be a member function of Receiver or one of its base classes.");
    static_assert(std::is_invocable_v<Slot, Receiver*, Args...>,
                  "Arguments do not match the parameters of the member function.");

    if (!receiver) {
        return;
    }

    GCallLaterKey key;
    key.context = receiver;
    key.typeHash = typeid(Slot).hash_code();
    key.targetSize = sizeof(slot);
    static_assert(sizeof(slot) <= 32, "Member function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &slot, sizeof(slot));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [receiver, slot, tupleArgs = std::move(tupleArgs)]() mutable {
        std::apply([receiver, slot](auto&&... a) {
            (receiver->*slot)(std::forward<decltype(a)>(a)...);
        }, std::move(tupleArgs));
    };

    scheduleCallLater(receiver, key, invoker);
}

template <typename Func, typename... Args>
std::enable_if_t<
    std::is_pointer<Func>::value && std::is_function<std::remove_pointer_t<Func>>::value,
    void
> GObject::callLater(GObject* context, Func func, Args&&... args) {
    static_assert(std::is_invocable_v<Func, Args...>,
                  "Arguments do not match the parameters of the function.");

    if (!context || !func) {
        return;
    }

    GCallLaterKey key;
    key.context = context;
    key.typeHash = typeid(Func).hash_code();
    key.targetSize = sizeof(func);
    static_assert(sizeof(func) <= 32, "Function pointer exceeds key size limit.");
    std::memcpy(key.targetBytes.data(), &func, sizeof(func));

    auto tupleArgs = std::make_tuple(std::forward<Args>(args)...);
    auto invoker = [func, tupleArgs = std::move(tupleArgs)]() mutable {
        std::apply([func](auto&&... a) {
            (*func)(std::forward<decltype(a)>(a)...);
        }, std::move(tupleArgs));
    };

    scheduleCallLater(context, key, invoker);
}

template <typename... SignalArgs, typename... Args>
void GObject::callLater(GObject* context, GSignal<SignalArgs...>& signal, Args&&... args) {
    static_assert(std::is_invocable_v<GSignal<SignalArgs...>, Args...>,
                  "Arguments do not match the parameters of the signal.");

    if (!context) {
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
    auto invoker = [sigPtr, tupleArgs = std::move(tupleArgs)]() mutable {
        std::apply([sigPtr](auto&&... a) {
            sigPtr->emit(std::forward<decltype(a)>(a)...);
        }, std::move(tupleArgs));
    };

    scheduleCallLater(context, key, invoker);
}

template <typename Target, typename... Args>
std::enable_if_t<
    !G::MemberFunctionTraits<Target>::is_member_function &&
    !(std::is_pointer<Target>::value && std::is_function<std::remove_pointer_t<Target>>::value) &&
    !G::IsGSignal<std::decay_t<Target>>::value,
    void
> GObject::callLater(GObject* context, Target&& target, Args&&... args) {
    (void)context;
    (void)target;
    static_assert(sizeof(Target) == 0, "Lambdas and general functors are not allowed in callLater.");
}

