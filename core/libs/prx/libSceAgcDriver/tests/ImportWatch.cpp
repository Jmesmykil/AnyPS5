#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <stdexcept>
#include <type_traits>
#if defined(__linux__)
#include <sys/mman.h>
#endif
namespace {
using namespace AgcDriver::Graphics;
namespace Memory = AgcDriver::GuestMemory;
constexpr std::size_t Bytes = 65536;
enum class Failure { None, Properties, Allocate, Bind, ThrowCreate };
Failure failure = Failure::None;
volatile std::uint8_t* host = nullptr;
int liveBuffers = 0, liveMemory = 0;
std::uintptr_t nextHandle=10;
std::map<VkBuffer,VkDeviceSize> bufferSizes;
std::map<VkBuffer,bool> externalBuffers;
std::map<VkBuffer,VkDeviceMemory> bufferMemory;
std::map<VkDeviceMemory,std::vector<std::byte>> mappedMemory;
template<typename T> T handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* output) {
    const bool external=info->pNext!=nullptr;
    if (external&&failure==Failure::ThrowCreate) {host[17]=0x52;throw std::runtime_error("controlled import exception");}
    *output=handle<VkBuffer>(++nextHandle);bufferSizes[*output]=info->size;externalBuffers[*output]=external;++liveBuffers;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    bufferSizes.erase(buffer);externalBuffers.erase(buffer);bufferMemory.erase(buffer);--liveBuffers;
}
VKAPI_ATTR VkResult VKAPI_CALL properties(VkDevice, VkExternalMemoryHandleTypeFlagBits, const void*, VkMemoryHostPointerPropertiesEXT* output) {
    if(failure==Failure::Properties)return VK_ERROR_INVALID_EXTERNAL_HANDLE;output->memoryTypeBits=1;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* output) {
    output->size=bufferSizes.at(buffer);output->alignment=4096;output->memoryTypeBits=1;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* output) {
    bool external=false;
    for(auto* extension=static_cast<const VkBaseInStructure*>(info->pNext);extension!=nullptr;extension=extension->pNext)
        if(extension->sType==VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT)external=true;
    if(external){host[17]=0x52;if(failure==Failure::Allocate)return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
    *output=handle<VkDeviceMemory>(++nextHandle);mappedMemory[*output].resize(info->allocationSize);++liveMemory;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {mappedMemory.erase(memory);--liveMemory;}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize) {
    if(externalBuffers.at(buffer)&&failure==Failure::Bind)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    bufferMemory[buffer]=memory;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL mapMemory(VkDevice,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize,VkMemoryMapFlags,void** output) {
    *output=mappedMemory.at(memory).data()+offset;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL unmapMemory(VkDevice,VkDeviceMemory) {}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL address(VkDevice, const VkBufferDeviceAddressInfo*) {return 0x10000;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolve(VkDevice, const char* name) {
#define RESOLVE(text, function) if (std::strcmp(name,text)==0) return reinterpret_cast<PFN_vkVoidFunction>(function)
    RESOLVE("vkCreateBuffer",create); RESOLVE("vkDestroyBuffer",destroy);
    RESOLVE("vkGetMemoryHostPointerPropertiesEXT",properties); RESOLVE("vkGetBufferMemoryRequirements",requirements);
    RESOLVE("vkAllocateMemory",allocate); RESOLVE("vkFreeMemory",freeMemory);
    RESOLVE("vkMapMemory",mapMemory); RESOLVE("vkUnmapMemory",unmapMemory);
    RESOLVE("vkBindBufferMemory",bind); RESOLVE("vkGetBufferDeviceAddressKHR",address);
#undef RESOLVE
    return nullptr;
}
#if defined(__linux__)
struct Block {
    explicit Block(const Context& context) : context(context) {
        raw = mmap(nullptr, Bytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED,"cannot map watched import test");
        base = (reinterpret_cast<std::uintptr_t>(raw)+Bytes-1)&~(static_cast<std::uintptr_t>(Bytes)-1);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(reinterpret_cast<void*>(base),Bytes);
        if(Memory::WriteWatched())Require(Memory::Watched(base,Bytes),"test allocation must start watched");
        GuestAllocations::Mutation mutation; mutation.Add(reinterpret_cast<void*>(base),Bytes,true,true);
        host = reinterpret_cast<volatile std::uint8_t*>(base);
    }
    ~Block() {
        { GuestAllocations::Mutation mutation; mutation.Remove(reinterpret_cast<void*>(base)); }
        HostImportFor(context,base,Bytes); // Reconcile both successful and rejected imports.
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(reinterpret_cast<void*>(base),Bytes);
        munmap(raw,Bytes*2);
    }
    const Context& context; void* raw=nullptr; std::uintptr_t base=0;
};
void check(const Context& context, Failure selected, ImportWatch policy) {
    failure = selected; SetImportWatch(context,policy); Block block(context);
    std::memset(reinterpret_cast<void*>(block.base),0,Bytes);
    const auto before = Memory::CollectWrites(block.base,Bytes);
    const bool available=Memory::WriteWatched();
    Require(available?(before!=0):(before==0),"test generation availability differs");
    host[9]=0x31; // Preexisting dirty data must survive an import window too.
    const HostImport* result=nullptr; bool threw=false;
    try { result=HostImportFor(context,block.base,Bytes); }
    catch (const std::runtime_error&) { threw=true; }
    const bool success=selected==Failure::None;
    Require(threw==(selected==Failure::ThrowCreate),"exception contract differs");
    Require(success==(result!=nullptr),"failed import did not fall back");
    Require(host[9]==0x31,"import lost preexisting CPU bytes");
    if (available&&(!success || policy==ImportWatch::Watch)) {
        Require(Memory::Watched(block.base,Bytes),"failed import removed CPU write tracking");
        Memory::CollectWritesUncached(block.base,Bytes);
        const std::array<std::uint64_t,1> generations{before};
        std::array<std::uint8_t,1> changed{},cpu{};
        Require(Memory::ChangedBlocks(block.base,Bytes,generations,changed,cpu)&&changed[0]!=0&&cpu[0]!=0,"CPU stores before/during failed import were lost");
        const auto settled=Memory::CollectWrites(block.base,Bytes);
        host[33]=0x73;Memory::CollectWritesUncached(block.base,Bytes);
        Require(!Memory::UnchangedSince(block.base,Bytes,settled),"CPU store after failed import was lost");
    } else if(success&&policy==ImportWatch::Unwatch) {
        Require(result->unwatched&&!Memory::Watched(block.base,Bytes),"successful import ignored unwatch policy");
        Require(Memory::CollectWrites(block.base,Bytes)==0,"successful unwatched import claims tracked state");
    }
    Require(!success || policy==ImportWatch::Unwatch || !result->unwatched,"watch-policy success marked unwatched");
    if(!success){
        // Exercise the real fallback uploader, then a fresh upload after a CPU store.
        failure=Failure::Allocate; // Imports continue to be rejected; ordinary staging allocation succeeds.
        for(int version=0;version<2;++version){
            host[41]=static_cast<std::uint8_t>(0x90+version);
            GuestBufferMemory upload(context);upload.AddReadable(block.base,Bytes);upload.Upload(false);
            std::uint32_t adjustment=0;const auto descriptor=upload.Descriptor(block.base,Bytes,adjustment);
            Require(adjustment==0,"unexpected fallback descriptor adjustment");
            const auto& actual=mappedMemory.at(bufferMemory.at(descriptor.buffer));
            Require(descriptor.offset<=actual.size()&&Bytes<=actual.size()-descriptor.offset,"fallback upload bounds");
            Require(std::memcmp(actual.data()+descriptor.offset,reinterpret_cast<const void*>(block.base),Bytes)==0,"fallback upload lost guest bytes or CPU refresh");
        }
    }

}
#endif
}
int main() {
#if !defined(__linux__)
    std::cout<<"Linux asynchronous write-watch regression only\n";return 77;
#else
    const bool available=Memory::WriteWatched();
    if(!available)std::cout<<"No write watching: exercising actual import/copy/cleanup baseline controls\n";
    try {
        Context context{};context.device=handle<VkDevice>(0x492);context.deviceProc=resolve;context.hostImportAlignment=Bytes;context.memory.memoryTypeCount=1;context.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;context.limits.maxMemoryAllocationCount=4096;context.limits.minStorageBufferOffsetAlignment=16;context.limits.maxStorageBufferRange=Bytes;
        if(!available){const auto automatic=PrepareImportWatch(context);Require(automatic==ImportWatch::Watch,"Auto None policy unexpectedly unwatches");}
        for (const auto selected:{Failure::Properties,Failure::Allocate,Failure::Bind,Failure::ThrowCreate}) {
            check(context,selected,ImportWatch::Unwatch);
            Require(liveBuffers==0&&liveMemory==0,"failed import leaked Vulkan objects");
        }
        check(context,Failure::None,ImportWatch::Unwatch);
        Require(liveBuffers==0&&liveMemory==0,"successful unwatched import cleanup leaked");
        check(context,Failure::None,ImportWatch::Watch);
        Require(liveBuffers==0&&liveMemory==0,"successful watched import cleanup leaked");
        std::cout<<"Failed/throwing imports preserve CPU tracking; successful policies, actual fallback bytes/CPU refresh and cleanup passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"Import watch regression failed: "<<error.what()<<"\n";return 1;}
#endif
}
