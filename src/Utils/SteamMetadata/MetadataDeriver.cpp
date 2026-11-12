#include "MetadataDeriver.h"

#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <toml++/toml.hpp>

namespace MetadataDeriver {

namespace {

// ---- tuning -------------------------------------------------------------
// Values were calibrated against real Steam builds (Oct 14 -> Nov 12 2026):
// with them the carry-forward reproduces every hand-verified entry it
// resolves and resolves nothing wrongly; looser settings started picking
// sibling template instantiations (e.g. CUtlMemoryGrow for 8-byte elements).
constexpr size_t   kMinConcreteAnchored = 10;   // exact hit confirmed by neighbours
constexpr size_t   kMinConcreteAlone    = 12;   // exact hit judged on its own
constexpr size_t   kMinConcreteFuzzy    = 12;   // fuzzy match near the predicted spot
constexpr size_t   kMinConcreteGlobal   = 32;   // fuzzy match anywhere in the image
constexpr uint32_t kMaxPrologueShift    = 16;   // bytes a prologue may have grown by
constexpr uint32_t kMaxFuzzyShift       = 8;
constexpr uint32_t kFuzzyWindow         = 0x10000;
constexpr uint32_t kConsensusDistance   = 0x200000;
constexpr int64_t  kConsensusDelta      = 0x8000;

constexpr size_t kNoMatch = std::numeric_limits<size_t>::max();

// ---- signatures -----------------------------------------------------------

struct Sig {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;   // 1 = concrete, 0 = wildcard
    size_t concrete = 0;
};

std::optional<Sig> ParseSig(std::string_view s)
{
    Sig sig;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == ',') { ++i; continue; }
        if (c == '?') {
            sig.bytes.push_back(0); sig.mask.push_back(0);
            i += (i + 1 < s.size() && s[i + 1] == '?') ? 2 : 1;
            continue;
        }
        if (i + 1 >= s.size()) return std::nullopt;
        const int h = nib(s[i]), l = nib(s[i + 1]);
        if (h < 0 || l < 0) return std::nullopt;
        sig.bytes.push_back(static_cast<uint8_t>((h << 4) | l));
        sig.mask.push_back(1);
        ++sig.concrete;
        i += 2;
    }
    if (sig.bytes.empty()) return std::nullopt;
    return sig;
}

std::string FormatSig(const Sig& sig)
{
    std::string out;
    out.reserve(sig.bytes.size() * 3);
    for (size_t i = 0; i < sig.bytes.size(); ++i) {
        if (i) out += ' ';
        out += sig.mask[i] ? std::format("{:02X}", sig.bytes[i]) : std::string("??");
    }
    return out;
}

// ---- mapped image view ----------------------------------------------------

class Image {
public:
    struct Entry {
        uint32_t begin   = 0;
        uint32_t end     = 0;
        uint32_t primary = 0;   // begin of the function this chunk belongs to
        bool     chained = false;
    };

    bool Init(OSTPlatform::DynamicLibrary::ModuleHandle module)
    {
        auto layout = OSTPlatform::Memory::GetImageLayout(module);
        if (!layout || !layout->base || layout->functions.empty()) return false;
        layout_ = std::move(*layout);
        for (const auto& s : layout_.sections) {
            if (s.executable) exec_.push_back(&s);
            else if (s.name == ".rdata") rdata_ = &s;
        }
        return !exec_.empty();
    }

    const uint8_t* Data() const { return layout_.base; }
    size_t Size() const { return layout_.size; }
    uintptr_t Base() const { return reinterpret_cast<uintptr_t>(layout_.base); }
    const std::vector<const OSTPlatform::Memory::ImageSection*>& Exec() const { return exec_; }
    const OSTPlatform::Memory::ImageSection* RData() const { return rdata_; }

    bool InExec(uint32_t rva) const
    {
        for (const auto* s : exec_)
            if (rva >= s->rva && rva < s->rva + s->size) return true;
        return false;
    }

