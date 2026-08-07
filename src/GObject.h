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

namespace QtLikeSignal
{
    class GThread;
    template <typename ... Args> class GSignal;
    class GAbstractEventDispatcher;
    class GEventDispatcherDefault;
    class GCoreApplication;

    //! Per-thread state owning that thread's event dispatcher.
    //!
    //! Handed out by GObject::threadData()/GThread::threadData() as an opaque handle. The dispatcher is
    //! held by shared_ptr and only ever reachable through dispatcher(), which hands back a *strong*
    //! reference. That is what makes cross-thread use safe: a thread finishing can drop its dispatcher
    //! at any moment, and an atomic raw pointer would only have made the pointer load safe, not the
    //! object's lifetime -- the owning thread could free it between another thread's load and its call.
    //! Holding a strong reference for the duration of the call keeps it alive until that caller is done.
    //!
    //! Access is private on purpose: a writable dispatcher handle would let outside code redirect a
    //! running loop or drop a dispatcher still in use. Only the three classes that legitimately manage
    //! a thread's lifecycle are granted access.
    struct GThreadData
    {
    private:
        //! Gets a strong reference to this thread's dispatcher, or nullptr if none is installed.
        //! Thread-safe.
        std::shared_ptr<GAbstractEventDispatcher> dispatcher() const
        {
            std::lock_guard<std::mutex> lock( m_dispatcherMutex );
            return m_dispatcher;
        }

        //! Installs or clears this thread's dispatcher. Thread-safe.
        void setDispatcher
            (
            std::shared_ptr<GAbstractEventDispatcher> dispatcher  //!< Dispatcher to install; nullptr clears it.
            )
        {
            std::lock_guard<std::mutex> lock( m_dispatcherMutex );
            m_dispatcher = std::move( dispatcher );
        }

        mutable std::mutex m_dispatcherMutex;                      //!< Guards m_dispatcher.
        std::shared_ptr<GAbstractEventDispatcher> m_dispatcher;    //!< This thread's dispatcher, if any.

        friend class GObject;
        friend class GThread;
        friend class GCoreApplication;
    };

    //! Base class for all objects participating in the signal-slot and event system.
    class GObject
    {
    public:
        GObject();
        virtual ~GObject();
        //! GObject is neither copyable nor movable.
        //!
        //! These are already deleted implicitly, because the class holds std::mutex members -- but
        //! only by accident. Stating it makes the guarantee survive refactoring: m_life is a
        //! shared_ptr, so a copy would raise its use count and ~GObject()'s m_life.reset() would no
        //! longer expire the token. Every connect()/callLater() wrapper's weakLife.lock() would keep
        //! succeeding and invoke slots on a destroyed object -- a use-after-free reintroduced silently
        //! by an unrelated change.
        GObject
            (
            const GObject&
            ) = delete;

        GObject& operator=
            (
            const GObject&
            ) = delete;
        GObject
            (
            GObject&&
            ) = delete;

        GObject& operator=
            (
            GObject&&
            ) = delete;
        GThread* thread() const;
        bool moveToThread
            (
            GThread* thread
            );
        std::string objectName() const;
        void setObjectName
            (
            const std::string& name
            );
        void deleteLater();
        virtual void timerEvent
            (
            GTimerEvent* event
            );
        int startTimer
            (
            int interval
            );
        void killTimer
            (
            int id
            );
        void addCleanupCallback
            (
            std::function<void()> callback
            );

        //! Gets the weak pointer tracking the lifetime of this object. Thread-safe.
        std::weak_ptr<int> objectLife() const
        {
            return m_life;
        }
        //! Connect Overload 1: connects a signal to a non-overloaded member function slot.
        template <typename Signal, typename Receiver, typename Slot>
        static std::enable_if_t<G::MemberFunctionTraits<Slot>::is_member_function, G::
            ConnectionHandle>
        connect
            (
            Signal& signal,
            Receiver* receiver,
            Slot slot,
            G::ConnectionType type = G::AutoConnection
            );

