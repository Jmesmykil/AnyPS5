// Standalone harness: invokes the actual NID-patched kernel, not native wrappers.
#include <dlfcn.h>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/mman.h>

namespace {
using Load = int (*)(const char*, std::size_t, const void*, unsigned, const void*, int*);
using Stop = int (*)(int, std::size_t, const void*, unsigned, const void*, int*);
using Lookup = int (*)(int, const char*, void**);
struct Args { std::size_t size; const void* pointer; };
using Getter = const Args* (*)(const void*);
Load load;
Stop stop;
Lookup lookup;
Getter getter;
char* (*loaderError)();
struct State { std::uint64_t size, pointer, third, calls, arrays, identity, unused[2]; };
std::atomic<int> callbackFailures{0};
std::atomic<int> threadCallbacks{0};
std::atomic<int> callbacks{0};
std::atomic<int> nestedCases{0};
int sentinel1, sentinel2;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Path(const char* name) { return std::string("app0/plugins/") + name + ".prx"; }
int Start(const char* name, std::size_t size, const void* pointer, int& result) {
    return load(Path(name).c_str(), size, pointer, 0, nullptr, &result);
}
void Unload(int handle) { Check(stop(handle, 0, nullptr, 0, nullptr, nullptr) == 0, "unload failed"); }
State* Inspect(int handle) {
    void* state = nullptr;
    Check(lookup(handle, "fixture_state", &state) == 0 && state, "patched dlsym could not resolve guest state");
    return static_cast<State*>(state);
}
void StateIs(int handle, std::size_t size, const void* pointer, unsigned count = 1) {
    auto* state = Inspect(handle);
    Check(state->size == size && state->pointer == reinterpret_cast<std::uintptr_t>(pointer), "entry args not exact");
    Check(state->third == 0 && state->calls == count, "entry third arg or count wrong");
}
void MustReject(const char* name, const char* message) {
    int result = 0x7654321;
    Check(Start(name, 123, &sentinel1, result) < 0, "unsupported lifecycle accepted");
    Check(result == 0x7654321, "failed load fabricated entry result");
    const char* error = loaderError();
    Check(error && std::strstr(error, message), "failure not named");
}
}