    std::optional<Entry> Lookup(uint32_t rva) const
    {
        const auto& fns = layout_.functions;
        auto it = std::upper_bound(fns.begin(), fns.end(), rva,
                                   [](uint32_t v, const auto& f) { return v < f.begin; });
        if (it == fns.begin()) return std::nullopt;
        --it;
        if (rva >= it->end) return std::nullopt;

        Entry e{it->begin, it->end, it->begin, false};
        uint32_t unwind = it->unwindInfo;
        for (int hop = 0; hop < 32; ++hop) {
            uint32_t chainRf = 0;
            if (unwind & 1) {
                chainRf = unwind & ~1u;                      // indirect: points at a RUNTIME_FUNCTION
            } else {
                if (!Readable(unwind, 4)) return std::nullopt;
                const uint8_t flags = Data()[unwind] >> 3;
                if (!(flags & 0x4)) break;                   // UNW_FLAG_CHAININFO
                const uint8_t codes = Data()[unwind + 2];
                chainRf = unwind + 4 + ((codes + 1u) & ~1u) * 2u;
            }
            if (!Readable(chainRf, 12)) return std::nullopt;
            uint32_t begin = 0;
            std::memcpy(&begin, Data() + chainRf, 4);
            std::memcpy(&unwind, Data() + chainRf + 8, 4);
            e.primary = begin;
            e.chained = true;
        }
        if (e.primary == e.begin) e.chained = false;
        return e;
    }

    bool IsFunctionStart(uint32_t rva) const
    {
        if (rva == 0 || rva >= Size()) return false;
        if (const auto e = Lookup(rva)) return e->begin == rva && !e->chained;
        // Leaf functions have no unwind entry; accept them only right after
        // inter-function padding or a return.
        if (!InExec(rva)) return false;
        const uint8_t prev = Data()[rva - 1];
        return (prev == 0xCC || prev == 0xC3) && Data()[rva] != 0xCC;
    }

    // End of a function including the contiguous chunks chained back to it
    // (MSVC splits shrink-wrapped functions into several unwind entries).
    uint32_t ExtentEnd(uint32_t primary) const
    {
        auto e = Lookup(primary);
        if (!e) return primary;
        uint32_t end = e->end;
        for (int i = 0; i < 64; ++i) {
            auto next = Lookup(end);
            if (!next || next->begin != end || next->primary != primary) break;
            end = next->end;
        }
        return end;
    }

    const std::vector<uint32_t>& CandidateStarts()
    {
        if (!candidates_.empty()) return candidates_;
        for (const auto& f : layout_.functions) {
            if (auto e = Lookup(f.begin); e && !e->chained && e->begin == f.begin)
                candidates_.push_back(f.begin);
        }
        for (const auto* s : exec_) {
            for (uint32_t p = (s->rva + 15) & ~15u; p + 1 < s->rva + s->size; p += 16) {
                const uint8_t prev = Data()[p - 1];
                if ((prev == 0xCC || prev == 0xC3) && Data()[p] != 0xCC && !Lookup(p))
                    candidates_.push_back(p);
            }
        }
        std::sort(candidates_.begin(), candidates_.end());
        candidates_.erase(std::unique(candidates_.begin(), candidates_.end()), candidates_.end());
        return candidates_;
    }

    size_t Mismatches(uint64_t pos, const Sig& sig, size_t cap) const
    {
        if (pos + sig.bytes.size() > Size()) return kNoMatch;
        const uint8_t* p = Data() + pos;
        size_t mis = 0;
        for (size_t i = 0; i < sig.bytes.size(); ++i) {
            if (sig.mask[i] && p[i] != sig.bytes[i] && ++mis > cap) return mis;
        }
        return mis;
    }

    // Up to `limit` exact matches within executable sections.
    std::vector<uint32_t> FindAll(const Sig& sig, size_t limit) const
    {
        std::vector<uint32_t> hits;
        size_t anchor = 0;
        while (anchor < sig.mask.size() && !sig.mask[anchor]) ++anchor;
        if (anchor == sig.mask.size()) return hits;

        for (const auto* s : exec_) {
            if (s->size < sig.bytes.size()) continue;
            const uint8_t* begin = Data() + s->rva;
            const size_t last = s->size - sig.bytes.size();   // last valid start offset
            size_t off = 0;
            while (off <= last) {
                const void* found = std::memchr(begin + off + anchor, sig.bytes[anchor], last - off + 1);
                if (!found) break;
                off = static_cast<size_t>(static_cast<const uint8_t*>(found) - begin) - anchor;
                if (Mismatches(s->rva + off, sig, 0) == 0) {
                    hits.push_back(static_cast<uint32_t>(s->rva + off));
                    if (hits.size() >= limit) return hits;
                }
                ++off;
            }
        }
        return hits;
    }

private:
    bool Readable(uint64_t rva, size_t n) const { return rva + n <= Size(); }

