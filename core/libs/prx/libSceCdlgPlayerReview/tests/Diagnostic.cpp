#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Call>
static void NamedFailure(Call call, const char* api, const char* nid) {
    // Patched libc may terminate an unhandled host exception. Isolate every
    // actual call, capture its diagnostic, and reject normal return or SIGSEGV.
    int stream[2];
    Require(pipe(stream) == 0, "diagnostic pipe failed");
    const auto child = fork();
    Require(child >= 0, "diagnostic fork failed");
    if (child == 0) {
        const rlimit noCore{0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        close(stream[0]);
        dup2(stream[1], STDERR_FILENO);
        close(stream[1]);
        call();
        _exit(90);
    }
    close(stream[1]);
    std::string message;
    std::array<char, 1024> buffer;
    ssize_t count;
    while ((count = read(stream[0], buffer.data(), buffer.size())) > 0)
        message.append(buffer.data(), static_cast<std::size_t>(count));
    close(stream[0]);
    int status = 0;
    Require(waitpid(child, &status, 0) == child, "diagnostic wait failed");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
            "entry returned or failed without an unsupported exception (SIGSEGV rejected)");
    Require(message.find(api) != std::string::npos, "missing API diagnostic");
    Require(message.find(nid) != std::string::npos, "missing NID diagnostic");
    Require(message.find("DIAGNOSTIC ONLY") != std::string::npos, "missing scope diagnostic");
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::printf("named failure: %s [%s]; child status=%d\n", api, nid, status);
}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected actual patched diagnostic provider path");
        void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!module) throw std::runtime_error(dlerror());
        // Loading and resolving the unused provider must not call any entry.
        using Control = std::int32_t (APS5_VABI *)();
        using Open = std::int32_t (APS5_VABI *)(const void*);
        using Result = std::int32_t (APS5_VABI *)(void*);
        auto initialize = reinterpret_cast<Control>(dlsym(module, "UtXl-tmi7iw"));
        auto open = reinterpret_cast<Open>(dlsym(module, "CtygVRCL+bA"));
        auto update = reinterpret_cast<Control>(dlsym(module, "4J5F23VgTjY"));
        auto result = reinterpret_cast<Result>(dlsym(module, "VuKbx6zlEG4"));
        auto terminate = reinterpret_cast<Control>(dlsym(module, "RmJKkzZFNFA"));
        Require(initialize && open && update && result && terminate, "missing actual NID export");
        Require(dlsym(module, "not-a-provider-NID") == nullptr, "unexpected unrelated export");
        std::puts("unused provider: loaded; five NIDs resolved; no entry invoked");

        auto* shared = static_cast<std::uint8_t*>(mmap(nullptr, 240, PROT_READ | PROT_WRITE,
                                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        Require(shared != MAP_FAILED, "shared argument mapping failed");
        std::memset(shared, 0xA5, 168); std::memset(shared + 168, 0x5A, 72);
        std::array<std::uint8_t, 240> before;
        std::memcpy(before.data(), shared, before.size());
        const auto checkFailure = [&](auto call, const char* api, const char* nid) {
            NamedFailure(call, api, nid);
            Require(std::memcmp(shared, before.data(), before.size()) == 0, "opaque arguments changed");
        };
        checkFailure([&] { initialize(); }, "scePlayerReviewDialogInitialize", "UtXl-tmi7iw");
        checkFailure([&] { open(shared); }, "scePlayerReviewDialogOpen", "CtygVRCL+bA");
        checkFailure([&] { update(); }, "scePlayerReviewDialogUpdateStatus", "4J5F23VgTjY");
        checkFailure([&] { result(shared + 168); }, "scePlayerReviewDialogGetResult", "VuKbx6zlEG4");
        checkFailure([&] { terminate(); }, "scePlayerReviewDialogTerminate", "RmJKkzZFNFA");
        // Even inaccessible opaque addresses are never dereferenced by this provider.
        checkFailure([&] { open(reinterpret_cast<const void*>(1)); }, "scePlayerReviewDialogOpen", "CtygVRCL+bA");
        checkFailure([&] { result(reinterpret_cast<void*>(1)); }, "scePlayerReviewDialogGetResult", "VuKbx6zlEG4");
        Require(munmap(shared, before.size()) == 0, "shared argument unmap failed");
        Require(dlclose(module) == 0, "provider close failed");
        std::puts("seven actual NID calls failed safely; opaque arguments unchanged");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "diagnostic test failure: %s\n", error.what());
        return 1;
    }
}
