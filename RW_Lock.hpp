#pragma once

#include "Saved_Async_Completion_Handler.hpp"
#include <asio.hpp>
#include <vector>
#include <queue>

// only for debug
#include <string>
#include <iomanip>
#include <iostream>



template <asio::execution::executor Executor = asio::any_io_executor>
class RW_Lock : asio::noncopyable
{

public: // Type definitions
    using executor_type = Executor;

    // RAII
    // Not to be constructed directly
    // Calling the interface functions for read locking should return an instance of this struct
    struct Read_Lock : asio::noncopyable
    {
        ~Read_Lock(){ if (m_rw_lock) m_rw_lock->end_read(); }
        Read_Lock(Read_Lock && other) : m_rw_lock(other.m_rw_lock) { other.m_rw_lock = nullptr; }

    private:
        friend class RW_Lock;

        Read_Lock(RW_Lock & rw_lock) : m_rw_lock(&rw_lock) { m_rw_lock->begin_read(); }

        RW_Lock * m_rw_lock;
    };

    // RAII
    // Not to be constructed directly
    // Calling the interface functions for write locking should return an instance of this struct
    struct Write_Lock : asio::noncopyable
    {
        Write_Lock(Write_Lock && other) : m_rw_lock(other.m_rw_lock) { other.m_rw_lock = nullptr; }
        ~Write_Lock(){ if (m_rw_lock) m_rw_lock->end_write(); }

    private:
        friend class RW_Lock;

        Write_Lock(RW_Lock & rw_lock) : m_rw_lock(&rw_lock) { m_rw_lock->begin_write(); }

        RW_Lock * m_rw_lock;
    };

    // Completion signatures for the interface async functions
    using Read_Signature = void(Read_Lock);
    using Write_Signature = void(Write_Lock);


public: // Constructor
    RW_Lock(executor_type executor, executor_type debug_executor)
        : m_lock_strand(executor)
        , m_debug_executor(debug_executor)
    { }


public: // Interface
    template <asio::completion_token_for<Read_Signature> CompletionToken>
    auto
    async_get_read_lock(CompletionToken && completion_token)
    {
        auto initiation = [&](asio::completion_handler_for<Read_Signature> auto && completion_handler)
        {
            auto f = [
                &,
                completion_handler = std::forward<decltype(completion_handler)>(completion_handler)
            ]() mutable
            {
                auto completion_executor = asio::get_associated_executor(completion_handler);
                static_assert(!std::same_as<decltype(completion_executor), asio::system_executor>, "system executor not allowed. Bind executor to your completion handler");            
            
                if (readers_allowed())
                {
                    asio::post(completion_executor,
                    [
                        completion_handler = std::forward<decltype(completion_handler)>(completion_handler),
                        rl = Read_Lock{*this}
                    ] () mutable
                    { completion_handler(std::move(rl)); });
                }
                else
                {
                    m_pending_readers.push( { completion_executor, std::forward<decltype(completion_handler)>(completion_handler) } );

                    print_debug_info("enqueue_reader");
                }
            };


            asio::dispatch(m_lock_strand, std::move(f));
        };


        return asio::async_initiate<CompletionToken, Read_Signature>
            (std::move(initiation), completion_token);
    }


    template <asio::completion_token_for<Write_Signature> CompletionToken>
    auto
    async_get_write_lock(CompletionToken && completion_token)
    {

        auto initiation = [&](asio::completion_handler_for<Write_Signature> auto && completion_handler)
        {

            auto f = [
                &,
                completion_handler = std::forward<decltype(completion_handler)>(completion_handler)
            ]() mutable
            {
                auto completion_executor = asio::get_associated_executor(completion_handler);
                static_assert(!std::same_as<decltype(completion_executor), asio::system_executor>, "system executor not allowed. Bind executor to your completion handler");
            
                if (writers_allowed())
                {
                    asio::post(completion_executor, 
                    [
                        completion_handler = std::forward<decltype(completion_handler)>(completion_handler),
                        wlock = Write_Lock{*this}
                    ] () mutable
                    { completion_handler(std::move(wlock)); });
                }
                else
                {
                    m_pending_writers.push( { completion_executor, std::forward<decltype(completion_handler)>(completion_handler) } );

                    print_debug_info("enqueue_writer");
                }
            };


            asio::dispatch(m_lock_strand, std::move(f));

        };


        return asio::async_initiate<CompletionToken, Write_Signature>
            (std::move(initiation), completion_token);
    }


public: // for debugging
    auto get_active_reader_count() const { return m_reader_count; }
    auto get_active_writer_count() const { return m_writer_count; }
    auto get_queued_reader_count() const { return m_pending_readers.size(); }
    auto get_queued_writer_count() const { return m_pending_writers.size(); }