    OSTPlatform::Memory::ImageLayout layout_;
    std::vector<const OSTPlatform::Memory::ImageSection*> exec_;
    const OSTPlatform::Memory::ImageSection* rdata_ = nullptr;
    std::vector<uint32_t> candidates_;
};

// ---- pattern derivation -----------------------------------------------------

struct Match {
    uint32_t    rva = 0;
    Sig         sig;     // signature valid for the new build (may be extended)
    std::string how;
};

// Unique exact hit at (or a few bytes into the prologue of) a function.
std::optional<Match> ExactMatch(const Image& img, const Sig& sig)
{
    if (sig.concrete < kMinConcreteAnchored) return std::nullopt;
    const auto hits = img.FindAll(sig, 2);
    if (hits.size() != 1) return std::nullopt;
    const uint32_t h = hits[0];
    if (img.IsFunctionStart(h)) return Match{h, sig, "exact"};

    // The function gained prologue bytes (e.g. an extra push) in front of the
    // old signature: re-anchor at the real start and prepend those bytes.
    const auto e = img.Lookup(h);
    if (e && !e->chained && h > e->begin && h - e->begin <= kMaxPrologueShift) {
        const uint32_t f = e->begin;
        Sig ext;
        for (uint32_t i = f; i < h; ++i) { ext.bytes.push_back(img.Data()[i]); ext.mask.push_back(1); }
        ext.bytes.insert(ext.bytes.end(), sig.bytes.begin(), sig.bytes.end());
        ext.mask.insert(ext.mask.end(), sig.mask.begin(), sig.mask.end());
        ext.concrete = sig.concrete + (h - f);
        return Match{f, std::move(ext), std::format("exact, prologue grew by {} bytes", h - f)};
    }
    return std::nullopt;
}

// No other function start comes within one byte of the signature.
bool TwinFree(Image& img, uint32_t rva, const Sig& sig)
{
    for (uint32_t f : img.CandidateStarts()) {
        if (f != rva && img.Mismatches(f, sig, 1) <= 1) return false;
    }
    return true;
}

// Rebuild a signature from the new bytes at `f` (+`shift` prologue bytes),
// keeping the old wildcard positions.
Sig ResampleSig(const Image& img, uint32_t f, uint32_t shift, const Sig& old)
{
    Sig out;
    for (uint32_t i = 0; i < shift; ++i) { out.bytes.push_back(img.Data()[f + i]); out.mask.push_back(1); }
    for (size_t i = 0; i < old.bytes.size(); ++i) {
        out.bytes.push_back(old.mask[i] ? img.Data()[f + shift + i] : 0);
        out.mask.push_back(old.mask[i]);
    }
    out.concrete = old.concrete + shift;
    return out;
}

std::optional<Match> FuzzyNear(Image& img, const Sig& sig, uint32_t pred)
{
    if (sig.concrete < kMinConcreteFuzzy) return std::nullopt;
    const size_t allowed = std::max<size_t>(2, sig.concrete / 8);
    const uint32_t lo = pred > kFuzzyWindow ? pred - kFuzzyWindow : 0;
    const uint32_t hi = pred + kFuzzyWindow;

    struct Cand { size_t mis; uint32_t shift; uint32_t f; };
    std::vector<Cand> cands;
    const auto& starts = img.CandidateStarts();
    for (auto it = std::lower_bound(starts.begin(), starts.end(), lo);
         it != starts.end() && *it <= hi; ++it) {
        Cand best{kNoMatch, 0, *it};
        for (uint32_t k = 0; k <= kMaxFuzzyShift; ++k) {
            const size_t mis = img.Mismatches(static_cast<uint64_t>(*it) + k, sig, allowed + 2);
            if (mis < best.mis) best = {mis, k, *it};
        }
        if (best.mis != kNoMatch) cands.push_back(best);
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.mis != b.mis ? a.mis < b.mis : a.shift < b.shift; });
    if (cands.empty() || cands[0].mis > allowed) return std::nullopt;
    if (cands.size() > 1 && cands[1].mis < cands[0].mis + 2) return std::nullopt;   // no clear winner

    const Cand& c = cands[0];
    if (c.shift) {
        const auto e = img.Lookup(c.f + c.shift);
        if (!e || e->primary != c.f) return std::nullopt;
    }
    return Match{c.f, ResampleSig(img, c.f, c.shift, sig),
                 std::format("fuzzy near predicted 0x{:X}: {} of {} bytes changed, prologue +{}",
                             pred, c.mis, sig.concrete, c.shift)};
}

