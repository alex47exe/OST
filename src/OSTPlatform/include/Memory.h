#pragma once

#include "include/DynamicLibrary.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OSTPlatform::Memory {

    struct ModuleImage {
        uint8_t* base = nullptr;
        size_t size = 0;
    };

    // One entry of the image's x64 exception directory (.pdata). All values
    // are RVAs; `unwindInfo` points at the UNWIND_INFO block for the range.
    struct RuntimeFunction {
        uint32_t begin = 0;
        uint32_t end = 0;
        uint32_t unwindInfo = 0;
    };

    struct ImageSection {
        std::string name;
        uint32_t rva = 0;
        uint32_t size = 0;
        bool executable = false;
    };

    // Layout of a module as mapped in this process (headers read from memory,
    // so RVAs/pointers reflect relocation). `functions` is sorted by `begin`.
    struct ImageLayout {
        uint8_t* base = nullptr;
        size_t size = 0;
        std::vector<ImageSection> sections;
        std::vector<RuntimeFunction> functions;
    };

    std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module);
    std::optional<ImageLayout> GetImageLayout(DynamicLibrary::ModuleHandle module);
    bool WriteExecutableByte(void* target, uint8_t value);

    // True if [address, address + size) is committed, readable memory. Safe to
    // call with arbitrary values (nullptr, non-canonical addresses, junk).
    bool IsReadable(const void* address, size_t size);

} // namespace OSTPlatform::Memory
