#pragma once

#include "OSTPlatform/include/DynamicLibrary.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Local recovery of pattern / IPC metadata for Steam builds that upstream
// (steam-monitor) has not published yet.
//
// Everything here works on the module as mapped in this process and is biased
// hard towards "skip" over "guess": a wrong function pointer crashes Steam or
// corrupts memory, a missing one only disables the hooks that need it.
namespace MetadataDeriver {

    // ---- pattern metadata ------------------------------------------------

    struct PatternSpec {
        std::string name;
        uint32_t    rva = 0;   // 0 = absent
        std::string sig;       // IDA-style bytes; empty = absent
        std::string note;      // how the entry was obtained (serialised as a comment)
    };

    // key = Fnv1aHash(name), as used by the steam-monitor TOML section names.
    using PatternSet = std::map<uint32_t, PatternSpec>;

    PatternSet  ParsePatternToml(std::string_view body, std::string* error = nullptr);
    std::string SerializePatternToml(const PatternSet& set, std::string_view header);

    struct PatternOutcome {
        PatternSet               entries;      // only entries that are safe to use
        size_t                   validated = 0; // input entries that checked out as-is
        size_t                   derived = 0;   // entries produced or repaired locally
        std::vector<std::string> rejected;      // input entries that failed validation
        std::vector<std::string> underivable;   // names that could not be derived
        bool                     changed = false;
        bool                     imageOk = true; // false: module layout unreadable, input passed through
    };

    // Validate `current` against the mapped module (each RVA must be a real
    // function start and its sig must match there). With `derive`, invalid
    // entries — and, when `current` is empty, every name in `baselines` — are
    // re-derived from baseline signatures. `baselines` are newest-first; the
    // first one's RVAs are the "previous build" used to predict locations.
    PatternOutcome ReconcilePatterns(OSTPlatform::DynamicLibrary::ModuleHandle module,
                                     const PatternSet& current,
                                     const std::vector<PatternSet>& baselines,
                                     bool derive);

    // ---- IPC metadata ----------------------------------------------------

    struct IpcMethodRequest {
        std::string iface;
        std::string method;
    };

    struct IpcMethodFound {
        std::string iface;
        std::string method;
        uint32_t    funcHash    = 0;
        uint32_t    fencepost   = 0;
        uint32_t    wrapperRva  = 0;
        uint32_t    vtableRva   = 0;
        uint32_t    methodIndex = 0;
    };

    // Locate each method's client-side IPC wrapper (the function that
    // references both the method-name and interface-name strings and sits in
    // a vtable) and read funcHash / fencepost from its serialisation code.
    // Methods that cannot be identified unambiguously are omitted.
    std::vector<IpcMethodFound> DeriveIpcMethods(OSTPlatform::DynamicLibrary::ModuleHandle module,
                                                 const std::vector<IpcMethodRequest>& requests);

} // namespace MetadataDeriver
