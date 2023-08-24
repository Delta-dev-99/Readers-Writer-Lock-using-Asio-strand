#pragma once

#include <asio.hpp>



// Holds a completion handler, it's associated executor and a work_guard
// Provides constructors that automatically create the work guard
// The executor is obtained by calling `asio::get_associated_executor`
template <class Signature,
          class Executor = asio::any_completion_executor,
          class CompletionHandler = asio::any_completion_handler<Signature>>
struct saved_async_completion_handler
{
    using signature = Signature;
    using executor_type = Executor;
    using work_guard_type = asio::executor_work_guard<executor_type>;
    using completion_handler_type = CompletionHandler;


    saved_async_completion_handler(executor_type ex, completion_handler_type && handler)
        : m_ex(asio::get_associated_executor(handler, ex))
        , m_handler(std::forward<completion_handler_type>(handler))
        , m_work_guard(asio::make_work_guard(m_ex))
    {}

    template <class Handler>
    saved_async_completion_handler(Handler && handler)
        : m_ex(asio::get_associated_executor(handler))
        , m_handler(std::forward<Handler>(handler))
        , m_work_guard(asio::make_work_guard(m_ex))
    {}

    executor_type m_ex;
    completion_handler_type m_handler;
    work_guard_type m_work_guard;
};
