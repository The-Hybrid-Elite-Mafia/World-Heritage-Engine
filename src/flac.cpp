// Native FLAC decoder written from the format specification (RFC 9639).
// Supports: STREAMINFO, every subframe type (CONSTANT, VERBATIM, FIXED 0-4,
// LPC 1-32), wasted bits, Rice/Rice2 residuals with escape partitions,
// independent / left-side / side-right / mid-side channel decorrelation,
// variable and fixed blocking, and MD5 verification of the decoded stream.
// Not supported: Ogg-encapsulated FLAC (.oga/.ogg).
#include <bit>

#include "whe/audio_decode.hpp"

namespace whe {

namespace {

class BitReader {
public:
    BitReader(const u8* d, std::size_t n, std::size_t startByte) : d_(d), n_(n), byte_(startByte) {}

    u32 read(int bits) {
        if (bits == 0) return 0;
        refill();
        const u32 v = static_cast<u32>(cache_ >> (64 - bits));
        cache_ <<= bits;
        cbits_ -= bits;
        return v;
    }
    u64 read64(int bits) {
        if (bits <= 32) return read(bits);
        const u64 hi = read(bits - 32);
        return (hi << 32) | read(32);
    }
    i64 readSigned(int bits) {
        if (bits == 0) return 0;
        const u64 v = read64(bits);
        const u64 sign = u64{1} << (bits - 1);
        return static_cast<i64>((v ^ sign) - sign);
    }
    u32 readUnary() {
        u32 count = 0;
        for (;;) {
            refill();
            if (cbits_ == 0) {
                overrun_ = true;
                return count;
            }
            const int lz = std::countl_zero(cache_);
            if (lz >= cbits_) {  // all buffered bits are zero
                count += static_cast<u32>(cbits_);
                cache_ = 0;
                cbits_ = 0;
                if (byte_ >= n_) {
                    overrun_ = true;
                    return count;
                }
                continue;
            }
            cache_ <<= (lz + 1);
            cbits_ -= lz + 1;
            return count + static_cast<u32>(lz);
        }
    }
    void alignByte() {
        const int drop = cbits_ & 7;
        cache_ <<= drop;
        cbits_ -= drop;
    }
    // Byte position of the next unread bit (only meaningful when byte aligned).
    std::size_t bytePosition() const { return byte_ - static_cast<std::size_t>(cbits_ / 8); }
    bool overrun() const { return overrun_ || bytePosition() > n_; }

private:
    void refill() {
        while (cbits_ <= 56) {
            u64 b = 0;
            if (byte_ < n_) {
                b = d_[byte_];
            } else if (byte_ >= n_ + 8) {
                overrun_ = true;
                return;
            }
            ++byte_;
            cache_ |= b << (56 - cbits_);
            cbits_ += 8;
        }
    }
    const u8* d_;
    std::size_t n_;
    std::size_t byte_;
    u64 cache_ = 0;
    int cbits_ = 0;
    bool overrun_ = false;
};

struct StreamInfo {
    u32 maxBlock = 0;
    u32 sampleRate = 0;
    u32 channels = 0;
    u32 bps = 0;
    u64 totalSamples = 0;
    std::array<u8, 16> md5{};
};

Result<void> decodeResidual(BitReader& br, int blockSize, int order, i64* residual) {
    const u32 method = br.read(2);
    if (method > 1) return fail("reserved residual coding method");
    const int paramBits = method == 0 ? 4 : 5;
    const u32 escape = method == 0 ? 15u : 31u;
    const int partitionOrder = static_cast<int>(br.read(4));
    const int partitions = 1 << partitionOrder;
    if ((blockSize >> partitionOrder) < order && partitionOrder > 0) return fail("bad residual partition order");
    int idx = 0;
    for (int p = 0; p < partitions; ++p) {
        int count = (blockSize >> partitionOrder) - (p == 0 ? order : 0);
        if (count < 0) return fail("bad residual partition size");
        const u32 param = br.read(paramBits);
        if (param == escape) {
            const int rawBits = static_cast<int>(br.read(5));
            for (int i = 0; i < count; ++i) residual[idx++] = br.readSigned(rawBits);
        } else {
            const int k = static_cast<int>(param);
            for (int i = 0; i < count; ++i) {
                const u64 q = br.readUnary();
                const u64 v = (q << k) | br.read(k);
                residual[idx++] = static_cast<i64>(v >> 1) ^ -static_cast<i64>(v & 1);
            }
        }
        if (br.overrun()) return fail("truncated residual");
    }
    return {};
}

Result<void> decodeSubframe(BitReader& br, int blockSize, int bps, i64* out, std::vector<i64>& residual) {
    if (br.read(1) != 0) return fail("subframe padding bit set");
    const u32 type = br.read(6);
    int wasted = 0;
    if (br.read(1)) wasted = static_cast<int>(br.readUnary()) + 1;
    bps -= wasted;
    if (bps <= 0 || bps > 33) return fail("invalid subframe bit depth");

    if (type == 0) {  // CONSTANT
        const i64 v = br.readSigned(bps);
        for (int i = 0; i < blockSize; ++i) out[i] = v;
    } else if (type == 1) {  // VERBATIM
        for (int i = 0; i < blockSize; ++i) out[i] = br.readSigned(bps);
    } else if (type >= 8 && type <= 12) {  // FIXED
        const int order = static_cast<int>(type - 8);
        if (order > blockSize) return fail("fixed order exceeds block size");
        for (int i = 0; i < order; ++i) out[i] = br.readSigned(bps);
        if (auto r = decodeResidual(br, blockSize, order, residual.data()); !r) return r;
        const i64* res = residual.data();
        for (int i = order; i < blockSize; ++i) {
            const i64 e = res[i - order];
            switch (order) {
                case 0: out[i] = e; break;
                case 1: out[i] = e + out[i - 1]; break;
                case 2: out[i] = e + 2 * out[i - 1] - out[i - 2]; break;
                case 3: out[i] = e + 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
                case 4: out[i] = e + 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
            }
        }
    } else if (type >= 32) {  // LPC
        const int order = static_cast<int>(type - 31);
        if (order > blockSize) return fail("LPC order exceeds block size");
        for (int i = 0; i < order; ++i) out[i] = br.readSigned(bps);
        const u32 precisionCode = br.read(4);
        if (precisionCode == 15) return fail("invalid LPC precision");
        const int precision = static_cast<int>(precisionCode) + 1;
        const int shift = static_cast<int>(br.readSigned(5));
        if (shift < 0) return fail("negative LPC shift");
        i64 coeffs[32];
        for (int i = 0; i < order; ++i) coeffs[i] = br.readSigned(precision);
        if (auto r = decodeResidual(br, blockSize, order, residual.data()); !r) return r;
        const i64* res = residual.data();
        for (int i = order; i < blockSize; ++i) {
            i64 sum = 0;
            for (int j = 0; j < order; ++j) sum += coeffs[j] * out[i - 1 - j];
            out[i] = res[i - order] + (sum >> shift);
        }
    } else {
        return fail("reserved subframe type");
    }
    if (wasted) {
        for (int i = 0; i < blockSize; ++i) out[i] = static_cast<i64>(static_cast<u64>(out[i]) << wasted);
    }
    if (br.overrun()) return fail("truncated subframe");
    return {};
}

}  // namespace

Result<DecodedAudio> decodeFlac(std::span<const u8> bytes) {
    const u8* d = bytes.data();
    const std::size_t n = bytes.size();
    std::size_t pos = 0;

    // Optional ID3v2 tag in front of the stream.
    if (n >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        const u32 size = (u32(d[6] & 0x7F) << 21) | (u32(d[7] & 0x7F) << 14) | (u32(d[8] & 0x7F) << 7) | u32(d[9] & 0x7F);
        pos = 10 + size + ((d[5] & 0x10) ? 10 : 0);
    }
    if (pos + 4 > n || std::memcmp(d + pos, "fLaC", 4) != 0) return fail("missing fLaC marker");
    pos += 4;

    StreamInfo si;
    bool haveInfo = false;
    for (bool last = false; !last;) {
        if (pos + 4 > n) return fail("truncated metadata");
        last = (d[pos] & 0x80) != 0;
        const int type = d[pos] & 0x7F;
        const std::size_t len = (std::size_t(d[pos + 1]) << 16) | (std::size_t(d[pos + 2]) << 8) | d[pos + 3];
        pos += 4;
        if (pos + len > n) return fail("truncated metadata block");
        if (type == 0 && len >= 34) {
            const u8* b = d + pos;
            si.maxBlock = (u32(b[2]) << 8) | b[3];
            si.sampleRate = (u32(b[10]) << 12) | (u32(b[11]) << 4) | (b[12] >> 4);
            si.channels = ((b[12] >> 1) & 7) + 1;
            si.bps = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
            si.totalSamples = (u64(b[13] & 0x0F) << 32) | (u64(b[14]) << 24) | (u64(b[15]) << 16) | (u64(b[16]) << 8) | b[17];
            std::memcpy(si.md5.data(), b + 18, 16);
            haveInfo = true;
        }
        pos += len;
    }
    if (!haveInfo) return fail("missing STREAMINFO");

    DecodedAudio out;
    out.format = SourceFormat::Flac;
    out.sampleRate = si.sampleRate;
    out.channels = si.channels;
    out.sourceBits = si.bps;
    if (si.totalSamples) out.interleaved.reserve(si.totalSamples * si.channels);

    const bool md5Present = std::ranges::any_of(si.md5, [](u8 b) { return b != 0; });
    Md5 md5;
    std::vector<u8> md5Buf;

    std::vector<std::vector<i64>> ch(8);
    std::vector<i64> residual;
    const double scale = 1.0 / std::ldexp(1.0, static_cast<int>(si.bps) - 1);

    while (pos + 2 <= n) {
        if (si.totalSamples && out.frames >= si.totalSamples) break;
        if (!(d[pos] == 0xFF && (d[pos + 1] & 0xFE) == 0xF8)) {
            // Trailing junk (e.g. ID3v1 tag) after the last frame.
            if (out.frames > 0) break;
            return fail("frame sync not found");
        }
        BitReader br(d, n, pos);
        br.read(15);  // sync + reserved
        br.read(1);   // blocking strategy
        const u32 bsCode = br.read(4);
        const u32 srCode = br.read(4);
        const u32 chanCode = br.read(4);
        const u32 ssCode = br.read(3);
        br.read(1);
        // UTF-8-like coded frame/sample number.
        {
            const u32 first = br.read(8);
            int extra = 0;
            if ((first & 0x80) == 0) extra = 0;
            else if ((first & 0xE0) == 0xC0) extra = 1;
            else if ((first & 0xF0) == 0xE0) extra = 2;
            else if ((first & 0xF8) == 0xF0) extra = 3;
            else if ((first & 0xFC) == 0xF8) extra = 4;
            else if ((first & 0xFE) == 0xFC) extra = 5;
            else if (first == 0xFE) extra = 6;
            else return fail("bad frame number encoding");
            for (int i = 0; i < extra; ++i) br.read(8);
        }
        int blockSize = 0;
        if (bsCode == 1) blockSize = 192;
        else if (bsCode >= 2 && bsCode <= 5) blockSize = 576 << (bsCode - 2);
        else if (bsCode == 6) blockSize = static_cast<int>(br.read(8)) + 1;
        else if (bsCode == 7) blockSize = static_cast<int>(br.read(16)) + 1;
        else if (bsCode >= 8) blockSize = 256 << (bsCode - 8);
        else return fail("reserved block size code");
        if (srCode == 12) br.read(8);
        else if (srCode == 13 || srCode == 14) br.read(16);
        else if (srCode == 15) return fail("invalid sample rate code");
        br.read(8);  // CRC-8 (integrity is verified end-to-end through MD5)

        int bps = 0;
        switch (ssCode) {
            case 0: bps = static_cast<int>(si.bps); break;
            case 1: bps = 8; break;
            case 2: bps = 12; break;
            case 4: bps = 16; break;
            case 5: bps = 20; break;
            case 6: bps = 24; break;
            case 7: bps = 32; break;
            default: return fail("reserved sample size code");
        }
        int channels;
        if (chanCode <= 7) channels = static_cast<int>(chanCode) + 1;
        else if (chanCode <= 10) channels = 2;
        else return fail("reserved channel assignment");
        if (static_cast<u32>(channels) != si.channels) return fail("channel count changes mid-stream");

        residual.resize(static_cast<std::size_t>(blockSize));
        for (int c = 0; c < channels; ++c) {
            ch[c].resize(static_cast<std::size_t>(blockSize));
            int subBps = bps;
            if ((chanCode == 8 && c == 1) || (chanCode == 9 && c == 0) || (chanCode == 10 && c == 1)) subBps += 1;  // side channel
            if (auto r = decodeSubframe(br, blockSize, subBps, ch[c].data(), residual); !r) return fail(r.error().message);
        }
        br.alignByte();
        br.read(16);  // CRC-16
        if (br.overrun()) return fail("truncated frame");
        pos = br.bytePosition();

        // Undo stereo decorrelation.
        if (chanCode >= 8) {
            i64* a = ch[0].data();
            i64* b = ch[1].data();
            for (int i = 0; i < blockSize; ++i) {
                if (chanCode == 8) {  // left / side
                    b[i] = a[i] - b[i];
                } else if (chanCode == 9) {  // side / right
                    a[i] = a[i] + b[i];
                } else {  // mid / side
                    const i64 side = b[i];
                    const i64 mid = (a[i] * 2) | (side & 1);
                    a[i] = (mid + side) >> 1;
                    b[i] = (mid - side) >> 1;
                }
            }
        }

        int take = blockSize;
        if (si.totalSamples) take = static_cast<int>(std::min<u64>(static_cast<u64>(blockSize), si.totalSamples - out.frames));
        const std::size_t base = out.interleaved.size();
        out.interleaved.resize(base + static_cast<std::size_t>(take) * channels);
        float* dst = out.interleaved.data() + base;
        for (int i = 0; i < take; ++i)
            for (int c = 0; c < channels; ++c) *dst++ = static_cast<float>(ch[c][i] * scale);

        if (md5Present) {
            const int bytesPer = (bps + 7) / 8;
            md5Buf.resize(static_cast<std::size_t>(take) * channels * bytesPer);
            u8* m = md5Buf.data();
            for (int i = 0; i < take; ++i)
                for (int c = 0; c < channels; ++c) {
                    const u64 v = static_cast<u64>(ch[c][i]);
                    for (int k = 0; k < bytesPer; ++k) *m++ = static_cast<u8>(v >> (8 * k));
                }
            md5.update(md5Buf.data(), md5Buf.size());
        }
        out.frames += static_cast<u64>(take);
    }
    if (out.frames == 0) return fail("no audio frames decoded");
    if (md5Present) {
        if (md5.finish() != si.md5) return fail("MD5 mismatch: decoded audio does not match the encoder's checksum");
        out.md5Verified = true;
    }
    return out;
}

}  // namespace whe
