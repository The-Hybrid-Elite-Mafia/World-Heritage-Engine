#include "whe/audio_decode.hpp"

#include <bit>
#include <cctype>
#include <fstream>

#include "whe/mapped_file.hpp"

namespace whe {

namespace {

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
u64 le64(const u8* p) { return static_cast<u64>(le32(p)) | (static_cast<u64>(le32(p + 4)) << 32); }
u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p) { return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3]; }
bool tag(const u8* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

// IEEE 754 80-bit extended (AIFF sample rate) -> double.
double ext80ToDouble(const u8* p) {
    const int sign = (p[0] & 0x80) ? -1 : 1;
    const int exponent = ((p[0] & 0x7F) << 8) | p[1];
    u64 mantissa = 0;
    for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | p[2 + i];
    if (exponent == 0 && mantissa == 0) return 0.0;
    return sign * std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
}

enum class PcmKind { Int, UInt8, Float };

// Converts packed PCM to float. `bigEndian` selects byte order.
void convertPcm(const u8* src, std::size_t samples, int bytesPerSample, PcmKind kind, bool bigEndian, float* dst) {
    for (std::size_t i = 0; i < samples; ++i) {
        const u8* s = src + i * static_cast<std::size_t>(bytesPerSample);
        u8 b[8];
        for (int k = 0; k < bytesPerSample; ++k) b[k] = bigEndian ? s[bytesPerSample - 1 - k] : s[k];  // now little-endian
        float v = 0.0f;
        if (kind == PcmKind::Float) {
            if (bytesPerSample == 4) {
                v = std::bit_cast<float>(le32(b));
            } else {
                v = static_cast<float>(std::bit_cast<double>(le64(b)));
            }
        } else if (kind == PcmKind::UInt8) {
            v = (static_cast<int>(b[0]) - 128) / 128.0f;
        } else {
            switch (bytesPerSample) {
                case 1: v = static_cast<i8>(b[0]) / 128.0f; break;
                case 2: v = static_cast<i16>(le16(b)) / 32768.0f; break;
                case 3: {
                    i32 x = static_cast<i32>(b[0] | (b[1] << 8) | (b[2] << 16));
                    if (x & 0x800000) x -= 0x1000000;
                    v = static_cast<float>(x / 8388608.0);
                    break;
                }
                case 4: v = static_cast<float>(static_cast<i32>(le32(b)) / 2147483648.0); break;
                default: v = 0.0f;
            }
        }
        dst[i] = v;
    }
}

}  // namespace

