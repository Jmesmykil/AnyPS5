#include "SceShaders.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
extern "C" int APS5_VABI sceAgcCreateShader(Shader**, void*, const volatile void*);
#include "prx/libSceAgc/Shader/include/CreateShader.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <limits>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace AgcDriver::DriverDetail;
// Inspect the existing registry in this test without adding a production API.
struct RegistryTag {
    using type = std::shared_ptr<ShaderRegistry> Driver::*;
    friend type RegistryMember(RegistryTag);
};
template<class Tag, typename Tag::type Member> struct Access {
    friend typename Tag::type RegistryMember(Tag) { return Member; }
};
template struct Access<RegistryTag, &Driver::shaders>;
static unsigned checks = 0;
static void Require(bool value, const char* text) {
    ++checks;
    if (!value) throw std::runtime_error(text);
}
template<class T> static T Load(const void* bytes) { T value; std::memcpy(&value, bytes, sizeof(value)); return value; }
template<class T> static void Store(void* bytes, const T& value) { std::memcpy(bytes, &value, sizeof(value)); }
struct Fixture {
    // Entire nested layout is SYNTHETIC. Only fixed PAC field sizes informed it.
    alignas(8) std::array<std::byte, 400> memory{};
    alignas(256) std::array<std::uint32_t, 152> code{};
    std::byte* header;
    explicit Fixture(std::size_t offset) : header(memory.data() + offset) {
        Shader fields{};
        fields.file_header = 0x34333231; fields.version = 0x18;
        fields.header_size = 368; fields.shader_size = sizeof(code);
        fields.type = static_cast<std::uint8_t>(ShaderRegs::ShaderBinaryType::Gs);
        fields.num_sh_registers = 6; fields.num_cx_registers = 10;
        fields.special_sizes_bytes = 48; fields.num_input_semantics = 1; fields.num_output_semantics = 1;
        fields.user_data = reinterpret_cast<ShaderUserData*>(288 - offsetof(Shader, user_data));
        fields.sh_registers = reinterpret_cast<ShaderRegister*>(160 - offsetof(Shader, sh_registers));
        fields.cx_registers = reinterpret_cast<ShaderRegister*>(208 - offsetof(Shader, cx_registers));
        fields.specials = reinterpret_cast<ShaderSpecialRegs*>(96 - offsetof(Shader, specials));
        fields.input_semantics = reinterpret_cast<ShaderSemantic*>(144 - offsetof(Shader, input_semantics));
        fields.output_semantics = reinterpret_cast<ShaderSemantic*>(152 - offsetof(Shader, output_semantics));
        Store(header, fields);
        ShaderUserData data{};
        data.direct_resource_count = 2;
        data.direct_resource_offset = reinterpret_cast<std::uint16_t*>(344 - 288);
        for (std::size_t i = 0; i < 4; ++i) {
            data.sharp_resource_count[i] = 1;
            data.sharp_resource_offset[i] = reinterpret_cast<ShaderSharp*>(348 + i * 2 - (288 + 8 + i * 8));
        }
        Store(header + 288, data);
        const std::array<std::uint16_t, 2> direct{3, 7};
        std::memcpy(header + 344, direct.data(), sizeof(direct));
        ShaderRegister lo{ShaderRegs::SPI_SHADER_PGM_LO_ES, 1};
        ShaderRegister hi{ShaderRegs::SPI_SHADER_PGM_LO_ES + 1, 0xabcd1200};
        Store(header + 160, lo); Store(header + 168, hi);
    }
};
static auto Snapshot(Fixture& fixture) {
    return (Driver::Get().*RegistryMember(RegistryTag{}))->at(reinterpret_cast<std::uintptr_t>(fixture.code.data()));
}
template<class Change> static void Invalid(Change change, const char* name) {
    Fixture fixture(1); change(fixture);
    const auto original = fixture.memory;
    Shader* output = reinterpret_cast<Shader*>(0x1234);
    bool rejected = false;
    try { rejected = sceAgcCreateShader(&output, fixture.header, fixture.code.data()) != 0; }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, name);
    Require(fixture.memory == original, "invalid create partially mutated header or nested records");
    Require(output == reinterpret_cast<Shader*>(0x1234), "invalid create published output");
}
int main() {
    try {
        static_assert(sizeof(Shader) == 96 && sizeof(ShaderUserData) == 56);
        for (const auto offset : {0u, 4u, 1u}) {
            Fixture fixture(offset);
            Shader* output = nullptr;
            Require(sceAgcCreateShader(&output, fixture.header, fixture.code.data()) == 0, "packed create failed");
            Require(reinterpret_cast<void*>(output) == fixture.header, "output lost original guest identity");
            const auto fields = Load<Shader>(fixture.header);
            const auto data = Load<ShaderUserData>(fixture.header + 288);
            Require(reinterpret_cast<void*>(fields.user_data) == fixture.header + 288, "outer relocation used copied field address");
            Require(reinterpret_cast<void*>(data.direct_resource_offset) == fixture.header + 344, "nested relocation used copied field address");
            for (std::size_t i = 0; i < 4; ++i)
                Require(reinterpret_cast<void*>(data.sharp_resource_offset[i]) == fixture.header + 348 + i * 2, "sharp relocation is not field relative");
            const auto lo = Load<ShaderRegister>(fixture.header + 160);
            const auto hi = Load<ShaderRegister>(fixture.header + 168);
            const auto expected = reinterpret_cast<std::uintptr_t>(fixture.code.data()) + 256;
            Require(lo.value == static_cast<std::uint32_t>(expected >> 8), "program lo patch incorrect");
            Require(hi.value == (0xabcd1200u | static_cast<std::uint32_t>((expected >> 40) & 255)), "program hi flags lost");
            const auto snapshot = Snapshot(fixture);
            Require(snapshot->headerAddress == reinterpret_cast<std::uintptr_t>(fixture.header), "snapshot guest header identity lost");
            Require(snapshot->codeAddress == reinterpret_cast<std::uintptr_t>(fixture.code.data()), "snapshot code identity lost");
            Require(snapshot->header.size() == 368 && std::memcmp(snapshot->header.data(), fixture.header, 368) == 0, "snapshot bytes differ from registered header");
            fixture.header[95] ^= std::byte{1};
            Require(snapshot->header[95] != fixture.header[95], "snapshot aliases guest header storage");
        }
        // Direct registration failures leave the previously registered snapshot intact.
        {
            Fixture f(1); Shader* output = nullptr;
            Require(sceAgcCreateShader(&output, f.header, f.code.data()) == 0, "registration control failed");
            const auto originalFields = Load<Shader>(f.header);
            const auto previous = Snapshot(f);
            for (unsigned kind = 0; kind < 6; ++kind) {
                Shader bad = originalFields;
                if (kind == 0) bad.file_header = 0;
                if (kind == 1) bad.header_size = 95;
                if (kind == 2) bad.shader_size = 0;
                if (kind == 3) bad.shader_size = 1;
                if (kind == 4) bad.code = reinterpret_cast<const std::byte*>(f.code.data()) + 4;
                if (kind == 5) bad.code = nullptr;
                Store(f.header, bad); const auto original = f.memory;
                bool rejected = false;
                try { AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(f.header)); } catch (const std::runtime_error&) { rejected = true; }
                Require(rejected && f.memory == original, "invalid direct registration mutated or accepted");
                Require(Snapshot(f) == previous, "invalid registration replaced snapshot");
            }
            Fixture destination(1); const auto original = destination.memory; bool rejected = false;
            try { sceAgcCreateShader(reinterpret_cast<Shader**>(1), destination.header, destination.code.data()); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && destination.memory == original, "invalid output address mutated header");
        }
        const auto alter = [](Fixture& f, auto change) { auto fields = Load<Shader>(f.header); change(fields); Store(f.header, fields); };
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.file_header = 0; }); }, "bad magic accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.version = 0; }); }, "bad version accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.header_size = 95; }); }, "short header accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.header_size = std::numeric_limits<std::uint32_t>::max(); }); }, "unmapped full header accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.shader_size = 0; }); }, "zero code accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.shader_size = 1; }); }, "nonword code accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.sh_registers = nullptr; }); }, "null register array accepted");
        Invalid([&](Fixture& f) { auto hi = Load<ShaderRegister>(f.header + 168); hi.offset = 0; Store(f.header + 168, hi); }, "missing hi accepted");
        Invalid([&](Fixture& f) { auto data = Load<ShaderUserData>(f.header + 288); data.direct_resource_count = 0xffff; data.direct_resource_offset = nullptr; Store(f.header + 288, data); }, "nonempty unmapped resource span accepted");
        Invalid([&](Fixture& f) { auto data = Load<ShaderUserData>(f.header + 288); data.direct_resource_offset = reinterpret_cast<std::uint16_t*>(std::uintptr_t{0x8000000000000000}); Store(f.header + 288, data); }, "nested pointer underflow accepted");
        Invalid([&](Fixture& f) { auto data = Load<ShaderUserData>(f.header + 288); data.direct_resource_offset = reinterpret_cast<std::uint16_t*>(1 - reinterpret_cast<std::uintptr_t>(f.header + 288)); Store(f.header + 288, data); }, "unmapped nested array accepted");
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.sh_registers = reinterpret_cast<ShaderRegister*>(1); }); }, "malformed aliased register pair accepted");
        {
            Fixture f(4); auto original = f.memory; Shader* output = nullptr; bool rejected = false;
            try { sceAgcCreateShader(&output, f.header, reinterpret_cast<std::byte*>(f.code.data()) + 4); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && f.memory == original && output == nullptr, "misaligned code mutated or accepted");
            rejected = false;
            try { sceAgcCreateShader(&output, f.header, nullptr); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && f.memory == original, "null code accepted");
        }
        Invalid([&](Fixture& f) { alter(f, [](Shader& s) { s.type = 255; }); }, "invalid shader type accepted");
        // A mapped external nested array is allowed; negative field-relative offsets work.
        {
            std::array<std::uint16_t, 2> external{2, 4}; Fixture f(1);
            auto data = Load<ShaderUserData>(f.header + 288);
            data.direct_resource_offset = reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uintptr_t>(external.data()) - reinterpret_cast<std::uintptr_t>(f.header + 288));
            Store(f.header + 288, data); Shader* output = nullptr;
            Require(sceAgcCreateShader(&output, f.header, f.code.data()) == 0, "external mapped metadata rejected");
            Require(Load<ShaderUserData>(f.header + 288).direct_resource_offset == external.data(), "external relative pointer incorrect");
        }
        {
            Fixture f(1); auto data = Load<ShaderUserData>(f.header + 288);
            data.direct_resource_offset = reinterpret_cast<std::uint16_t*>(std::uintptr_t{144} - 288);
            Store(f.header + 288, data); Shader* output = nullptr;
            Require(sceAgcCreateShader(&output, f.header, f.code.data()) == 0, "negative nested offset rejected");
            Require(reinterpret_cast<void*>(Load<ShaderUserData>(f.header + 288).direct_resource_offset) == f.header + 144, "negative nested offset incorrect");
        }