        //! Connect Overload 2: connects an overloaded void member function slot inherited from a
        //! base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void ( SlotClass::*slot )( SignalArgs... ),
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 3: connects an overloaded const void member function slot inherited
        //! from a base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void ( SlotClass::*slot )( SignalArgs... ) const,
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 4: connects an overloaded non-void returning member function slot
        //! inherited from a base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<
            std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret ( SlotClass::*slot )( SignalArgs... ),
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 5: connects an overloaded non-void returning const member function
        //! slot inherited from a base class.
        template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
        static std::enable_if_t<
            std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret ( SlotClass::*slot )( SignalArgs... ) const,
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 6: connects an overloaded void member function slot defined directly
        //! on the receiver.
        template <typename ... SignalArgs, typename Receiver>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ),
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 7: connects an overloaded const void member function slot defined
        //! directly on the receiver.
        template <typename ... SignalArgs, typename Receiver>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            void ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ) const,
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 8: connects an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        template <typename ... SignalArgs, typename Receiver, typename Ret>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ),
            G::ConnectionType type = G::AutoConnection );

        //! Connect Overload 9: connects an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        template <typename ... SignalArgs, typename Receiver, typename Ret>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            G::ConnectionHandle>
        connect( GSignal<SignalArgs...>& signal,
            Receiver* receiver,
            Ret ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ) const,
            G::ConnectionType type = G::AutoConnection );
        //! Connect Overload 10: connects a signal to a free function, lambda, or general functor
        //! slot.
        template <typename Signal, typename Functor>
        static std::enable_if_t<!G::MemberFunctionTraits<Functor>::is_member_function,
            G::ConnectionHandle>
        connect
            (
            Signal& signal,
            GObject* context,
            Functor slot,
            G::ConnectionType type = G::AutoConnection
            );
        static void disconnect
            (
            const G::ConnectionHandle& handle
            );
        //! CallLater Overload 1: schedules a non-overloaded member function slot to run deferred.
        template <typename Receiver, typename Slot, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            G::MemberFunctionTraits<Slot>::is_member_function,
            void>
        callLater
            (
            Receiver* receiver,
            Slot slot,
            Args&&... args
            );

        //! CallLater Overload 2: schedules an overloaded void member function slot inherited from
        //! a base class.
        template <typename Receiver, typename SlotClass, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            void>
        callLater( Receiver* receiver, void ( SlotClass::*slot )( G::NonDeduced<Args>... ), Args&&
            ...
            args );

        //! CallLater Overload 3: schedules an overloaded const void member function slot inherited
        //! from a base class.
        template <typename Receiver, typename SlotClass, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            std::is_base_of<SlotClass, Receiver>::value &&
            !std::is_same<SlotClass, Receiver>::value,
            void>
        callLater( Receiver* receiver,
            void ( SlotClass::*slot )( G::NonDeduced<Args>... ) const,
            Args&&... args );

        //! CallLater Overload 4: schedules an overloaded non-void returning member function slot
        //! inherited from a base class.
        template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
        static std::enable_if_t<
            std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* receiver, Ret ( SlotClass::*slot )( G::NonDeduced<Args>... ), Args&&...
            args );

        //! CallLater Overload 5: schedules an overloaded non-void returning const member function
        //! slot inherited from a base class.
        template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
        static std::enable_if_t<
            std::is_base_of<GObject, Receiver>::value && std::is_base_of<SlotClass, Receiver>::
            value &&
            !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* receiver,
            Ret ( SlotClass::*slot )( G::NonDeduced<Args>... ) const,
            Args&&... args );

        //! CallLater Overload 6: schedules an overloaded void member function slot defined
        //! directly on the receiver.
        template <typename Receiver, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>
        callLater( Receiver* receiver,
            void ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ),
            Args&&... args );

        //! CallLater Overload 7: schedules an overloaded const void member function slot defined
        //! directly on the receiver.
        template <typename Receiver, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>
        callLater( Receiver* receiver,
            void ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ) const,
            Args&&... args );

        //! CallLater Overload 8: schedules an overloaded non-void returning member function slot
        //! defined directly on the receiver.
        template <typename Receiver, typename Ret, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* receiver,
            Ret ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ),
            Args&&... args );

        //! CallLater Overload 9: schedules an overloaded non-void returning const member function
        //! slot defined directly on the receiver.
        template <typename Receiver, typename Ret, typename ... Args>
        static std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
            !std::is_same<Ret, void>::value,
            void>
        callLater( Receiver* receiver,
            Ret ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ) const,
            Args&&... args );
        //! CallLater Overload 10: schedules a static or free function to run deferred.
        template <typename Func, typename ... Args>
        static std::enable_if_t<std::is_pointer<Func>::value &&
            std::is_function<std::remove_pointer_t<Func> >::value,
            void>
        callLater
            (
            GObject* context,
            Func func,
            Args&&... args
            );
        //! CallLater Overload 11: schedules a GSignal emission to run deferred.
        template <typename ... SignalArgs, typename ... Args>
        static void callLater
            (
            GObject* context,
            GSignal<SignalArgs...>& signal,
            Args&&... args
            );
        //! CallLater Overload 12: fallback overload producing a compile-time error for unsupported
        //! targets (e.g. lambdas).
        template <typename Target, typename ... Args>
        static std::enable_if_t<!G::MemberFunctionTraits<Target>::is_member_function &&
            !( std::is_pointer<Target>::value &&
            std::is_function<std::remove_pointer_t<Target> >::value ) &&
            !G::IsGSignal<std::decay_t<Target> >::value,
            void>
        callLater
            (
            GObject* context,
            Target&& target,
            Args&&... args
            );

    private:
        //! Key identifying a deduplicated deferred call.
        //!
        //! Implementation detail of callLater()'s per-cycle deduplication; not part of the API.
        struct GCallLaterKey
        {
            GObject* context { nullptr };            //!< Target context GObject.
            size_t typeHash { 0 };                   //!< Type hash code of the callable target.
            size_t targetSize { 0 };                 //!< Size of the callable target representation, in bytes.
            std::array<uint8_t, 32> targetBytes {};  //!< Binary payload representing the callable target.

            //! Compares two keys for equality.
            bool operator==
                (
                const GCallLaterKey& other  //!< Key to compare.
                ) const
            {
                if( context != other.context || typeHash != other.typeHash ||
                    targetSize != other.targetSize )
                {
                    return false;
                }
                return std::memcmp( targetBytes.data(), other.targetBytes.data(), targetSize ) == 0;
            }

        };

        //! Hash functor for GCallLaterKey.
        struct GCallLaterKeyHash
        {
            //! Computes the hash value for a key.
            size_t operator()
                (
                const GCallLaterKey& key  //!< Key to hash.
                ) const
            {
                size_t h = std::hash<GObject*>()( key.context ) ^ ( key.typeHash << 1 );
                for( size_t i = 0; i < key.targetSize; ++i )
                {
                    h = h * 31 + key.targetBytes[i];
                }
                return h;
            }

        };
        static void
        scheduleCallLater
            (
            GObject* context,
            const GCallLaterKey& key,
            std::function<void()> invoker
            );
        std::shared_ptr<GThreadData> threadData() const;
        bool event
            (
            GEvent* event
            );
        static bool
        dispatchMetaCall
            (
            GObject* target,
            std::function<void()> slot,
            G::ConnectionType type
            );

        //! Grants the event queue access to event(), which it alone invokes.
        friend class GEventDispatcherDefault;

        //! Grants the callLater pending-call registry (defined in GObject.cpp) the ability to
        //! name the private GCallLaterKey/GCallLaterKeyHash types its map is keyed on.
        friend struct GCallLaterRegistry;

        //! Grants GThread access to dispatchMetaCall(), which GThread::post() uses to queue an
        //! arbitrary task onto itself.
        friend class GThread;

        std::shared_ptr<int> m_life;                          //!< Lifetime token; reset in ~GObject() so weak references expire.
        std::shared_ptr<GThreadData> m_threadData;             //!< Thread data of the thread this object lives in, if any.
        mutable std::mutex m_threadDataMutex;                 //!< Guards m_threadData.
        std::atomic<GThread*> m_thread { nullptr };           //!< The thread this object lives in.
        std::string m_objectName;                              //!< This object's descriptive name.
        mutable std::mutex m_nameMutex;                       //!< Guards m_objectName.
        std::vector<std::function<void()> > m_cleanupCallbacks;  //!< Callbacks to run on destruction.
        std::mutex m_cleanupMutex;                            //!< Guards m_cleanupCallbacks.
        static std::atomic<int> s_nextTimerId;                //!< Process-wide timer id counter.
    };

    //! Connect Overload 1 definition. This is the primary overload for standard member functions.
    //! Because the target slot is not overloaded, the compiler can directly deduce the Slot type
    //! without needing explicit template resolution.
    template <typename Signal, typename Receiver, typename Slot>
    std::enable_if_t<G::MemberFunctionTraits<Slot>::is_member_function, G::ConnectionHandle>
    GObject::
    connect
        (
        Signal& signal,          //!< The signal to connect.
        Receiver* receiver,      //!< The object receiving the signal (must derive from GObject).
        Slot slot,                //!< The member function to call when the signal is emitted.
        G::ConnectionType type   //!< The type of connection.
        )
    {
        using SlotClass = typename G::MemberFunctionTraits<Slot>::class_type;

        static_assert(
            std::is_base_of<GObject, Receiver>::value, "Receiver must be an instance of GObject." );
        static_assert( G::MemberFunctionTraits<Slot>::is_member_function,
            "Slot must be a member function pointer." );
        static_assert( std::is_base_of<SlotClass, Receiver>::value,
            "Slot must be a member function of Receiver or one of its base classes." );

        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( auto&&... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 2 definition. If the target slot is overloaded, the compiler cannot deduce
    //! Slot in Overload 1. When the overloaded slot is defined in a base class of the receiver,
    //! type deduction fails. This overload explicitly resolves the base class pointer so you can
    //! connect inherited overloaded methods seamlessly. SignalArgs are the signal's parameter
    //! types, used to select the slot overload; Receiver must derive from GObject; SlotClass is
    //! the base class owning the member function slot.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,           //!< The signal to connect.
        Receiver* receiver,                        //!< The object receiving the signal.
        void ( SlotClass::*slot )
        (
        SignalArgs...
        ),                                          //!< The member function pointer matching SignalArgs.
        G::ConnectionType type                     //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 3 definition. Same as Overload 2, but specifically for const member
    //! functions; C++ requires separate template matching for const qualifiers on member
    //! function pointers.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        void ( SlotClass::*slot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 4 definition. If an overloaded inherited slot returns a value (e.g. bool),
    //! it won't match the void-returning Overloads 2 and 3. This overload explicitly catches
    //! non-void slots from base classes; the return value is safely discarded during emission.
    //! Ret is that discarded return type.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        Ret ( SlotClass::*slot )
        (
        SignalArgs...
        ),                                    //!< The member function pointer matching SignalArgs and returning Ret.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 5 definition. Same as Overload 4, but specifically for const member
    //! functions.
    template <typename ... SignalArgs, typename Receiver, typename SlotClass, typename Ret>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        Ret ( SlotClass::*slot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs and returning Ret.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 6 definition. If the target slot is overloaded (e.g. onEvent() and
    //! onEvent(int)), the compiler cannot deduce Slot in Overload 1. Using G::NonDeduced<Receiver>
    //! forces the compiler to use SignalArgs from the signal to perfectly select the right
    //! overload pointer. SignalArgs is used both to deduce and to select the slot overload.
    template <typename ... SignalArgs, typename Receiver>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        void ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ),  //!< The member function pointer matching SignalArgs.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 7 definition. Same as Overload 6, but specifically matches const member
    //! functions.
    template <typename ... SignalArgs, typename Receiver>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value, G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        void ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 8 definition. If an overloaded slot returns a value (e.g. bool), it won't
    //! match the void-returning Overload 6. This overload ensures connecting an overloaded method
    //! that returns Ret compiles successfully.
    template <typename ... SignalArgs, typename Receiver, typename Ret>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value,
        G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        Ret ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ),  //!< The member function pointer matching SignalArgs and returning Ret.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 9 definition. Same as Overload 8, but specifically for const member
    //! functions.
    template <typename ... SignalArgs, typename Receiver, typename Ret>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value,
        G::ConnectionHandle>GObject::connect
        (
        GSignal<SignalArgs...>& signal,     //!< The signal to connect.
        Receiver* receiver,                  //!< The object receiving the signal.
        Ret ( G::NonDeduced<Receiver>::*slot )( SignalArgs... ) const,  //!< The const member function pointer matching SignalArgs and returning Ret.
        G::ConnectionType type               //!< The type of connection.
        )
    {
        if( !receiver )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = receiver->objectLife();

        auto wrapper = [weakLife, receiver, slot, type]( SignalArgs... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, receiver, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            ( receiver->*slot )( args ... );
                        }
                    };

                dispatchMetaCall( receiver, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Connect Overload 10 definition. Captures anything that is not a member function: free
    //! functions, lambdas, or general functors. Binds the functor's lifetime and thread affinity
    //! to the provided context object.
    template <typename Signal, typename Functor>
    std::enable_if_t<!G::MemberFunctionTraits<Functor>::is_member_function, G::ConnectionHandle>
    GObject::connect
        (
        Signal& signal,           //!< The signal to connect.
        GObject* context,         //!< The GObject context defining thread affinity and lifetime.
        Functor slot,             //!< The slot functor (lambda, std::function, etc.).
        G::ConnectionType type    //!< The type of connection.
        )
    {
        if( !context )
        {
            return {};
        }

        std::weak_ptr<int> weakLife = context->objectLife();

        auto wrapper = [weakLife, context, slot, type]( auto&&... args )
            {
                auto life = weakLife.lock();
                if( !life )
                {
                    return;
                }

                auto boundSlot = [weakLife, slot, args ...]()
                    {
                        if( auto lifeCheck = weakLife.lock() )
                        {
                            slot( args ... );
                        }
                    };

                dispatchMetaCall( context, boundSlot, type );
            };

        return signal.connect( wrapper );
    }

    //! Disconnects a signal connection using a connection handle. Thread-safe.
    inline void GObject::disconnect
        (
        const G::ConnectionHandle& handle  //!< The handle to disconnect.
        )
    {
        handle.disconnect();
    }

    //! CallLater Overload 1 definition. This is the primary overload for standard member
    //! functions. Because the target slot is not overloaded, the compiler can directly deduce the
    //! Slot type.
    template <typename Receiver, typename Slot, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        G::MemberFunctionTraits<Slot>::is_member_function,
        void>GObject::callLater
        (
        Receiver* receiver,  //!< Target object receiving the call.
        Slot slot,            //!< Member function pointer.
        Args&&... args        //!< Arguments passed to slot.
        )
    {
        using SlotClass = typename G::MemberFunctionTraits<Slot>::class_type;

        static_assert(
            std::is_base_of<GObject, Receiver>::value, "Receiver must be an instance of GObject." );
        static_assert( G::MemberFunctionTraits<Slot>::is_member_function,
            "Slot must be a member function pointer." );
        static_assert( std::is_base_of<SlotClass, Receiver>::value,
            "Slot must be a member function of Receiver or one of its base classes." );
        static_assert( std::is_invocable_v<Slot, Receiver*, Args...>,
            "Arguments do not match the parameters of the member function." );

        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( Slot ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 2 definition. If the target slot is overloaded and inherited from a base
    //! class, type deduction fails. This overload explicitly resolves the base class pointer so
    //! you can defer execution of inherited overloaded methods.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        void>GObject::callLater
        (
        Receiver* receiver,  //!< Target object receiving the call.
        void ( SlotClass::*slot )
        (
        G::NonDeduced<Args>...
        ),                    //!< Member function pointer.
        Args&&... args        //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( void ( SlotClass::* )( Args... ) ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 3 definition. Same as Overload 2, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value,
        void>GObject::callLater
        (
        Receiver* receiver,                                             //!< Target object receiving the call.
        void ( SlotClass::*slot )( G::NonDeduced<Args>... ) const,       //!< Const member function pointer.
        Args&&... args                                                   //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( void ( SlotClass::* )( Args... ) const ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 4 definition. If an overloaded inherited slot returns a value, it won't
    //! match the void-returning overloads. This overload explicitly catches non-void slots from
    //! base classes; the return value is safely discarded upon invocation.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        void>GObject::callLater
        (
        Receiver* receiver,  //!< Target object receiving the call.
        Ret ( SlotClass::*slot )
        (
        G::NonDeduced<Args>...
        ),                    //!< Member function pointer.
        Args&&... args        //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( Ret ( SlotClass::* )( Args... ) ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 5 definition. Same as Overload 4, but specifically for const member
    //! functions.
    template <typename Receiver, typename SlotClass, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        std::is_base_of<SlotClass, Receiver>::value &&
        !std::is_same<SlotClass, Receiver>::value && !std::is_same<Ret, void>::value,
        void>GObject::callLater
        (
        Receiver* receiver,                                        //!< Target object receiving the call.
        Ret ( SlotClass::*slot )( G::NonDeduced<Args>... ) const,   //!< Const member function pointer.
        Args&&... args                                              //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( Ret ( SlotClass::* )( Args... ) const ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 6 definition. If the target slot is overloaded, the compiler cannot
    //! deduce Slot in Overload 1. Using G::NonDeduced<Receiver>, this overload forces the compiler
    //! to use the passed args types to select the right overload.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>GObject::callLater
        (
        Receiver* receiver,                                                  //!< Target object receiving the call.
        void ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ),   //!< Member function pointer.
        Args&&... args                                                       //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( void ( Receiver::* )( Args... ) ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 7 definition. Same as Overload 6, but specifically for const member
    //! functions.
    template <typename Receiver, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value, void>GObject::callLater
        (
        Receiver* receiver,                                                        //!< Target object receiving the call.
        void ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ) const,   //!< Const member function pointer.
        Args&&... args                                                             //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( void ( Receiver::* )( Args... ) const ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 8 definition. If an overloaded slot returns a value, it won't match the
    //! void-returning Overload 6. This ensures deferring overloaded methods that return Ret
    //! compiles successfully.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value &&
        !std::is_same<Ret, void>::value,
        void>GObject::callLater
        (
        Receiver* receiver,                                                 //!< Target object receiving the call.
        Ret ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ),   //!< Member function pointer.
        Args&&... args                                                     //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( Ret ( Receiver::* )( Args... ) ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 9 definition. Same as Overload 8, but specifically for const member
    //! functions.
    template <typename Receiver, typename Ret, typename ... Args>
    std::enable_if_t<std::is_base_of<GObject, Receiver>::value && !std::is_same<Ret, void>::value,
        void>GObject::callLater
        (
        Receiver* receiver,                                                       //!< Target object receiving the call.
        Ret ( G::NonDeduced<Receiver>::*slot )( G::NonDeduced<Args>... ) const,   //!< Const member function pointer.
        Args&&... args                                                           //!< Arguments passed to slot.
        )
    {
        if( !receiver )
        {
            return;
        }

        GCallLaterKey key;
        key.context = receiver;
        key.typeHash = typeid( Ret ( Receiver::* )( Args... ) const ).hash_code();
        key.targetSize = sizeof( slot );
        static_assert( sizeof( slot ) <= 32, "Member function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &slot, sizeof( slot ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [receiver, slot, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [receiver, slot]( auto&&... a )
                    {
                        ( receiver->*slot )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( receiver, key, invoker );
    }

    //! CallLater Overload 10 definition. Captures static and free functions, binding their
    //! execution to the provided context object's thread loop.
    template <typename Func, typename ... Args>
    std::enable_if_t<std::is_pointer<Func>::value &&
        std::is_function<std::remove_pointer_t<Func> >::value,
        void>GObject::callLater
        (
        GObject* context,  //!< Target GObject defining thread affinity and lifetime.
        Func func,          //!< Function pointer.
        Args&&... args      //!< Arguments passed to function.
        )
    {
        static_assert( std::is_invocable_v<Func, Args...>,
            "Arguments do not match the parameters of the function." );

        if( !context || !func )
        {
            return;
        }

        GCallLaterKey key;
        key.context = context;
        key.typeHash = typeid( Func ).hash_code();
        key.targetSize = sizeof( func );
        static_assert( sizeof( func ) <= 32, "Function pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &func, sizeof( func ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [func, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [func]( auto&&... a )
                    {
                        (*func )( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( context, key, invoker );
    }

    //! CallLater Overload 11 definition. Allows callLater to queue a signal emission
    //! (signal.emit(args...)) on a target thread instead of executing a function. SignalArgs are
    //! the signal's parameter types.
    template <typename ... SignalArgs, typename ... Args>
    void GObject::callLater
        (
        GObject* context,             //!< Target GObject defining thread affinity and lifetime.
        GSignal<SignalArgs...>& signal,  //!< GSignal instance to emit.
        Args&&... args                 //!< Arguments passed to signal.
        )
    {
        static_assert( std::is_invocable_v<GSignal<SignalArgs...>, Args...>,
            "Arguments do not match the parameters of the signal." );

        if( !context )
        {
            return;
        }

        GSignal<SignalArgs...>* sigPtr = &signal;

        GCallLaterKey key;
        key.context = context;
        key.typeHash = typeid( GSignal<SignalArgs...> ).hash_code();
        key.targetSize = sizeof( sigPtr );
        static_assert( sizeof( sigPtr ) <= 32, "Signal pointer exceeds key size limit." );
        std::memcpy( key.targetBytes.data(), &sigPtr, sizeof( sigPtr ) );

        auto tupleArgs = std::make_tuple( std::forward<Args>( args )... );
        auto invoker = [sigPtr, tupleArgs = std::move( tupleArgs )]() mutable
            {
                std::apply( [sigPtr]( auto&&... a )
                    {
                        sigPtr->emit( std::forward<decltype( a )>( a )... );
                    },
                    std::move( tupleArgs ) );
            };

        scheduleCallLater( context, key, invoker );
    }

    //! CallLater Overload 12 definition. callLater relies on hashing the target address for
    //! deduplication. Lambdas cannot be reliably hashed, so this overload intentionally catches
    //! lambdas and general functors (Target) and triggers a static_assert.
    template <typename Target, typename ... Args>
    std::enable_if_t<!G::MemberFunctionTraits<Target>::is_member_function &&
        !( std::is_pointer<Target>::value &&
        std::is_function<std::remove_pointer_t<Target> >::value ) &&
        !G::IsGSignal<std::decay_t<Target> >::value,
        void>GObject::callLater
        (
        GObject* context,  //!< Target GObject context.
        Target&& target,   //!< Unsupported callable object (e.g. lambda).
        Args&&... args      //!< Arguments.
        )
    {
        ( void )context;
        ( void )target;
        static_assert(
            sizeof( Target ) == 0, "Lambdas and general functors are not allowed in callLater." );
    }
}

#endif // GOBJECT_H
