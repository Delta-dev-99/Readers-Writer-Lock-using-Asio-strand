#include <thread>
#include <vector>
#include <queue>
#include <asio.hpp>

// only for debug
#include <iostream>
#include <iomanip>
#include <string>



// The io_context used for printing
asio::io_context cout_io_ctx;





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
        Write_Lock(RW_Lock & rw_lock) : m_rw_lock(&rw_lock) { m_rw_lock->begin_write(); }

        RW_Lock * m_rw_lock;
    };

    // Completion signatures for the interface async functions
    using Read_Signature = void(Read_Lock);
    using Write_Signature = void(Write_Lock);


public: // Constructor
    RW_Lock(executor_type executor)
        : m_lock_strand(executor)
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
        asio::dispatch(cout_io_ctx, [=, tag = std::move(tag)](){
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
};





// a fake reader that just waits
asio::awaitable<void> reader(asio::any_io_executor executor, RW_Lock<> & lock)
{
    auto rl = co_await lock.async_get_read_lock(asio::use_awaitable);

    // simulate busy reader that blocks the thread
    // std::this_thread::sleep_for(std::chrono::milliseconds{200});

    // simulate lightweight reader that doesn't hog the CPU
    asio::steady_timer t{executor};
    t.expires_after(std::chrono::milliseconds{200});
    co_await t.async_wait(asio::use_awaitable);
}

// a fake writer that just waits
asio::awaitable<void> writer(asio::any_io_executor executor, RW_Lock<> & lock)
{
    auto wl = co_await lock.async_get_write_lock(asio::use_awaitable);

    // simulate busy writer that blocks the thread
    // std::this_thread::sleep_for(std::chrono::milliseconds{750});

    // simulate lightweight writer that doesn't hog the CPU
    asio::steady_timer t{executor};
    t.expires_after(std::chrono::milliseconds{750});
    co_await t.async_wait(asio::use_awaitable);
}



// a coroutine that posts new readers periodically
asio::awaitable<void> post_readers(asio::any_io_executor executor, RW_Lock<> & lock)
{
    asio::steady_timer t{executor};

    while (true)
    {
        t.expires_after(std::chrono::milliseconds{50});
        co_await t.async_wait(asio::use_awaitable);
        asio::co_spawn(executor, reader(executor, lock), asio::detached);
    }
}

// a coroutine that posts new writers periodically
asio::awaitable<void> post_writers(asio::any_io_executor executor, RW_Lock<> & lock)
{
    asio::steady_timer t{executor};

    while (true)
    {
        t.cancel();
        t.expires_after(std::chrono::milliseconds{1500});
        co_await t.async_wait(asio::use_awaitable);
        asio::co_spawn(executor, writer(executor, lock), asio::detached);
    }
}



int main()
{
    asio::io_context io_ctx;

    RW_Lock<> lock{io_ctx.get_executor()};

    // spawn coroutines that periodically spawn readers and writers
    asio::co_spawn(io_ctx, post_readers(io_ctx.get_executor(), lock), asio::detached);
    asio::co_spawn(io_ctx, post_writers(io_ctx.get_executor(), lock), asio::detached);

    // prevent the printing thread from terminating when there is nothing to print
    auto cout_work_guard = asio::make_work_guard(cout_io_ctx);
    // start the printing thread
    std::jthread cout_thread{ [&](){ cout_io_ctx.run(); } };

    // run the main io_context in multiple threads
    std::vector<std::jthread> threads;
    threads.resize(3);
    for (auto & x : threads)
        x = std::jthread{ [&io_ctx](){ io_ctx.run(); } };
    io_ctx.run();

    cout_work_guard.reset();
}
