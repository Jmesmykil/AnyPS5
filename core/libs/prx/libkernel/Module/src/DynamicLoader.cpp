#include "prx/libc/include/General.hpp"
#include <nid/NidCompute.hpp>
#include <array>
#include <filesystem>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

namespace {
thread_local std::array<char, 512> loaderError{};
thread_local bool pendingError = false;
void Error(const char* message) {
    std::snprintf(loaderError.data(), loaderError.size(), "%s", message);
    pendingError = true;
}
struct Module {
    void* native = nullptr;
    bool owned = true;
    bool global = false;
    ~Module() {
        if (owned && native) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(native));
#else
            ::dlclose(native);
#endif
        }
    }
};
std::mutex modulesMutex;
std::map<std::uintptr_t, std::shared_ptr<Module>> modules;
std::uintptr_t nextHandle = 0x20000000;
std::map<const void*, std::uintptr_t> imageIds;
void* Symbol(Module& module, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module.native), name));
#else
    return ::dlsym(module.native, name);
#endif
}
void* FindSymbol(Module& module, const char* name) {
    if (auto* symbol = Symbol(module, name)) return symbol;
    const auto nid = Nid::ComputeNid(name, "");
#ifdef _WIN32
    return Symbol(module, nid.c_str());
#else
    if (auto* symbol = Symbol(module, nid.c_str())) return symbol;
    constexpr char guestSuffix[] = "#guest";
    if (auto* symbol = Symbol(module, (std::string(name) + guestSuffix).c_str())) return symbol;
    return Symbol(module, (nid + guestSuffix).c_str());
#endif
}
#ifndef _WIN32
struct ModuleArgs { std::size_t size = 0; const void* data = nullptr; };
struct StartFrame;
thread_local StartFrame* currentStart = nullptr;
struct StartFrame {
    std::filesystem::path target;
    ModuleArgs args;
    StartFrame* previous;
    bool observed = false;
    StartFrame(std::filesystem::path path, std::size_t size, const void* data)
        : target(std::filesystem::weakly_canonical(std::filesystem::absolute(path))), args{size, data}, previous(currentStart) {
        for (auto* parent = previous; parent; parent = parent->previous)
            if (parent->target == target) throw std::runtime_error("sceKernelLoadStartModule: recursive module initialization is unsupported");
        currentStart = this;
    }
    ~StartFrame() { currentStart = previous; }
};
struct LifecycleRecord {
    std::uint64_t magic;
    std::uint32_t size, version, flags;
    std::int32_t result;
    std::uint32_t returned, reserved;
};
static_assert(sizeof(LifecycleRecord) == 32);
struct RecordRange { std::uintptr_t base, address; bool readable = false; };
int CheckRecordRange(dl_phdr_info* image, std::size_t, void* opaque) {
    auto& range = *static_cast<RecordRange*>(opaque);
    if (image->dlpi_addr != range.base) return 0;
    for (std::size_t index = 0; index < image->dlpi_phnum; ++index) {
        const auto& header = image->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_R)) continue;
        if (header.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - image->dlpi_addr) continue;
        const auto start = image->dlpi_addr + header.p_vaddr;
        if (range.address >= start && range.address - start <= header.p_memsz && sizeof(LifecycleRecord) <= header.p_memsz - (range.address - start)) range.readable = true;
    }
    return 1;
}
int ModuleStartResult(Module& module, const StartFrame& frame) {
    link_map* image = nullptr;
    if (::dlinfo(module.native, RTLD_DI_LINKMAP, &image) != 0 || !image) throw std::runtime_error("sceKernelLoadStartModule: image identity unavailable");
    const auto* record = static_cast<const LifecycleRecord*>(::dlsym(module.native, "__aps5_guest_lifecycle_nid_no_patch"));
    Dl_info owner{};
    void* ownerMap = nullptr;
    if (!record || !::dladdr1(record, &owner, &ownerMap, RTLD_DL_LINKMAP) || ownerMap != image)
        throw std::runtime_error("sceKernelLoadStartModule: exact module lifecycle record unavailable");
    RecordRange range{static_cast<std::uintptr_t>(image->l_addr), reinterpret_cast<std::uintptr_t>(record)};
    ::dl_iterate_phdr(CheckRecordRange, &range);
    if (!range.readable || record->magic != 0x3159434c35535041ull || record->size != sizeof(LifecycleRecord) || record->version != 1 || (record->flags & ~3u) || record->reserved != 0)
        throw std::runtime_error("sceKernelLoadStartModule: invalid module lifecycle record");
    if (frame.previous && (record->flags & 3u) && !frame.observed)
        throw std::runtime_error("sceKernelLoadStartModule: nested module initialization completion is unproven");
    if (record->flags & 1u) {
        if (record->returned != 1) throw std::runtime_error("sceKernelLoadStartModule: module entry has not returned");
        return record->result;
    }
    if (record->returned != 0 || record->result != 0) throw std::runtime_error("sceKernelLoadStartModule: invalid no-entry lifecycle record");
    return 0;
}
#endif
}