// ============================================================================
// WAV
// ============================================================================
Result<DecodedAudio> decodeWav(std::span<const u8> bytes) {
    const u8* d = bytes.data();
    const std::size_t n = bytes.size();
    if (n < 12 || !(tag(d, "RIFF") || tag(d, "RF64")) || !tag(d + 8, "WAVE")) return fail("not a RIFF/WAVE file");
    const bool rf64 = tag(d, "RF64");

    DecodedAudio out;
    out.format = SourceFormat::Wav;
    u16 formatTag = 0, channels = 0, bits = 0, blockAlign = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    u64 dataSize = 0;
    u64 ds64DataSize = 0;

    std::size_t pos = 12;
    while (pos + 8 <= n) {
        const u8* c = d + pos;
        u64 size = le32(c + 4);
        const u8* body = c + 8;
        const std::size_t bodyAvail = n - (pos + 8);
        if (tag(c, "ds64") && size >= 16 && bodyAvail >= 16) {
            ds64DataSize = le64(body + 8);
        } else if (tag(c, "fmt ") && size >= 16 && bodyAvail >= 16) {
            formatTag = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            blockAlign = le16(body + 12);
            bits = le16(body + 14);
            if (formatTag == 0xFFFE && size >= 40 && bodyAvail >= 40) formatTag = le16(body + 24);  // SubFormat GUID's first two bytes
        } else if (tag(c, "data")) {
            if (rf64 && size == 0xFFFFFFFFu) size = ds64DataSize;
            data = body;
            dataSize = std::min<u64>(size, bodyAvail);
        } else if (tag(c, "smpl") && size >= 36 && bodyAvail >= 36) {
            const u32 unity = le32(body + 12);
            const u32 frac = le32(body + 16);
            if (unity <= 127) out.meta.rootKey = static_cast<int>(unity);
            out.meta.fineTuneCents = frac / 4294967296.0 * 100.0;
            const u32 loops = le32(body + 28);
            if (loops > 0 && size >= 36 + 24 && bodyAvail >= 60) {
                const u8* l = body + 36;
                const u32 start = le32(l + 8);
                const u32 end = le32(l + 12);  // inclusive in the spec
                if (end > start) out.meta.loop = LoopInfo{start, static_cast<u64>(end) + 1};
            }
        } else if (tag(c, "inst") && size >= 7 && bodyAvail >= 7) {
            if (!out.meta.rootKey && body[0] <= 127) out.meta.rootKey = body[0];
            out.meta.fineTuneCents -= static_cast<i8>(body[1]);  // inst fineTune is a correction to apply
            if (body[3] <= 127 && body[4] <= 127 && body[3] <= body[4]) {
                out.meta.lowKey = body[3];
                out.meta.highKey = body[4];
            }
            if (body[5] >= 1 && body[6] <= 127 && body[5] <= body[6]) {
                out.meta.lowVel = body[5];
                out.meta.highVel = body[6];
            }
        }
        if (size > n) break;
        pos += 8 + static_cast<std::size_t>(size) + (size & 1);
    }

    if (!data) return fail("WAV has no data chunk");
    if (channels == 0 || rate == 0 || bits == 0) return fail("WAV has no valid fmt chunk");
    int bytesPerSample = (bits + 7) / 8;
    if (blockAlign != 0 && blockAlign / channels > bytesPerSample) bytesPerSample = blockAlign / channels;  // container wider than valid bits
    PcmKind kind;
    if (formatTag == 1) {
        kind = (bytesPerSample == 1) ? PcmKind::UInt8 : PcmKind::Int;
        if (bytesPerSample < 1 || bytesPerSample > 4) return fail("unsupported WAV PCM width");
    } else if (formatTag == 3) {
        kind = PcmKind::Float;
        if (bytesPerSample != 4 && bytesPerSample != 8) return fail("unsupported WAV float width");
    } else {
        return fail("unsupported WAV format tag " + std::to_string(formatTag) + " (compressed WAV is not supported)");
    }
    out.channels = channels;
    out.sampleRate = rate;
    out.sourceBits = bits;
    out.frames = dataSize / (static_cast<u64>(bytesPerSample) * channels);
    out.interleaved.resize(out.frames * channels);
    convertPcm(data, out.interleaved.size(), bytesPerSample, kind, false, out.interleaved.data());
    if (out.meta.loop && out.meta.loop->end > out.frames) out.meta.loop.reset();
    return out;
}

