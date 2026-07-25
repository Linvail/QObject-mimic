#pragma once

#include <memory>
#include <type_traits>
#include <boost/signals2.hpp>

namespace G {
    /**
     * @brief Specifies the type of a signal-slot connection.
     */
    enum ConnectionType {
        /**
         * @brief Automatically determines the connection type based on thread affinity.
         *
         * If the receiver lives in the thread that emits the signal, DirectConnection is used.
         * Otherwise, QueuedConnection is used.
         */
        AutoConnection,

        /**
         * @brief The slot is invoked immediately when the signal is emitted.
         *
         * The slot is executed in the signaling thread.
         */
        DirectConnection,

        /**
         * @brief The slot is invoked when control returns to the event loop of the receiver's thread.
         *
         * The slot is executed in the receiver's thread.
         */
        QueuedConnection
    };

    /**
     * @brief A handle representing a signal-slot connection.
     */
    using ConnectionHandle = boost::signals2::connection;

    /**
     * @brief Type traits for inspecting member function pointers.
     * @tparam T The type to inspect.
     */
    template <typename T>
    struct MemberFunctionTraits {
        /** @brief Flag indicating if T is a member function pointer. */
        static constexpr bool is_member_function = false;
        /** @brief The class type of the member function pointer (void for non-member functions). */
        using class_type = void;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for standard member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template <typename C, typename R, typename... Args>
    struct MemberFunctionTraits<R (C::*)(Args...)> {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member_function = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for const member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template <typename C, typename R, typename... Args>
    struct MemberFunctionTraits<R (C::*)(Args...) const> {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member_function = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for volatile member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template <typename C, typename R, typename... Args>
    struct MemberFunctionTraits<R (C::*)(Args...) volatile> {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member_function = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for const volatile member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template <typename C, typename R, typename... Args>
    struct MemberFunctionTraits<R (C::*)(Args...) const volatile> {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member_function = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

#if __cplusplus >= 201703L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
    /**
     * @brief Specialization of MemberFunctionTraits for noexcept member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template <typename C, typename R, typename... Args>
    struct MemberFunctionTraits<R (C::*)(Args...) noexcept> {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member_function = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };

    /**
     * @brief Specialization of MemberFunctionTraits for const noexcept member function pointers.
     * @tparam C The class type.
     * @tparam R The return type.
     * @tparam Args The argument types.
     */
    template <typename C, typename R, typename... Args>
    struct MemberFunctionTraits<R (C::*)(Args...) const noexcept> {
        /** @brief Flag indicating T is a member function pointer. */
        static constexpr bool is_member_function = true;
        /** @brief The class type containing the member function. */
        using class_type = C;
        /** @brief The return type of the member function. */
        using return_type = R;
    };
#endif
}
