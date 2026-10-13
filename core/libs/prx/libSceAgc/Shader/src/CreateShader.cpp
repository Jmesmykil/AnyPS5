#include "prx/libSceAgc/Shader/include/CreateShader.hpp"

#include <cstdio>
#include <cstring>
#include <cstddef>
#include <limits>
#include <vector>
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <stdexcept>
#include <prx/libc/include/General.hpp>

#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgc/Shader/include/ShaderUtils.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"

#ifndef APS5_AGC_CREATE_LOG
#define APS5_AGC_CREATE_LOG 1
#endif

namespace {
// Relocate using the original guest field address, never the local copy's address.
template <typename T> void ResolveGuestField(T*& field, std::uintptr_t fieldAddress) {
    const auto relative = reinterpret_cast<std::uintptr_t>(field);
    if (relative == 0) return;
    std::uintptr_t address;
    if (relative <= std::numeric_limits<std::intptr_t>::max()) {
        if (relative > std::numeric_limits<std::uintptr_t>::max() - fieldAddress)
            throw std::runtime_error("sceAgcCreateShader: relative pointer overflow");
        address = fieldAddress + relative;
    } else {
        const auto magnitude = ~relative + 1u;
        if (magnitude > fieldAddress) throw std::runtime_error("sceAgcCreateShader: relative pointer underflow");
        address = fieldAddress - magnitude;
    }
    field = reinterpret_cast<T*>(address);
}
void CheckBytes(const void* pointer, std::size_t bytes, bool writable = false) {
    if (bytes != 0) AgcDriverCheckGuestMemory_nid_postfix(pointer, bytes, 1, writable);
}
template <typename T> void CheckArray(const T* pointer, std::size_t count, bool writable = false) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
        throw std::runtime_error("sceAgcCreateShader: array size overflow");
    CheckBytes(pointer, count * sizeof(T), writable);
}

}

extern "C" {

int APS5_VABI sceAgcCreateShader(Shader** dst, void* header, const volatile void* code) {
    constexpr auto fn = __func__;
    if (dst == nullptr) {
        throw std::runtime_error(std::string(fn) + ": dst is null");
    }
    if (header == nullptr) {
        throw std::runtime_error(std::string(fn) + ": header is null");
    }
    if (code == nullptr) {
        throw std::runtime_error(std::string(fn) + ": code is null");
    }

    CheckBytes(header, sizeof(Shader));
    Shader fields;
    std::memcpy(&fields, header, sizeof(fields));
    if (fields.file_header != ShaderRegs::SHADER_FILE_HEADER_MAGIC || fields.version != ShaderRegs::SHADER_VERSION)
        throw std::runtime_error(std::string(fn) + ": invalid shader header or version");
    if (fields.header_size < sizeof(Shader)) throw std::runtime_error(std::string(fn) + ": truncated shader header");
    const auto base = reinterpret_cast<std::uint64_t>(code);
    if ((base & ShaderRegs::SHADER_BASE_ALIGN_MASK) != 0 || fields.shader_size == 0 || (fields.shader_size & 3u) != 0)
        throw std::runtime_error(std::string(fn) + ": invalid shader code address or size");
    AgcDriverCheckGuestMemory_nid_postfix(const_cast<const void*>(code), fields.shader_size, 256, false);
    CheckBytes(header, fields.header_size);
    CheckBytes(header, sizeof(Shader), true);
    CheckBytes(dst, sizeof(*dst), true);
    std::uint32_t programOffset = 0;
    (void)GetProgramAddressRegisterOffset(fields.type, programOffset);

    const auto guest = reinterpret_cast<std::uintptr_t>(header);
    ResolveGuestField(fields.cx_registers, guest + offsetof(Shader, cx_registers));
    ResolveGuestField(fields.sh_registers, guest + offsetof(Shader, sh_registers));
    ResolveGuestField(fields.user_data, guest + offsetof(Shader, user_data));
    ResolveGuestField(fields.specials, guest + offsetof(Shader, specials));
    ResolveGuestField(fields.input_semantics, guest + offsetof(Shader, input_semantics));
    ResolveGuestField(fields.output_semantics, guest + offsetof(Shader, output_semantics));
    // Validate actual mapped spans; independently stored metadata is not forbidden.
    CheckArray(fields.cx_registers, fields.num_cx_registers);
    CheckArray(fields.sh_registers, fields.num_sh_registers);
    CheckBytes(fields.specials, fields.special_sizes_bytes);
    CheckArray(fields.input_semantics, fields.num_input_semantics);
    CheckArray(fields.output_semantics, fields.num_output_semantics);
    ShaderUserData userData{};
    bool userDataChanged = false;
    if (fields.user_data != nullptr) {
        CheckBytes(fields.user_data, sizeof(userData));
        std::memcpy(&userData, fields.user_data, sizeof(userData));
        const auto originalUserData = userData;
        const auto userAddress = reinterpret_cast<std::uintptr_t>(fields.user_data);
        ResolveGuestField(userData.direct_resource_offset, userAddress + offsetof(ShaderUserData, direct_resource_offset));
        for (std::size_t i = 0; i < 4; ++i)
            ResolveGuestField(userData.sharp_resource_offset[i], userAddress + offsetof(ShaderUserData, sharp_resource_offset) + i * sizeof(void*));
        CheckArray(userData.direct_resource_offset, userData.direct_resource_count);
        for (std::size_t i = 0; i < 4; ++i) CheckArray(userData.sharp_resource_offset[i], userData.sharp_resource_count[i]);
        userDataChanged = std::memcmp(&userData, &originalUserData, sizeof(userData)) != 0;
        if (userDataChanged) CheckBytes(fields.user_data, sizeof(userData), true);
    }
    std::vector<ShaderRegister> registers(fields.num_sh_registers);
    if (!registers.empty()) std::memcpy(registers.data(), fields.sh_registers, registers.size() * sizeof(ShaderRegister));
    const auto originalRegisters = registers;
    const int result = PatchProgramAddressRegister(registers.data(), fields.num_sh_registers, fields.type, base);
    if (result != 0) return result;
    const bool registersChanged = !registers.empty() && std::memcmp(registers.data(), originalRegisters.data(), registers.size() * sizeof(ShaderRegister)) != 0;
    if (registersChanged) CheckArray(fields.sh_registers, fields.num_sh_registers, true);
    fields.code = code;

    // Complete malformed-input and access validation before changing guest bytes.
    std::memcpy(header, &fields, sizeof(fields));
    if (userDataChanged) std::memcpy(fields.user_data, &userData, sizeof(userData));
    if (registersChanged) std::memcpy(fields.sh_registers, registers.data(), registers.size() * sizeof(ShaderRegister));
    auto* originalHeader = static_cast<Shader*>(header);
    AgcDriverRegisterShader_nid_postfix(originalHeader);
    std::memcpy(dst, &originalHeader, sizeof(originalHeader));
    return 0;
}

}
