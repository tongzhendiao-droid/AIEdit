#include "core/MappingGuard.h"

#ifdef _WIN32

namespace aiedit::core {

void guardMapping(const void* /*start*/, std::size_t /*length*/) {}
void unguardMapping(const void* /*start*/) {}
std::uint64_t truncatedReadsCaught()
{
    return 0;
}

} // namespace aiedit::core

#else

#include <atomic>
#include <mutex>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace aiedit::core {

namespace {

// The registered mappings. The signal handler may only read them through lock-free atomics;
// the mutex orders the threads that change them.
struct Slot
{
    std::atomic<std::uintptr_t> start{0}; // 0: free
    std::atomic<std::size_t> length{0};
};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free && std::atomic<std::size_t>::is_always_lock_free);

constexpr std::size_t kMaxMappings = 1024; // more open files than that go unguarded
Slot g_slots[kMaxMappings];
std::mutex g_mutex;
std::atomic<std::uint64_t> g_caught{0};
std::uintptr_t g_pageSize = 4096;
struct sigaction g_previous{};

void passOn(int number, siginfo_t* info, void* context)
{
    if ((g_previous.sa_flags & SA_SIGINFO) && g_previous.sa_sigaction) {
        g_previous.sa_sigaction(number, info, context);
        return;
    }
    if (!(g_previous.sa_flags & SA_SIGINFO) && g_previous.sa_handler != SIG_DFL && g_previous.sa_handler != SIG_IGN) {
        g_previous.sa_handler(number);
        return;
    }
    // The default action (a fault cannot be ignored): once this returns, the read faults again
    // and ends the process as if this handler had never been installed.
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    ::sigaction(SIGBUS, &fallback, nullptr);
}

void onSigbus(int number, siginfo_t* info, void* context)
{
    const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    for (const Slot& slot : g_slots) {
        const std::uintptr_t start = slot.start.load(std::memory_order_acquire);
        if (start == 0 || address < start)
            continue;
        const std::size_t length = slot.length.load(std::memory_order_relaxed);
        if (address - start >= length)
            continue;
        // Zero-filled pages from the one that faulted to the end of the mapping, at the same
        // addresses; the read is then retried and succeeds.
        const std::uintptr_t page = address & ~(g_pageSize - 1);
        if (::mmap(reinterpret_cast<void*>(page), start + length - page, PROT_READ,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED) {
            g_caught.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        break;
    }
    passOn(number, info, context);
}

void installHandler()
{
    g_pageSize = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
    struct sigaction action{};
    action.sa_sigaction = onSigbus;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGBUS, &action, &g_previous);
}

} // namespace

void guardMapping(const void* start, std::size_t length)
{
    static std::once_flag installed;
    std::call_once(installed, installHandler);
    const std::lock_guard lock(g_mutex);
    for (Slot& slot : g_slots) {
        if (slot.start.load(std::memory_order_relaxed) == 0) {
            slot.length.store(length, std::memory_order_relaxed);
            slot.start.store(reinterpret_cast<std::uintptr_t>(start), std::memory_order_release);
            return;
        }
    }
}

void unguardMapping(const void* start)
{
    const std::lock_guard lock(g_mutex);
    for (Slot& slot : g_slots) {
        if (slot.start.load(std::memory_order_relaxed) == reinterpret_cast<std::uintptr_t>(start)) {
            slot.start.store(0, std::memory_order_release);
            slot.length.store(0, std::memory_order_relaxed);
            return;
        }
    }
}

std::uint64_t truncatedReadsCaught()
{
    return g_caught.load(std::memory_order_relaxed);
}

} // namespace aiedit::core

#endif