// ============================================================================
// AIFF / AIFC
// ============================================================================
Result<DecodedAudio> decodeAiff(std::span<const u8> bytes) {
    const u8* d = bytes.data();
    const std::size_t n = bytes.size();
    if (n < 12 || !tag(d, "FORM") || !(tag(d + 8, "AIFF") || tag(d + 8, "AIFC"))) return fail("not an AIFF file");
    const bool aifc = tag(d + 8, "AIFC");

    DecodedAudio out;
    out.format = SourceFormat::Aiff;
    u16 channels = 0, bits = 0;
    u32 frames = 0;
    double rate = 0;
    char comp[5] = "NONE";
    const u8* data = nullptr;
    std::size_t dataAvail = 0;
    struct Marker { u16 id; u32 pos; };
    std::vector<Marker> markers;
    u16 sustainMode = 0, sustainBegin = 0, sustainEnd = 0;
    bool haveInst = false;

    std::size_t pos = 12;
    while (pos + 8 <= n) {
        const u8* c = d + pos;
        const u32 size = be32(c + 4);
        const u8* body = c + 8;
        const std::size_t avail = n - (pos + 8);
        if (tag(c, "COMM") && size >= 18 && avail >= 18) {
            channels = be16(body);
            frames = be32(body + 2);
            bits = be16(body + 6);
            rate = ext80ToDouble(body + 8);
            if (aifc && size >= 22 && avail >= 22) {
                std::memcpy(comp, body + 18, 4);
                comp[4] = 0;
            }
        } else if (tag(c, "SSND") && size >= 8 && avail >= 8) {
            const u32 offset = be32(body);
            if (8ull + offset <= avail) {
                data = body + 8 + offset;
                dataAvail = std::min<std::size_t>(size - 8 - offset, avail - 8 - offset);
            }
        } else if (tag(c, "MARK") && size >= 2 && avail >= 2) {
            const u16 count = be16(body);
            std::size_t p = 2;
            for (u16 i = 0; i < count && p + 7 <= std::min<std::size_t>(size, avail); ++i) {
                Marker m{be16(body + p), be32(body + p + 2)};
                const u8 len = body[p + 6];
                std::size_t pstr = 1 + len;
                if (pstr & 1) ++pstr;
                p += 6 + pstr;
                markers.push_back(m);
            }
        } else if (tag(c, "INST") && size >= 20 && avail >= 20) {
            haveInst = true;
            if (body[0] <= 127) out.meta.rootKey = body[0];
            out.meta.fineTuneCents = -static_cast<i8>(body[1]);  // INST detune is a correction to apply
            if (body[2] <= body[3] && body[3] <= 127) {
                out.meta.lowKey = body[2];
                out.meta.highKey = body[3];
            }
            if (body[4] >= 1 && body[4] <= body[5] && body[5] <= 127) {
                out.meta.lowVel = body[4];
                out.meta.highVel = body[5];
            }
            sustainMode = be16(body + 8);
            sustainBegin = be16(body + 10);
            sustainEnd = be16(body + 12);
        }
        if (size > n) break;
        pos += 8 + static_cast<std::size_t>(size) + (size & 1);
    }
    if (!data) return fail("AIFF has no SSND chunk");
    if (channels == 0 || bits == 0 || rate <= 0) return fail("AIFF has no valid COMM chunk");

    PcmKind kind = PcmKind::Int;
    bool bigEndian = true;
    int bytesPerSample = (bits + 7) / 8;
    const std::string ct(comp);
    if (ct == "NONE" || ct == "twos") {
    } else if (ct == "sowt") {
        bigEndian = false;
    } else if (ct == "fl32" || ct == "FL32") {
        kind = PcmKind::Float;
        bytesPerSample = 4;
    } else if (ct == "fl64" || ct == "FL64") {
        kind = PcmKind::Float;
        bytesPerSample = 8;
    } else {
        return fail("unsupported AIFC compression '" + ct + "'");
    }
    if (kind == PcmKind::Int && (bytesPerSample < 1 || bytesPerSample > 4)) return fail("unsupported AIFF sample width");

    out.channels = channels;
    out.sampleRate = static_cast<u32>(std::lround(rate));
    out.sourceBits = bits;
    out.frames = std::min<u64>(frames, dataAvail / (static_cast<u64>(bytesPerSample) * channels));
    out.interleaved.resize(out.frames * channels);
    convertPcm(data, out.interleaved.size(), bytesPerSample, kind, bigEndian, out.interleaved.data());

    if (haveInst && sustainMode != 0) {
        std::optional<u32> b, e;
        for (const auto& m : markers) {
            if (m.id == sustainBegin) b = m.pos;
            if (m.id == sustainEnd) e = m.pos;
        }
        if (b && e && *e > *b && *e <= out.frames) out.meta.loop = LoopInfo{*b, *e};
    }
    return out;
}

