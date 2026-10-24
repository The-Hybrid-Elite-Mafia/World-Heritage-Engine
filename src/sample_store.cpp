#include "whe/sample_store.hpp"

#include <cstdio>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace whe {

namespace {

u64 fnv1a64(const std::string& s) {
    u64 h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

i64 mtimeOf(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<i64>(t.time_since_epoch().count());
}

bool headerCurrent(const fs::path& whs, u64 size, i64 mtime) {
    std::ifstream f(whs, std::ios::binary);
    if (!f) return false;
    WhsHeader h{};
    f.read(reinterpret_cast<char*>(&h), sizeof h);
    if (!f) return false;
    return std::memcmp(h.magic, kWhsMagic, 8) == 0 && h.version == 1 && h.sourceSize == size && h.sourceMtime == mtime &&
           h.dataOffset == kWhsDataOffset;
}

}  // namespace

Result<fs::path> ensureCached(const fs::path& source, const fs::path& cacheDir, IngestStats& stats) {
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(source, ec);
    const fs::path key = ec ? source : abs;
    const u64 size = fs::file_size(source, ec);
    if (ec) return fail("cannot stat " + source.string());
    const i64 mtime = mtimeOf(source);

    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a64(key.generic_string())));
    const fs::path whs = cacheDir / (std::string(hex) + ".whs");
    if (headerCurrent(whs, size, mtime)) {
        ++stats.cacheHits;
        return whs;
    }

    auto decoded = decodeAudioFile(source);
    if (!decoded) return std::unexpected(decoded.error());

    // The playback path is stereo: multichannel recordings (e.g. several mic
    // positions interleaved in one file) are folded to stereo once, here.
    if (decoded->channels > 2) {
        const u32 C = decoded->channels;
        std::vector<float> st(decoded->frames * 2);
        for (u64 f = 0; f < decoded->frames; ++f) {
            float l = 0, r = 0;
            u32 nl = 0, nr = 0;
            for (u32 c = 0; c < C; ++c) {
                const float v = decoded->interleaved[f * C + c];
                if (c % 2 == 0) { l += v; ++nl; } else { r += v; ++nr; }
            }
            st[f * 2] = l / static_cast<float>(nl);
            st[f * 2 + 1] = r / static_cast<float>(nr);
        }
        decoded->interleaved = std::move(st);
        decoded->channels = 2;
    }

    WhsHeader h{};
    std::memcpy(h.magic, kWhsMagic, 8);
    h.version = 1;
    h.channels = decoded->channels;
    h.sampleRate = decoded->sampleRate;
    h.sourceBits = decoded->sourceBits;
    h.frames = decoded->frames;
    h.rootKey = decoded->meta.rootKey.value_or(-1);
    h.fineTuneCents = static_cast<float>(decoded->meta.fineTuneCents);
    h.lowKey = decoded->meta.lowKey.value_or(-1);
    h.highKey = decoded->meta.highKey.value_or(-1);
    h.lowVel = decoded->meta.lowVel.value_or(-1);
    h.highVel = decoded->meta.highVel.value_or(-1);
    if (decoded->meta.loop) {
        h.loopStart = decoded->meta.loop->start;
        h.loopEnd = decoded->meta.loop->end;
    }
    h.sourceSize = size;
    h.sourceMtime = mtime;
    h.dataOffset = kWhsDataOffset;
    float peak = 0.0f;
    for (float v : decoded->interleaved) peak = std::max(peak, std::fabs(v));
    h.peak = peak;
    h.sourceFormat = static_cast<u32>(decoded->format);
    h.md5Verified = decoded->md5Verified ? 1u : 0u;
    const std::string name = source.filename().string();
    std::snprintf(h.sourceName, sizeof h.sourceName, "%s", name.c_str());

    fs::create_directories(cacheDir, ec);
    const fs::path tmp = whs.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot write cache file " + tmp.string());
        std::vector<char> head(kWhsDataOffset, 0);
        std::memcpy(head.data(), &h, sizeof h);
        f.write(head.data(), static_cast<std::streamsize>(head.size()));
        f.write(reinterpret_cast<const char*>(decoded->interleaved.data()),
                static_cast<std::streamsize>(decoded->interleaved.size() * sizeof(float)));
        if (!f) return fail("disk full or write error on " + tmp.string());
    }
    fs::rename(tmp, whs, ec);
    if (ec) return fail("cannot finalize cache file " + whs.string());
    ++stats.decoded;
    if (decoded->md5Verified) ++stats.md5Verified;
    return whs;
}

Result<u32> SampleStore::addCached(const fs::path& whsPath, const fs::path& originalSource) {
    auto mf = MappedFile::open(whsPath);
    if (!mf) return std::unexpected(mf.error());
    if (mf->size() < kWhsDataOffset) return fail("cache file too small: " + whsPath.string());
    auto s = std::make_unique<StoredSample>();
    std::memcpy(&s->header, mf->data(), sizeof(WhsHeader));
    const WhsHeader& h = s->header;
    if (std::memcmp(h.magic, kWhsMagic, 8) != 0 || h.channels == 0 || h.sampleRate == 0) return fail("corrupt cache file: " + whsPath.string());
    const u64 need = h.dataOffset + h.frames * h.channels * sizeof(float);
    if (mf->size() < need) return fail("truncated cache file: " + whsPath.string());
    s->map = std::move(*mf);
    s->frames = reinterpret_cast<const float*>(s->map.data() + h.dataOffset);
    s->source = originalSource;
    samples_.push_back(std::move(s));
    return static_cast<u32>(samples_.size() - 1);
}

void SampleStore::finalize(bool lockInRam) {
    std::size_t total = 0;
    for (auto& s : samples_) {
        s->preloadFrames = static_cast<u32>(std::min<u64>(preloadFrames_, s->header.frames));
        total += static_cast<std::size_t>(s->preloadFrames) * s->header.channels;
        total = (total + 15) & ~std::size_t{15};  // keep every head 64-byte aligned
    }
    arena_.resize(total);
    std::size_t offset = 0;
    for (auto& s : samples_) {
        const std::size_t count = static_cast<std::size_t>(s->preloadFrames) * s->header.channels;
        std::memcpy(arena_.data() + offset, s->frames, count * sizeof(float));
        s->preload = arena_.data() + offset;
        offset = (offset + count + 15) & ~std::size_t{15};
    }
    locked_ = false;
    if (lockInRam && arena_.size()) {
#ifdef _WIN32
        const SIZE_T bytes = arena_.size() * sizeof(float);
        SIZE_T minWs = 0, maxWs = 0;
        HANDLE proc = GetCurrentProcess();
        if (GetProcessWorkingSetSize(proc, &minWs, &maxWs)) SetProcessWorkingSetSize(proc, minWs + bytes + (1u << 24), maxWs + bytes + (1u << 24));
        locked_ = VirtualLock(arena_.data(), bytes) != 0;
#else
        locked_ = mlock(arena_.data(), arena_.size() * sizeof(float)) == 0;
#endif
    }
}

u64 SampleStore::mappedBytes() const noexcept {
    u64 t = 0;
    for (auto& s : samples_) t += s->map.size();
    return t;
}

}  // namespace whe
