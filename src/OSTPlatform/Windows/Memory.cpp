#include "include/Memory.h"

#include "include/Log.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>

namespace OSTPlatform::Memory {

std::optional<ImageLayout> GetImageLayout(DynamicLibrary::ModuleHandle module) {
    const auto image = GetModuleImage(module);
    if (!image) return std::nullopt;

    ImageLayout layout;
    layout.base = image->base;
    layout.size = image->size;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image->base);
    if (image->size < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    if (dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image->size)
        return std::nullopt;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image->base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return std::nullopt;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto* s = sec + i;
        if (reinterpret_cast<const uint8_t*>(s + 1) > image->base + image->size) break;
        ImageSection out;
        out.name.assign(reinterpret_cast<const char*>(s->Name),
                        strnlen(reinterpret_cast<const char*>(s->Name), IMAGE_SIZEOF_SHORT_NAME));
        out.rva = s->VirtualAddress;
        out.size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        out.executable = (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (static_cast<size_t>(out.rva) + out.size > image->size) continue;
        layout.sections.push_back(std::move(out));
    }

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.VirtualAddress && dir.Size &&
        static_cast<size_t>(dir.VirtualAddress) + dir.Size <= image->size) {
        const auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(image->base + dir.VirtualAddress);
        const size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
        layout.functions.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!rf[i].BeginAddress || rf[i].EndAddress <= rf[i].BeginAddress) continue;
            layout.functions.push_back({rf[i].BeginAddress, rf[i].EndAddress, rf[i].UnwindData});
        }
        std::sort(layout.functions.begin(), layout.functions.end(),
                  [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin < b.begin; });
    }
    return layout;
}

std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module) {
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(module), &info, sizeof(info))) {
        OSTP_LOG_DEBUG("GetModuleImage(module={}) failed (error={})", module, GetLastError());
        return std::nullopt;
    }

    return ModuleImage{
        static_cast<uint8_t*>(info.lpBaseOfDll),
        static_cast<size_t>(info.SizeOfImage),
    };
}

bool IsReadable(const void* address, size_t size) {
    if (!address || size == 0) return false;

    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto end   = begin + size;
    if (end < begin) return false;

    // A range may span several regions; walk them all.
    for (uintptr_t cur = begin; cur < end; ) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

bool WriteExecutableByte(void* target, uint8_t value) {
    if (!target) {
        OSTP_LOG_WARN("WriteExecutableByte: target is null");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OSTP_LOG_WARN("WriteExecutableByte(target={}) VirtualProtect(RWX) failed (error={})",
                      target, GetLastError());
        return false;
    }
    *static_cast<uint8_t*>(target) = value;

    DWORD ignored = 0;
    if (!VirtualProtect(target, 1, oldProtect, &ignored)) {
        OSTP_LOG_WARN("WriteExecutableByte(target={}) VirtualProtect(restore=0x{:X}) failed (error={})",
                      target, oldProtect, GetLastError());
        return false;
    }
    if (!FlushInstructionCache(GetCurrentProcess(), target, 1)) {
        OSTP_LOG_WARN("WriteExecutableByte(target={}) FlushInstructionCache failed (error={})",
                      target, GetLastError());
        return false;
    }
    return true;
}

} // namespace OSTPlatform::Memory
