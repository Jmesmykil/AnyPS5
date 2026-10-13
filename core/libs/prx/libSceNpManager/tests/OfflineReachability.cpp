#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <thread>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static std::atomic<int> callbacks{0};
static void APS5_VABI CallbackA(int, unsigned, void*) { ++callbacks; }
static void APS5_VABI CallbackB(int, unsigned, void*) { ++callbacks; }

template<class Function>
static void ConcurrentOneOwner(Function function, std::int32_t losingResult) {
    std::array<std::int32_t, 4> results{};
    std::array<std::thread, 4> threads;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            ++ready;
            while (!go.load()) std::this_thread::yield();
            results[i] = function(i);
        });
    }
    while (ready.load() != 4) std::this_thread::yield();
    go = true;
    for (auto& thread : threads) thread.join();
    int winners = 0;
    for (const auto result : results) {
        if (result == 0) ++winners;
        else Require(result == losingResult, "unexpected concurrent failure result");
    }
    Require(winners == 1, "concurrent slot operation did not have exactly one owner");
}

int main(int argc, char** argv) {
    try {
        Require(argc == 2 || argc == 3, "expected actual patched provider and optional --expect-missing");
        const bool missing = argc == 3 && std::strcmp(argv[2], "--expect-missing") == 0;
        Require(argc == 2 || missing, "unknown test option");
        void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!module) throw std::runtime_error(dlerror());
        using Register = std::int32_t (APS5_VABI *)(void*, void*);
        using Control = std::int32_t (APS5_VABI *)();
        auto registerCallback = reinterpret_cast<Register>(dlsym(module, "hw5KNqAAels"));
        auto unregisterCallback = reinterpret_cast<Control>(dlsym(module, "cRILAEvn+9M"));
        auto checkCallback = reinterpret_cast<Control>(dlsym(module, "3Zl8BePTh9Y"));
        Require(registerCallback && checkCallback, "existing actual NID exports missing");
        Require(callbacks == 0, "unused provider invoked a callback");
        if (missing) {
            Require(!unregisterCallback, "frozen provider unexpectedly exports unregister");
            Require(dlclose(module) == 0, "frozen provider close failed");
            std::puts("frozen missing-NID negative control passed; no entry called");
            return 0;
        }
        Require(unregisterCallback != nullptr, "actual unregister NID missing");
        constexpr auto invalid = static_cast<std::int32_t>(0x80550003u);
        constexpr auto already = static_cast<std::int32_t>(0x80550008u);
        constexpr auto absent = static_cast<std::int32_t>(0x80550009u);
        auto callbackA = reinterpret_cast<void*>(&CallbackA);
        auto callbackB = reinterpret_cast<void*>(&CallbackB);
        std::array<unsigned char, 32> userdata;
        userdata.fill(0xa5);
        const auto canary = userdata;
        Require(unregisterCallback() == absent, "unused provider already owns a slot");
        Require(registerCallback(nullptr, reinterpret_cast<void*>(1)) == invalid, "null callback accepted");
        Require(unregisterCallback() == absent, "null registration changed empty state");
        Require(registerCallback(callbackA, userdata.data()) == 0, "first registration failed");
        Require(registerCallback(nullptr, nullptr) == invalid, "null callback priority changed");
        Require(registerCallback(callbackB, reinterpret_cast<void*>(1)) == already, "duplicate callback accepted");
        Require(checkCallback() == 0 && callbacks == 0, "offline check manufactured an event");
        Require(userdata == canary, "opaque userdata was changed");
        Require(unregisterCallback() == 0, "registered slot not cleared");
        Require(unregisterCallback() == absent, "second unregister accepted");
        Require(registerCallback(callbackB, reinterpret_cast<void*>(1)) == 0, "opaque userdata was accessed");
        Require(checkCallback() == 0 && callbacks == 0, "offline re-registration produced an event");
        Require(unregisterCallback() == 0, "re-registered slot not cleared");
        for (int round = 0; round < 16; ++round) {
            ConcurrentOneOwner([&](std::size_t i) {
                return registerCallback(i % 2 ? callbackA : callbackB, reinterpret_cast<void*>(i + 1));
            }, already);
            Require(checkCallback() == 0 && callbacks == 0, "concurrent registration produced an event");
            ConcurrentOneOwner([&](std::size_t) { return unregisterCallback(); }, absent);
        }
        Require(unregisterCallback() == absent, "concurrency left a registered slot");
        Require(userdata == canary && callbacks == 0, "callback or userdata side effect observed");
        Require(dlclose(module) == 0, "provider close failed");
        std::puts("actual NID lifecycle passed: null/duplicate/clear/re-register; 16 four-thread ownership rounds; no events");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