std::optional<Match> FuzzyGlobal(Image& img, const Sig& sig)
{
    if (sig.concrete < kMinConcreteGlobal) return std::nullopt;
    const size_t allowed = sig.concrete / 16;
    size_t best = kNoMatch, second = kNoMatch;
    uint32_t bestF = 0;
    for (uint32_t f : img.CandidateStarts()) {
        const size_t mis = img.Mismatches(f, sig, allowed + 2);
        if (mis < best) { second = best; best = mis; bestF = f; }
        else if (mis < second) { second = mis; }
    }
    if (best > allowed) return std::nullopt;
    if (second != kNoMatch && second < best + 2) return std::nullopt;
    return Match{bestF, ResampleSig(img, bestF, 0, sig),
                 std::format("fuzzy (moved): {} of {} bytes changed", best, sig.concrete)};
}

} // namespace

// ---- TOML I/O ----------------------------------------------------------------

PatternSet ParsePatternToml(std::string_view body, std::string* error)
{
    PatternSet set;
    toml::table tbl;
    try {
        tbl = toml::parse(body);
    } catch (const toml::parse_error& e) {
        if (error) *error = std::string(e.description());
        return set;
    }
    for (auto& [rawKey, val] : tbl) {
        if (!val.is_table()) continue;
        const auto key = OSTPlatform::Numbers::ParseHexUInt32(std::string(rawKey.str()));
        if (!key) continue;
        const auto& sub = *val.as_table();
        PatternSpec spec;
        if (auto v = sub["name"].value<std::string>()) spec.name = *v;
        if (auto v = sub["rva"].value<std::string>()) {
            if (const auto rva = OSTPlatform::Numbers::ParseHexUInt64(*v); rva && *rva <= 0xFFFFFFFFull)
                spec.rva = static_cast<uint32_t>(*rva);
        }
        if (auto v = sub["sig"].value<std::string>()) spec.sig = *v;
        set[*key] = std::move(spec);
    }
    return set;
}

std::string SerializePatternToml(const PatternSet& set, std::string_view header)
{
    std::string out(header);
    for (const auto& [key, spec] : set) {
        if (!out.empty()) out += '\n';
        if (!spec.note.empty()) out += std::format("# {}\n", spec.note);
        out += std::format("[0x{:08X}]\n", key);
        out += std::format("name = \"{}\"\n", spec.name);
        if (spec.rva) out += std::format("rva = \"0x{:X}\"\n", spec.rva);
        if (!spec.sig.empty()) out += std::format("sig = \"{}\"\n", spec.sig);
    }
    return out;
}

// ---- pattern reconciliation ---------------------------------------------------