extern "C" {
char* APS5_VABI dlerror_nid_postfix() {
    if (!pendingError) return nullptr;
    pendingError = false;
    return loaderError.data();
}
static std::filesystem::path RelinkedModulePath(const std::filesystem::path& path) {
    auto relinked = path;
    relinked += ".guest.prx";
    std::error_code error;
    return std::filesystem::is_regular_file(relinked, error) ? relinked : path;
}

#ifndef _WIN32
const void* __aps5_get_pending_module_args_nid_no_patch(const void* self) {
    static const ModuleArgs empty{};
    if (!currentStart) return &empty;
    Dl_info owner{};
    if (!::dladdr(self, &owner) || !owner.dli_fname) throw std::runtime_error("sceKernelLoadStartModule: initializer identity unavailable");
    const auto actual = std::filesystem::weakly_canonical(std::filesystem::absolute(owner.dli_fname));
    if (actual != currentStart->target) return &empty;
    currentStart->observed = true;
    return &currentStart->args;
}
#endif
static void* OpenModule(const char* path, int flags, bool start, std::size_t args, const void* argp, int* result) {
    if ((flags & ~0x103) || (flags & 3) == 0 || (flags & 3) == 3) {
        Error("dlopen: unsupported flags"); return nullptr;
    }
    try {
        auto module = std::make_shared<Module>();
        module->global = (flags & 0x100) != 0 || !path;
#ifdef _WIN32
        if (!path) {
            module->native = GetModuleHandleW(nullptr);
            module->owned = false;
        } else {
            if (!*path) { Error("dlopen: empty module path"); return nullptr; }
            const auto resolved = RelinkedModulePath(ResolvePath_nid_no_patch(path));
            module->native = LoadLibraryExW(resolved.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!module->native) {
            char message[128];
            std::snprintf(message, sizeof(message), "dlopen: Windows loader error %lu (module must be host-compatible)", GetLastError());
            Error(message); return nullptr;
        }
#else
        const auto resolved = path ? RelinkedModulePath(ResolvePath_nid_no_patch(path)).string() : std::string{};
        const int nativeFlags = ((flags & 3) == 1 ? RTLD_LAZY : RTLD_NOW) |
            ((flags & 0x100) ? RTLD_GLOBAL : RTLD_LOCAL);
        if (start) {
            if (!path || !*path) { Error("sceKernelLoadStartModule: empty module path"); return nullptr; }
            StartFrame frame(resolved, args, argp);
            module->native = ::dlopen(resolved.c_str(), nativeFlags);
            if (!module->native) { Error(::dlerror()); return nullptr; }
            const int started = ModuleStartResult(*module, frame);
            if (result) *result = started;
        } else {
            module->native = ::dlopen(path ? resolved.c_str() : nullptr, nativeFlags);
            if (!module->native) { Error(::dlerror()); return nullptr; }
        }
#endif
        std::lock_guard lock(modulesMutex);
        const auto handle = nextHandle++;
        modules.emplace(handle, std::move(module));
        return reinterpret_cast<void*>(handle);
    } catch (const std::exception& error) { Error(error.what()); return nullptr; }
}
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags) {
    return OpenModule(path, flags, false, 0, nullptr, nullptr);
}
void* GuestLoadStartModule_nid_no_patch(const char* path, int flags, std::size_t args, const void* argp, int* result) {
#ifdef _WIN32
    return OpenModule(path, flags, false, args, argp, result);
#else
    return OpenModule(path, flags, true, args, argp, result);
#endif
}
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name) {
    if (!name || !*name) { Error("dlsym: empty symbol name"); return nullptr; }
    try {
        std::vector<std::shared_ptr<Module>> search;
        {
            std::lock_guard lock(modulesMutex);
            if (handle == reinterpret_cast<void*>(static_cast<std::intptr_t>(-2))) {
                for (const auto& [key, module] : modules) if (module->global) search.push_back(module);
            } else {
                auto found = modules.find(reinterpret_cast<std::uintptr_t>(handle));
                if (found == modules.end()) { Error("dlsym: invalid or unsupported module handle"); return nullptr; }
                search.push_back(found->second);
            }
        }
        for (const auto& module : search) if (auto* result = FindSymbol(*module, name)) return result;
        Error("dlsym: symbol not found in supported module scope");
        return nullptr;
    } catch (const std::exception& error) { Error(error.what()); return nullptr; }
}
int APS5_VABI dlclose_nid_postfix(void* handle) {
    std::shared_ptr<Module> module;
    {
        std::lock_guard lock(modulesMutex);
        auto found = modules.find(reinterpret_cast<std::uintptr_t>(handle));
        if (found == modules.end()) { Error("dlclose: invalid module handle"); return -1; }
        module = std::move(found->second);
        modules.erase(found);
    }
    // Unload outside the registry lock: module destructors may call loader APIs.
    module.reset();
    return 0;
}
std::int32_t ModuleIdForImage_nid_no_patch(const void* native) {
    std::lock_guard lock(modulesMutex);
    for (const auto& [handle, module] : modules) {
        if (module->native == native) return static_cast<std::int32_t>(handle);
    }
    const auto [found, inserted] = imageIds.emplace(native, nextHandle);
    if (inserted) ++nextHandle;
    return static_cast<std::int32_t>(found->second);
}
}
