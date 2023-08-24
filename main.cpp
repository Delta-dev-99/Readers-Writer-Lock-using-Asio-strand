#include "RW_Lock.hpp"
#include <iostream>
#include <thread>



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
    asio::io_context debug_io_ctx; // used for printing debug info

    RW_Lock<> lock{io_ctx.get_executor(), debug_io_ctx.get_executor()};

    // spawn coroutines that periodically spawn readers and writers
    asio::co_spawn(io_ctx, post_readers(io_ctx.get_executor(), lock), asio::detached);
    asio::co_spawn(io_ctx, post_writers(io_ctx.get_executor(), lock), asio::detached);

    // prevent the printing thread from terminating when there is nothing to print
    auto debug_ctx_work_guard = asio::make_work_guard(debug_io_ctx);
    // start the printing thread
    std::jthread debug_thread{ [&](){ debug_io_ctx.run(); } };

    // run the main io_context in multiple threads
    std::vector<std::jthread> threads;
    threads.resize(3);
    for (auto & x : threads)
        x = std::jthread{ [&io_ctx](){ io_ctx.run(); } };
    io_ctx.run();

    debug_ctx_work_guard.reset();
}