PatternOutcome ReconcilePatterns(OSTPlatform::DynamicLibrary::ModuleHandle module,
                                 const PatternSet& current,
                                 const std::vector<PatternSet>& baselines,
                                 bool derive)
{
    PatternOutcome out;
    Image img;
    if (!img.Init(module)) {
        LOG_WARN("MetadataDeriver: module layout unavailable; using metadata unvalidated");
        out.entries = current;
        out.validated = current.size();
        out.imageOk = false;
        return out;
    }

    // 1. Validate what we were given.
    std::map<uint32_t, uint32_t> known;   // key -> rva (validated)
    std::set<uint32_t> needed;
    for (const auto& [key, spec] : current) {
        bool ok = false;
        PatternSpec fixed = spec;
        const auto sig = spec.sig.empty() ? std::nullopt : ParseSig(spec.sig);
        if (spec.rva) {
            ok = spec.rva < img.Size() && img.IsFunctionStart(spec.rva) &&
                 (!sig || img.Mismatches(spec.rva, *sig, 0) == 0);
        } else if (sig) {
            const auto hits = img.FindAll(*sig, 2);
            if (hits.size() == 1 && img.IsFunctionStart(hits[0])) {
                fixed.rva = hits[0];
                ok = true;
            }
        }
        if (ok) {
            known[key] = fixed.rva;
            out.entries[key] = std::move(fixed);
            ++out.validated;
        } else {
            LOG_WARN("MetadataDeriver: '{}' rva=0x{:X} failed validation (not a function start or sig mismatch)",
                     spec.name, spec.rva);
            out.rejected.push_back(spec.name);
            out.changed = true;
            needed.insert(key);
        }
    }
    if (!derive) return out;

    if (current.empty()) {
        for (const auto& b : baselines)
            for (const auto& [key, spec] : b) needed.insert(key);
    }
    if (needed.empty()) return out;

    // Candidate signatures per key: the rejected entry's own sig first, then
    // baselines newest-first. Names / previous RVAs come from the same sources.
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, std::vector<Sig>> sigs;
    std::map<uint32_t, int64_t> old;
    auto addSig = [&](uint32_t key, const std::string& text) {
        if (text.empty()) return;
        auto s = ParseSig(text);
        if (!s) return;
        auto& v = sigs[key];
        for (const auto& e : v)
            if (e.bytes == s->bytes && e.mask == s->mask) return;
        v.push_back(std::move(*s));
    };
    for (uint32_t key : needed) {
        if (auto it = current.find(key); it != current.end()) {
            names[key] = it->second.name;
            addSig(key, it->second.sig);
        }
        for (const auto& b : baselines) {
            if (auto it = b.find(key); it != b.end()) {
                if (names[key].empty()) names[key] = it->second.name;
                addSig(key, it->second.sig);
            }
        }
    }
    if (!baselines.empty()) {
        for (const auto& [key, spec] : baselines.front())
            if (spec.rva) old[key] = spec.rva;
    }

    // 2. Exact carry-forward.
    std::map<uint32_t, std::vector<Match>> exact;
    for (uint32_t key : needed) {
        for (const auto& s : sigs[key]) {
            if (auto m = ExactMatch(img, s)) exact[key].push_back(std::move(*m));
        }
    }

    // Anchors: (previous rva -> new rva) pairs we trust enough to predict from.
    std::map<uint32_t, uint32_t> anchors;
    for (const auto& [key, rva] : known)
        if (old.count(key)) anchors[key] = rva;
    for (const auto& [key, ms] : exact) {
        if (!old.count(key) || ms.empty()) continue;
        const bool agree = std::all_of(ms.begin(), ms.end(), [&](const Match& m) { return m.rva == ms[0].rva; });
        if (agree && ms[0].sig.concrete >= kMinConcreteAlone) anchors.emplace(key, ms[0].rva);
    }

    auto consistent = [&](uint32_t key, uint32_t rva) {
        const auto ok = old.find(key);
        if (ok == old.end()) return false;
        const int64_t d = static_cast<int64_t>(rva) - ok->second;
        for (const auto& [q, qrva] : anchors) {
            if (q == key) continue;
            const int64_t qo = old.at(q);
            if (qo == ok->second) continue;                         // alias of the same function
            if (std::llabs(qo - ok->second) > kConsensusDistance) continue;
            if (std::llabs((static_cast<int64_t>(qrva) - qo) - d) <= kConsensusDelta) return true;
        }
        return false;
    };

    std::map<uint32_t, Match> result;
    for (auto& [key, ms] : exact) {
        std::map<uint32_t, Match*> ok;
        for (auto& m : ms) {
            if (ok.count(m.rva)) continue;
            if (consistent(key, m.rva)) {
                m.how += ", consistent with neighbours";
                ok[m.rva] = &m;
            } else if (m.sig.concrete >= kMinConcreteAlone && TwinFree(img, m.rva, m.sig)) {
                m.how += ", moved but unambiguous";
                ok[m.rva] = &m;
            }
        }
        if (ok.size() == 1) result.emplace(key, std::move(*ok.begin()->second));
        else if (ok.size() > 1) LOG_WARN("MetadataDeriver: '{}' has conflicting exact matches; skipped", names[key]);
    }

    // 3. Fuzzy: near the location predicted by the nearest anchor, then
    //    anywhere for long, distinctive signatures.
    std::vector<std::pair<int64_t, int64_t>> points;   // (old rva, delta)
    for (const auto& [key, rva] : known)
        if (old.count(key)) points.emplace_back(old[key], static_cast<int64_t>(rva) - old[key]);
    for (const auto& [key, m] : result)
        if (old.count(key)) points.emplace_back(old[key], static_cast<int64_t>(m.rva) - old[key]);

    for (uint32_t key : needed) {
        if (result.count(key)) continue;
        std::optional<Match> m;
        if (old.count(key) && !points.empty()) {
            const auto nearest = *std::min_element(points.begin(), points.end(), [&](const auto& a, const auto& b) {
                return std::llabs(a.first - old[key]) < std::llabs(b.first - old[key]);
            });
            const int64_t pred = old[key] + nearest.second;
            if (pred > 0 && pred < static_cast<int64_t>(img.Size())) {
                for (const auto& s : sigs[key]) {
                    if ((m = FuzzyNear(img, s, static_cast<uint32_t>(pred)))) break;
                }
            }
        }
        if (!m) {
            for (const auto& s : sigs[key]) {
                if ((m = FuzzyGlobal(img, s))) break;
            }
        }
        if (m) result.emplace(key, std::move(*m));
    }

    // 4. Two names on one function only if they already shared one before.
    std::map<uint32_t, std::vector<uint32_t>> byRva;
    for (const auto& [key, m] : result) byRva[m.rva].push_back(key);
    for (const auto& [key, rva] : known) byRva[rva].push_back(key);
    for (const auto& [rva, keys] : byRva) {
        if (keys.size() < 2) continue;
        std::set<int64_t> olds;
        for (uint32_t k : keys) olds.insert(old.count(k) ? old[k] : -1 - static_cast<int64_t>(k));
        if (olds.size() == 1) continue;
        for (uint32_t k : keys) {
            if (result.erase(k))
                LOG_WARN("MetadataDeriver: '{}' resolved to 0x{:X} shared with another function; skipped",
                         names[k], rva);
        }
    }

    // 5. Assemble.
    for (uint32_t key : needed) {
        auto it = result.find(key);
        if (it == result.end()) {
            out.underivable.push_back(names[key]);
            LOG_WARN("MetadataDeriver: could not derive '{}' safely", names[key]);
            continue;
        }
        PatternSpec spec;
        spec.name = names[key];
        spec.rva  = it->second.rva;
        spec.sig  = FormatSig(it->second.sig);
        spec.note = "derived: " + it->second.how;
        LOG_INFO("MetadataDeriver: {} -> 0x{:X} ({})", spec.name, spec.rva, it->second.how);
        out.entries[key] = std::move(spec);
        ++out.derived;
        out.changed = true;
    }
    return out;
}