#ifndef _WIN32
        {
            const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
            auto* mapping = static_cast<std::byte*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            Require(mapping != MAP_FAILED, "boundary mapping failed");
            Fixture f(0); auto* shortHeader = mapping + page - 80;
            std::memcpy(shortHeader, f.header, 80);
            Require(mprotect(mapping + page, page, PROT_NONE) == 0, "boundary protection failed");
            Shader* output = nullptr; bool rejected = false;
            try { sceAgcCreateShader(&output, shortHeader, f.code.data()); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && output == nullptr && std::memcmp(shortHeader, f.header, 80) == 0, "fixed read crossed inaccessible page");
            Require(mprotect(mapping + page, page, PROT_READ | PROT_WRITE) == 0, "restore protection failed");
            auto* nestedHeader = mapping + page - 344;
            std::memcpy(nestedHeader, f.header, 344);
            Require(mprotect(mapping + page, page, PROT_NONE) == 0, "nested protection failed");
            rejected = false;
            try { sceAgcCreateShader(&output, nestedHeader, f.code.data()); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && output == nullptr && std::memcmp(nestedHeader, f.header, 344) == 0, "full read crossed inaccessible page");
            Require(mprotect(mapping + page, page, PROT_READ | PROT_WRITE) == 0, "nested restore failed");
            // Header remains fully readable while an independently stored nested record is truncated.
            Fixture external(1); auto fields = Load<Shader>(external.header);
            fields.user_data = reinterpret_cast<ShaderUserData*>(reinterpret_cast<std::uintptr_t>(mapping + page - 40) - reinterpret_cast<std::uintptr_t>(external.header + offsetof(Shader, user_data)));
            Store(external.header, fields); const auto before = external.memory;
            Require(mprotect(mapping + page, page, PROT_NONE) == 0, "external nested protection failed");
            rejected = false;
            try { sceAgcCreateShader(&output, external.header, external.code.data()); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && external.memory == before, "nested record crossed inaccessible page or mutated header");
            Require(mprotect(mapping + page, page, PROT_READ | PROT_WRITE) == 0, "external nested restore failed");
            auto* readonlyDst = reinterpret_cast<Shader**>(mapping + page);
            Require(mprotect(mapping + page, page, PROT_READ) == 0, "destination protection failed");
            Fixture readonly(4); const auto saved = readonly.memory; rejected = false;
            try { sceAgcCreateShader(readonlyDst, readonly.header, readonly.code.data()); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && readonly.memory == saved, "readonly destination mutated header");
            Require(mprotect(mapping + page, page, PROT_READ | PROT_WRITE) == 0, "destination restore failed");
            fields = Load<Shader>(readonly.header);
            fields.user_data = reinterpret_cast<ShaderUserData*>(reinterpret_cast<std::uintptr_t>(mapping + page) - reinterpret_cast<std::uintptr_t>(readonly.header + offsetof(Shader, user_data)));
            Store(readonly.header, fields); const auto nestedSaved = readonly.memory;
            const auto relativeData = Load<ShaderUserData>(f.header + 288);
            Store(mapping + page, relativeData);
            Require(mprotect(mapping + page, page, PROT_READ) == 0, "readonly nested protection failed");
            rejected = false;
            try { sceAgcCreateShader(&output, readonly.header, readonly.code.data()); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && readonly.memory == nestedSaved, "readonly nested record mutated header");
            munmap(mapping, page * 2);
        }
#endif
        std::printf("ShaderCreateBytes: %u checks PASS; nested fixture SYNTHETIC; host-only\n", checks);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