// ============================================================================
// Dispatch
// ============================================================================
Result<DecodedAudio> decodeAudioMemory(std::span<const std::byte> bytes, const std::string& name) {
    std::span<const u8> b(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
    if (b.size() < 12) return fail(name + ": file too small");
    Result<DecodedAudio> r = fail("unknown");
    if (tag(b.data(), "RIFF") || tag(b.data(), "RF64")) {
        r = decodeWav(b);
    } else if (tag(b.data(), "FORM")) {
        r = decodeAiff(b);
    } else if (tag(b.data(), "fLaC") || tag(b.data(), "ID3\x03") || tag(b.data(), "ID3\x04") || (b[0] == 'I' && b[1] == 'D' && b[2] == '3')) {
        r = decodeFlac(b);
    } else {
        return fail(name + ": unrecognized audio container (expected WAV, AIFF or FLAC)");
    }
    if (!r) return fail(name + ": " + r.error().message);
    if (r->frames == 0) return fail(name + ": contains no audio frames");
    return r;
}

Result<DecodedAudio> decodeAudioFile(const fs::path& path) {
    auto mf = MappedFile::open(path);
    if (!mf) return std::unexpected(mf.error());
    return decodeAudioMemory({mf->data(), mf->size()}, path.filename().string());
}

bool isSupportedAudioExtension(const fs::path& path) {
    std::string e = path.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".wav" || e == ".wave" || e == ".flac" || e == ".aif" || e == ".aiff" || e == ".aifc";
}

Result<void> writeWav24(const fs::path& path, u32 sampleRate, u32 channels, std::span<const float> interleaved) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return fail("cannot write " + path.string());
    const u32 dataBytes = static_cast<u32>(interleaved.size() * 3);
    auto w32 = [&](u32 v) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; f.write(b, 4); };
    auto w16 = [&](u16 v) { char b[2] = {char(v), char(v >> 8)}; f.write(b, 2); };
    f.write("RIFF", 4);
    w32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    w32(16);
    w16(1);
    w16(static_cast<u16>(channels));
    w32(sampleRate);
    w32(sampleRate * channels * 3);
    w16(static_cast<u16>(channels * 3));
    w16(24);
    f.write("data", 4);
    w32(dataBytes);
    std::vector<char> buf(interleaved.size() * 3);
    for (std::size_t i = 0; i < interleaved.size(); ++i) {
        const double v = std::clamp(static_cast<double>(interleaved[i]), -1.0, 1.0);
        i32 s = static_cast<i32>(std::lround(v * 8388607.0));
        buf[i * 3] = char(s);
        buf[i * 3 + 1] = char(s >> 8);
        buf[i * 3 + 2] = char(s >> 16);
    }
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!f) return fail("write failed for " + path.string());
    return {};
}

// ============================================================================
// MD5 (RFC 1321)
// ============================================================================
namespace {
struct Md5Tables {
    u32 k[64];
    Md5Tables() {
        for (int i = 0; i < 64; ++i) k[i] = static_cast<u32>(std::floor(std::fabs(std::sin(i + 1.0)) * 4294967296.0));
    }
};
const Md5Tables& md5Tables() {
    static const Md5Tables t;
    return t;
}
constexpr int kMd5Shift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                               14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
}  // namespace

Md5::Md5() : h_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}, buf_{} {}

void Md5::block(const u8* p) {
    const auto& t = md5Tables();
    u32 m[16];
    for (int i = 0; i < 16; ++i) m[i] = le32(p + i * 4);
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    for (int i = 0; i < 64; ++i) {
        u32 f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const u32 tmp = d;
        d = c;
        c = b;
        b = b + std::rotl(a + f + t.k[i] + m[g], kMd5Shift[i]);
        a = tmp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
}

void Md5::update(const u8* data, std::size_t len) {
    total_ += len;
    while (len > 0) {
        const std::size_t take = std::min(len, 64 - used_);
        std::memcpy(buf_ + used_, data, take);
        used_ += take;
        data += take;
        len -= take;
        if (used_ == 64) {
            block(buf_);
            used_ = 0;
        }
    }
}

std::array<u8, 16> Md5::finish() {
    const u64 bitLen = total_ * 8;
    const u8 pad = 0x80;
    update(&pad, 1);
    const u8 zero = 0;
    while (used_ != 56) update(&zero, 1);
    u8 lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<u8>(bitLen >> (8 * i));
    update(lenBytes, 8);
    std::array<u8, 16> out{};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = static_cast<u8>(h_[i] >> (8 * k));
    return out;
}

}  // namespace whe
