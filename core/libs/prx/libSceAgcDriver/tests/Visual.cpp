#define SDL_MAIN_HANDLED
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

void RequireSdl(bool condition) {
    Require(condition, SDL_GetError());
}
constexpr std::uint32_t Width = 640;
constexpr std::uint32_t Height = 480;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

void PrintReadback(const char* draw) {
    const auto center = (Height / 2 * Width + Width / 2) * 4;
    std::cout << "GPU readback " << draw << " center RGBA=";
    for (std::size_t i = 0; i < 4; ++i) std::cout << (i == 0 ? "" : ",") << std::to_integer<unsigned>(Pixels[center + i]);
    std::cout << " background RGBA=";
    for (std::size_t i = 0; i < 4; ++i) std::cout << (i == 0 ? "" : ",") << std::to_integer<unsigned>(Pixels[i]);
    std::cout << std::endl;
}

ShaderRecompiler::RecompileResult LoadShader(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(file.is_open(), "cannot open test SPIR-V: " + path.string());
    const auto size = file.tellg();
    Require(size >= 20 && size <= 1024 * 1024 && size % 4 == 0, "invalid test SPIR-V size");
    ShaderRecompiler::RecompileResult result;
    result.spirv.resize(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    Require(static_cast<bool>(file.read(reinterpret_cast<char*>(result.spirv.data()), size)), "cannot read test SPIR-V");
    return result;
}

void Run(SDL_Window* window, const std::filesystem::path& directory, bool verifyOnly) {
    unsigned count = 0;
    RequireSdl(SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr) == SDL_TRUE);
    std::vector<const char*> extensions(count);
    RequireSdl(SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data()) == SDL_TRUE);
    const AgcDriver::PresentationWindow presentation{window, extensions, [](void* context, VkInstance instance) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        RequireSdl(SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(context), instance, &surface) == SDL_TRUE);
        return surface;
    }, [](void* context, std::uint32_t* width, std::uint32_t* height) {
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(context), &drawableWidth, &drawableHeight);
        *width = drawableWidth > 0 ? static_cast<std::uint32_t>(drawableWidth) : 0;
        *height = drawableHeight > 0 ? static_cast<std::uint32_t>(drawableHeight) : 0;
    }, Width, Height};
    AgcDriver::VulkanDevice device(&presentation);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(Pixels.data(), Pixels.size(), true, true);
    }
    struct RemovePixels {
        ~RemovePixels() { GuestAllocations::Mutation mutation; mutation.Remove(Pixels.data()); }
    } removePixels;
    auto vertex = LoadShader(directory / "Triangle.vert.spv");
    auto fragment = LoadShader(directory / "Triangle.frag.spv");
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderRecompiler::ShaderStage::Vertex, &vertex, 0},
        {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}
    }};
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        Pixels[i] = std::byte{16};
        Pixels[i + 1] = std::byte{24};
        Pixels[i + 2] = std::byte{40};
        Pixels[i + 3] = std::byte{255};
    }
    AgcDriver::Graphics::State state{};
    state.stages.path = AgcDriver::Graphics::ShaderPath::Vertex;
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size()};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    // The production pipeline consumes decoded color and blend attachment lists.
    state.colors = {state.color};
    state.blends = {state.blend};
    const std::array<std::uint16_t, 3> indices{0, 1, 2};
    const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(indices.data()), 3, 2, 1, 0};
    device.Draw(state, draw, shaders);
    Require(AgcDriver::Graphics::StorageTexture::FindPending(state.color.address, Pixels.size()) != nullptr, "linear draw did not retain a resident target");
    // Draw can record asynchronous work; explicitly flush guest reads before CPU access.
    AgcDriver::GuestMemory::Read(state.color.address, Pixels);
    device.WaitIdle();
    PrintReadback("indexed");
    const std::vector<std::byte> indexedPixels(Pixels.begin(), Pixels.end());
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        Pixels[i] = std::byte{16};
        Pixels[i + 1] = std::byte{24};
        Pixels[i + 2] = std::byte{40};
        Pixels[i + 3] = std::byte{255};
    }
    const AgcDriver::Pm4::DrawParameters autoDraw{0, 3, 0, 1, 0, false};
    device.Draw(state, autoDraw, shaders);
    AgcDriver::GuestMemory::Read(state.color.address, Pixels);
    device.WaitIdle();
    PrintReadback("auto");
    Require(std::equal(Pixels.begin(), Pixels.end(), indexedPixels.begin()), "GPU readback: auto draw differs from indexed triangle");
    const auto center = (Height / 2 * Width + Width / 2) * 4;
    Require(std::to_integer<unsigned>(Pixels[center]) > 30 && std::to_integer<unsigned>(Pixels[center + 1]) > 30 && std::to_integer<unsigned>(Pixels[center + 2]) > 30, "GPU readback: triangle center was not rendered");
    Require(Pixels[0] == std::byte{16} && Pixels[1] == std::byte{24} && Pixels[2] == std::byte{40}, "GPU readback: background changed");
    // CPU modification must be observed by the next resident draw, including blending.
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        Pixels[i] = std::byte{200}; Pixels[i + 1] = std::byte{80};
        Pixels[i + 2] = std::byte{20}; Pixels[i + 3] = std::byte{255};
    }
    state.blend.blendEnable = VK_TRUE;
    state.blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    state.blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    state.blend.colorBlendOp = VK_BLEND_OP_ADD;
    state.blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.blend.alphaBlendOp = VK_BLEND_OP_ADD;
    state.blendConstants[3] = 0.5f;
    state.blends = {state.blend};
    device.Draw(state, autoDraw, shaders);
    auto resident = AgcDriver::Graphics::StorageTexture::FindPending(state.color.address, Pixels.size());
    Require(resident != nullptr, "CPU-modified linear target did not become resident");
    AgcDriver::GuestMemory::Read(state.color.address, Pixels);
    for (std::size_t c = 0; c < 3; ++c) {
        const unsigned background[] = {200, 80, 20};
        const auto expected = (std::to_integer<unsigned>(indexedPixels[center + c]) + background[c]) / 2;
        const auto got = std::to_integer<unsigned>(Pixels[center + c]);
        Require(got + 2 >= expected && got <= expected + 2, "linear blend did not refresh CPU-modified destination pixels");
        Require(Pixels[c] == std::byte(background[c]), "linear refresh changed the untouched background");
    }
    // A different target slot naming identical guest bytes must reuse the same image.
    state.colors[0].slot = 1;
    device.Draw(state, autoDraw, shaders);
    Require(AgcDriver::Graphics::StorageTexture::FindPending(state.color.address, Pixels.size()) == resident, "linear target alias did not reuse its resident image");
    AgcDriver::GuestMemory::Read(state.color.address, Pixels);
    device.WaitIdle();

    // A padded row stays on the CPU transfer path; its non-pixel bytes must survive.
    constexpr unsigned paddedWidth = 65, paddedHeight = 16, pitch = 128 * 4;
    alignas(256) std::array<std::byte, pitch * paddedHeight> padded;
    padded.fill(std::byte{0xa5});
    for (unsigned y = 0; y < paddedHeight; ++y) {
        for (unsigned x = 0; x < paddedWidth; ++x) {
            const auto i = y * pitch + x * 4;
            padded[i] = std::byte{16}; padded[i + 1] = std::byte{24};
            padded[i + 2] = std::byte{40}; padded[i + 3] = std::byte{255};
        }
    }
    auto pitchedState = state;
    pitchedState.color = {reinterpret_cast<std::uintptr_t>(padded.data()), {paddedWidth, paddedHeight}, VK_FORMAT_R8G8B8A8_UNORM, padded.size()};
    pitchedState.colors = {pitchedState.color};
    pitchedState.renderExtent = {paddedWidth, paddedHeight};
    pitchedState.viewport = {0, float(paddedHeight), float(paddedWidth), -float(paddedHeight), 0, 1};
    pitchedState.scissor = {{0, 0}, {paddedWidth, paddedHeight}};
    pitchedState.blend.blendEnable = VK_FALSE;
    pitchedState.blends = {pitchedState.blend};
    { GuestAllocations::Mutation mutation; mutation.Add(padded.data(), padded.size(), true, true); }
    device.Draw(pitchedState, autoDraw, shaders);
    Require(AgcDriver::Graphics::StorageTexture::FindPending(pitchedState.color.address, padded.size()) == nullptr, "padded linear target entered the resident path");
    device.WaitIdle();
    const auto paddedCenter = (paddedHeight / 2 * pitch + paddedWidth / 2 * 4);
    Require(std::to_integer<unsigned>(padded[paddedCenter]) > 30, "padded fallback did not render a pixel");
    for (unsigned y = 0; y < paddedHeight; ++y) {
        for (unsigned x = paddedWidth * 4; x < pitch; ++x) Require(padded[y * pitch + x] == std::byte{0xa5}, "padded fallback zeroed or changed row padding");
    }
    { GuestAllocations::Mutation mutation; mutation.Remove(padded.data()); }
    std::cout << "Linear resident draw, CPU read/modify refresh, blending, alias and padded fallback verified\n";
    device.PresentPixels(Width, Height, Pixels);
    std::cout << "SPIR-V triangle rendered, GPU readback verified, frame queued for presentation. Close the window or press Escape.\n" << std::flush;
    if (!verifyOnly) {
        bool running = true;
        while (running) {
            SDL_Event event{};
            if (SDL_WaitEventTimeout(&event, 100)) {
                if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED) device.PresentPixels(Width, Height, Pixels);
            }
        }
    }
    device.WaitIdle();
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--verify"), "usage: agc_driver_visual_test [--verify]");
        SDL_SetMainReady();
        RequireSdl(SDL_Init(SDL_INIT_VIDEO) == 0);
        {
            const auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(SDL_CreateWindow("AnyPS5 AGC - SPIR-V triangle test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, Width, Height, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN), SDL_DestroyWindow);
            Require(window != nullptr, SDL_GetError());
            const auto base = std::unique_ptr<char, decltype(&SDL_free)>(SDL_GetBasePath(), SDL_free);
            Require(base != nullptr, SDL_GetError());
            Run(window.get(), std::filesystem::path(base.get()), argc == 2);
        }
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