    void print_debug_info(std::string tag)
    {
        // This function only gets called from the lock strand
        // but it dispatches the printing to a different context
        // with a dedicated thread.
        // 
        // This is done to so that printing does not affect (much)
        // the lock strand performance
        // 
        // It also means we must take copies of changing values
        // before dispatching to the printing thread


        // Get elapsed time since the first time this function is called
        static const auto start_time = std::chrono::steady_clock::now();
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(current_time - start_time);

        // Copy lock counters
        auto ar = get_active_reader_count();
        auto aw = get_active_writer_count();
        auto qr = get_queued_reader_count();
        auto qw = get_queued_writer_count();

        // NOTE: lock counters are copied again here, but this being a debug feature,
        // the nicer looks and readability are preferred over inserting all function calls
        // into the lambda capture or moving all copies there.
        // Also, the compiler should be able to take care of that optimization by itself.
        asio::dispatch(m_debug_executor, [=, tag = std::move(tag)](){
            std::cout << std::setw(16) << std::setprecision(10) << elapsed << "    "
                    << std::setw(16) << ("<" + tag + ">") << "\t"
                    << "active r/w: " << ar << " / " << aw << "\t"
                    << "queued r/w: " << qr << " / " << qw << std::endl;
        });
    }


private: // auxiliary functions
    // Determine if readers can run now or should be enqueued
    // Returns false if there are writers running or queued
    bool readers_allowed() const
    {
        return m_writer_count == 0 && m_pending_writers.empty();
    }

    // Determine if writers can run now or should be enqueued
    // Returns false if there are readers running
    bool writers_allowed() const
    {
        return m_reader_count == 0 && m_writer_count == 0;
    }


    // Dispatches pending writers and readers, in that order
    // Only dispatches readers/writers when allowed (functions above)
    void dispatch_pending()
    {
        while (writers_allowed() && !m_pending_writers.empty())
        {
            auto [ex, handler, guard] = std::move(m_pending_writers.front());
            m_pending_writers.pop();

            print_debug_info("dequeue_writer");

            asio::dispatch(ex, [h = std::move(handler), wl = Write_Lock{*this}]() mutable { h(std::move(wl)); });
            guard.reset();
        }

        while (readers_allowed() && !m_pending_readers.empty())
        {
            auto [ex, handler, guard] = std::move(m_pending_readers.front());
            m_pending_readers.pop();

            print_debug_info("dequeue_reader");

            asio::dispatch(ex, [h = std::move(handler), rl = Read_Lock{*this}]() mutable { h(std::move(rl)); });
            guard.reset();
        }
    }


    void begin_read()
    {
        // always called from the lock strand
        ++m_reader_count;
        
        print_debug_info("begin_read");
    }

    void begin_write()
    {
        // always called from the lock strand
        ++m_writer_count;

        print_debug_info("begin_write");
    }


    void end_read()
    {
        // this will be called without synchronization
        // dispatch to lock strand
        asio::dispatch(m_lock_strand, [&](){
            --m_reader_count;

            print_debug_info("end_read");

            dispatch_pending();
        });
    }

    void end_write()
    {
        // this will be called without synchronization
        // dispatch to lock strand
        asio::dispatch(m_lock_strand, [&](){
            --m_writer_count;

            print_debug_info("end_write");

            dispatch_pending();
        });
    }


private: // members
    asio::strand<executor_type> m_lock_strand;
    int m_reader_count{};
    int m_writer_count{};
    std::queue<saved_async_completion_handler<Read_Signature>> m_pending_readers;
    std::queue<saved_async_completion_handler<Write_Signature>> m_pending_writers;
    
    // debug
    executor_type m_debug_executor;
};