extern "C" void fixture_callback(State* state, int phase) {
    ++callbacks;
    // Failure is captured here instead of unwinding through arbitrary guest code.
    try {
        const auto* foreign = getter(reinterpret_cast<const void*>(&fixture_callback));
        Check(foreign->size == 0 && foreign->pointer == nullptr, "foreign self borrowed pending arguments");
        if (state->identity == 5 && phase == 1) {
            const Args before = *getter(state);
            Check(before.size == 55 && before.pointer == &sentinel1, "outer context missing");
            int result = 0;
            const int inner = Start("inner", 77, &sentinel2, result);
            Check(inner > 0 && result == -19, "nested result wrong");
            StateIs(inner, 77, &sentinel2);
            Unload(inner);
            const Args after = *getter(state);
            Check(after.size == before.size && after.pointer == before.pointer, "nested context did not restore");
            // This target has arrays and no getter observation: nested reuse is unproven.
            MustReject("noentryarray", "nested module initialization");
            ++nestedCases;
        } else if (state->identity == 7 || (state->identity == 8 && phase == 2)) {
            MustReject(state->identity == 7 ? "recursive" : "arrayrecursive", "recursive module initialization");
            ++nestedCases;
        } else if (state->identity == 9) {
            Check(state->size == 0 && state->pointer == 0, "dependency received target arguments");
            MustReject("dep", "nested module initialization");
            ++nestedCases;
        } else if (state->identity == 10 && phase == 1) {
            const auto* own = getter(state);
            Check(own->size == 66 && own->pointer == &sentinel2, "outer dependency load context wrong");
        } else if (state->identity == 17 && phase == 2) {
            const Args before = *getter(state);
            int result = 0;
            const int inner = Start("inner", 77, &sentinel2, result);
            Check(inner > 0 && result == -19, "nested-from-array result wrong");
            StateIs(inner, 77, &sentinel2); Unload(inner);
            const Args after = *getter(state);
            Check(before.size == after.size && before.pointer == after.pointer, "array nested context did not restore");
            ++nestedCases;
        } else if (state->identity == 11 || state->identity == 12) {
            const auto* expected = state->identity == 11 ? &sentinel1 : &sentinel2;
            const auto size = state->identity == 11 ? 111u : 222u;
            Check(state->size == size && state->pointer == reinterpret_cast<std::uintptr_t>(expected), "concurrent arguments crossed");
            const auto* own = getter(state);
            Check(own->size == size && own->pointer == expected, "concurrent frame wrong");
            ++threadCallbacks;
        }
    } catch (const std::exception& error) {
        ++callbackFailures;
        std::fprintf(stderr, "callback control failed: %s\n", error.what());
    }
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected actual patched kernel path");
        auto* kernel = ::dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
        if (!kernel) throw std::runtime_error(::dlerror());
        load = reinterpret_cast<Load>(::dlsym(kernel, "wzvqT4UqKX8"));
        stop = reinterpret_cast<Stop>(::dlsym(kernel, "QKd0qM58Qes"));
        lookup = reinterpret_cast<Lookup>(::dlsym(kernel, "LwG8g3niqwA"));
        loaderError = reinterpret_cast<char* (*)()>(::dlsym(kernel, "ucFJiTO1EUw"));
        getter = reinterpret_cast<Getter>(::dlsym(kernel, "__aps5_get_pending_module_args_nid_no_patch"));
        Check(load && stop && lookup && loaderError && getter, "actual patched exports missing");
        int result = 0;
        int first = Start("plain", 41, &sentinel1, result);
        if (first <= 0 || result != 17) std::fprintf(stderr, "first load handle=%d result=%d error=%s\n", first, result, loaderError());
        Check(first > 0 && result == 17, "actual entry return lost");
        StateIs(first, 41, &sentinel1);
        int second = Start("plain", 99, &sentinel2, result);
        Check(second > 0 && result == 17, "repeat first-result replay wrong");
        StateIs(second, 41, &sentinel1);
        Unload(first); StateIs(second, 41, &sentinel1); Unload(second);
        first = Start("plain", 99, &sentinel2, result);
        Check(first > 0 && result == 17, "reload result wrong");
        StateIs(first, 99, &sentinel2); Unload(first);
        first = Start("../plugins/plain", 42, &sentinel1, result);
        Check(first > 0 && result == 17, "canonical path alias did not receive args");
        StateIs(first, 42, &sentinel1); Unload(first);
        std::puts("actual patched NID calls: opaque identity, first-result repeat, refcount, reload PASS");

        void* opaque = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(opaque != MAP_FAILED, "opaque guard map failed");
        first = Start("negative", 4096, opaque, result);
        Check(first > 0 && result == -37, "signed negative entry result lost");
        StateIs(first, 4096, opaque); Unload(first); ::munmap(opaque, 4096);

        for (auto name : {"noentry", "array", "noentryarray"}) {
            first = Start(name, 19, &sentinel2, result);
            Check(first > 0 && result == (std::strcmp(name, "array") == 0 ? 17 : 0), "no-entry metadata / array result wrong");
            auto* state = Inspect(first);
            Check(state->calls == (std::strcmp(name, "array") == 0 ? 1u : 0u), "no-entry executed entry");
            Check(state->arrays == (std::strcmp(name, "noentry") == 0 ? 0u : 1u), "init array not run exactly once");
            Unload(first);
        }
        std::puts("negative result, inaccessible opaque pointer, no-entry, init-array PASS");

        // A normal loader startup has no LoadStart frame; entry receives zeros.
        auto* native = ::dlopen("./app0/plugins/plain.prx.guest.prx", RTLD_NOW | RTLD_LOCAL);
        Check(native != nullptr, "static startup probe failed");
        first = Start("plain", 88, &sentinel1, result);
        Check(first > 0 && result == 17, "startup-loaded first result missing");
        StateIs(first, 0, nullptr); Unload(first); ::dlclose(native);

        for (const auto& scenario : std::array<std::pair<const char*, unsigned>, 5>{{{"nested",55},{"recursive",1},{"arrayrecursive",1},{"outerdep",66},{"arraynested",55}}}) {
            first = Start(scenario.first, scenario.second, scenario.second == 66 ? &sentinel2 : &sentinel1, result);
            Check(first > 0 && result == 17, "outer lifecycle failed");
            Unload(first);
        }
        Check(nestedCases == 6 && callbackFailures == 0, "nested / dependency / array guards failed");
        std::puts("nested restore, ancestor and dependency init/array recursion, exact self ownership PASS");

        auto threaded = [](const char* name, unsigned size, int* pointer) {
            try {
                int observed = 0;
                const int handle = Start(name, size, pointer, observed);
                Check(handle > 0 && observed == 17, "concurrent load failed");
                StateIs(handle, size, pointer); Unload(handle);
            } catch (const std::exception& error) { ++callbackFailures; std::fprintf(stderr, "%s\n", error.what()); }
        };
        std::thread one(threaded, "thread1", 111, &sentinel1), two(threaded, "thread2", 222, &sentinel2);
        one.join(); two.join();
        Check(threadCallbacks == 2 && callbackFailures == 0, "concurrent isolation failed");
        std::barrier held(2);
        std::array<int, 2> sharedHandles{};
        auto sharedLoad = [&](unsigned index) {
            int observed = 0;
            held.arrive_and_wait();
            sharedHandles[index] = Start("same", index + 1, index == 0 ? &sentinel1 : &sentinel2, observed);
            if (sharedHandles[index] <= 0 || observed != 17) ++callbackFailures;
            held.arrive_and_wait(); // Both references stay live while the state is inspected.
        };
        std::thread three(sharedLoad, 0), four(sharedLoad, 1);
        three.join(); four.join();
        Check(callbackFailures == 0, "concurrent same-image load failed");
        auto* shared = Inspect(sharedHandles[0]);
        Check(shared == Inspect(sharedHandles[1]) && shared->calls == 1 && shared->third == 0, "concurrent same-image entry was rerun");
        Check((shared->size == 1 && shared->pointer == reinterpret_cast<std::uintptr_t>(&sentinel1)) ||
              (shared->size == 2 && shared->pointer == reinterpret_cast<std::uintptr_t>(&sentinel2)), "same-image first actual args not preserved");
        Unload(sharedHandles[0]); Unload(sharedHandles[1]);

        MustReject("badrecord", "invalid module lifecycle record");
        MustReject("badextent", "invalid module lifecycle record");
        MustReject("unreadablerecord", "invalid module lifecycle record");
        MustReject("missingrecord", "exact module lifecycle record");
        MustReject("wrongowner", "exact module lifecycle record");
        MustReject("nonexistent", "cannot open");
        Check(getter(reinterpret_cast<const void*>(&fixture_callback))->size == 0, "failed load leaked frame");
        first = Start("plain", 23, &sentinel1, result);
        Check(first > 0 && result == 17, "load after failure broken"); StateIs(first, 23, &sentinel1); Unload(first);
        Check(callbackFailures == 0, "callback checks failed");
        std::puts("concurrency, missing/schema/ownership rejection, failure cleanup PASS");
        ::dlclose(kernel);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "module lifecycle: %s\n", error.what()); return 1;
    }
}