// ---- IPC derivation ---------------------------------------------------------------

std::vector<IpcMethodFound> DeriveIpcMethods(OSTPlatform::DynamicLibrary::ModuleHandle module,
                                             const std::vector<IpcMethodRequest>& requests)
{
    std::vector<IpcMethodFound> found;
    Image img;
    if (requests.empty() || !img.Init(module) || !img.RData()) return found;

    const uint8_t* data = img.Data();
    const auto* rdata = img.RData();
    const std::string_view rdataView(reinterpret_cast<const char*>(data + rdata->rva), rdata->size);

    // Name strings (exact, NUL-delimited) -> id.
    std::vector<std::string> nameList;
    std::unordered_map<std::string, int> nameId;
    auto idOf = [&](const std::string& n) {
        auto [it, inserted] = nameId.emplace(n, static_cast<int>(nameList.size()));
        if (inserted) nameList.push_back(n);
        return it->second;
    };
    for (const auto& r : requests) { idOf(r.iface); idOf(r.method); }

    std::unordered_map<uint32_t, int> stringAt;   // rva of string -> name id
    for (size_t id = 0; id < nameList.size(); ++id) {
        std::string needle;
        needle.reserve(nameList[id].size() + 2);
        needle += '\0'; needle += nameList[id]; needle += '\0';
        for (size_t pos = rdataView.find(needle); pos != std::string_view::npos;
             pos = rdataView.find(needle, pos + 1)) {
            stringAt[static_cast<uint32_t>(rdata->rva + pos + 1)] = static_cast<int>(id);
        }
    }
    if (stringAt.empty()) return found;

    // One pass over code: `lea r64, [rip+disp32]` referencing those strings,
    // attributed to the function that owns the instruction.
    std::unordered_map<uint32_t, std::set<int>> refs;   // function -> name ids
    for (const auto* s : img.Exec()) {
        const uint32_t end = s->rva + s->size;
        for (uint32_t i = s->rva; i + 7 <= end; ++i) {
            const uint8_t b0 = data[i];
            if ((b0 != 0x48 && b0 != 0x4C) || data[i + 1] != 0x8D || (data[i + 2] & 0xC7) != 0x05) continue;
            int32_t disp = 0;
            std::memcpy(&disp, data + i + 3, 4);
            const int64_t target = static_cast<int64_t>(i) + 7 + disp;
            if (target < 0 || target > 0xFFFFFFFFll) continue;
            const auto hit = stringAt.find(static_cast<uint32_t>(target));
            if (hit == stringAt.end()) continue;
            if (const auto e = img.Lookup(i)) refs[e->primary].insert(hit->second);
        }
    }

    // Functions that are referenced from a vtable (first slot found).
    std::unordered_map<uint32_t, uint32_t> slotOf;   // function -> rva of the slot
    {
        std::unordered_set<uint32_t> wanted;
        for (const auto& [f, ids] : refs) wanted.insert(f);
        const uintptr_t base = img.Base();
        for (uint32_t off = (8 - (rdata->rva & 7)) & 7; off + 8 <= rdata->size; off += 8) {
            uint64_t v = 0;
            std::memcpy(&v, data + rdata->rva + off, 8);
            if (v < base || v >= base + img.Size()) continue;
            const auto f = static_cast<uint32_t>(v - base);
            if (wanted.count(f)) slotOf.emplace(f, rdata->rva + off);
        }
    }

    // Client wrappers serialise `funcHash` then `fencepost`, each as
    //   mov r8d, 4 ; mov dword ptr [rbp|rsp + disp], imm32 ; call CUtlBuffer::Put
    auto readStores = [&](uint32_t f) {
        std::vector<uint32_t> imms;
        const uint32_t end = img.ExtentEnd(f);
        for (uint32_t i = f; i + 16 <= end && imms.size() < 2; ++i) {
            if (std::memcmp(data + i, "\x41\xB8\x04\x00\x00\x00\xC7", 7) != 0) continue;
            const uint8_t* p = data + i + 7;
            size_t immOff = 0;
            if (p[0] == 0x45) immOff = 2;                                  // [rbp+disp8]
            else if (p[0] == 0x44 && p[1] == 0x24) immOff = 3;             // [rsp+disp8]
            else if (p[0] == 0x85) immOff = 5;                             // [rbp+disp32]
            else if (p[0] == 0x84 && p[1] == 0x24) immOff = 6;             // [rsp+disp32]
            else continue;
            uint32_t imm = 0;
            std::memcpy(&imm, p + immOff, 4);
            imms.push_back(imm);
        }
        return imms;
    };

    auto vtableStart = [&](uint32_t slot) {
        uint32_t start = slot;
        const uintptr_t base = img.Base();
        while (start >= rdata->rva + 8) {
            uint64_t v = 0;
            std::memcpy(&v, data + start - 8, 8);
            if (v < base || v >= base + img.Size() || !img.InExec(static_cast<uint32_t>(v - base))) break;
            start -= 8;
        }
        return start;
    };

    for (const auto& r : requests) {
        const int ifaceId = nameId.at(r.iface), methodId = nameId.at(r.method);
        std::vector<IpcMethodFound> cands;
        for (const auto& [f, ids] : refs) {
            if (!ids.count(ifaceId) || !ids.count(methodId)) continue;
            const auto slot = slotOf.find(f);
            if (slot == slotOf.end()) continue;
            const auto imms = readStores(f);
            if (imms.size() < 2 || !imms[0] || !imms[1]) continue;
            IpcMethodFound m;
            m.iface = r.iface;
            m.method = r.method;
            m.funcHash = imms[0];
            m.fencepost = imms[1];
            m.wrapperRva = f;
            m.vtableRva = vtableStart(slot->second);
            m.methodIndex = (slot->second - m.vtableRva) / 8;
            cands.push_back(std::move(m));
        }
        if (cands.size() == 1) {
            LOG_INFO("MetadataDeriver: IPC {}::{} -> wrapper 0x{:X} hash=0x{:08X} fencepost=0x{:08X} slot {}",
                     r.iface, r.method, cands[0].wrapperRva, cands[0].funcHash, cands[0].fencepost,
                     cands[0].methodIndex);
            found.push_back(std::move(cands[0]));
        } else {
            LOG_WARN("MetadataDeriver: IPC {}::{} not derived ({} candidates)", r.iface, r.method, cands.size());
        }
    }
    return found;
}

} // namespace MetadataDeriver
