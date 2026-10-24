#include "whe/hybrid.hpp"

#include <map>

namespace whe::hybrid {

namespace {

// Phase delay (samples) of y = (1-b)x + b*y1 at radian frequency w.
double onePolePhaseDelay(double b, double w) { return std::atan2(b * std::sin(w), 1.0 - b * std::cos(w)) / w; }
double onePoleMagnitude(double b, double w) { return (1.0 - b) / std::abs(std::complex<double>(1.0 - b * std::cos(w), b * std::sin(w))); }
// Phase delay of first-order allpass (c + z^-1)/(1 + c z^-1).
double allpassPhaseDelay(double c, double w) {
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> h = (c + z1) / (1.0 + c * z1);
    double ph = std::arg(h);
    if (ph > 0) ph -= 2.0 * std::numbers::pi;
    return -ph / w;
}
float t60ToLoopGain(float t60, float hz) { return std::pow(10.0f, -3.0f / (std::max(t60, 0.01f) * std::max(hz, 1.0f))); }

}  // namespace

// ============================================================================
// StringResonator
// ============================================================================
void StringResonator::prepare(float sampleRate, float lowestHz) {
    fs_ = sampleRate;
    const int maxDelay = static_cast<int>(sampleRate / std::max(lowestHz, 10.0f)) + 16;
    loop_.prepare(maxDelay);
    comb_.prepare(maxDelay);
    retune();
    reset();
}

void StringResonator::reset() noexcept {
    loop_.reset();
    comb_.reset();
    lpY_ = 0;
    apX1_.fill(0);
    apY1_.fill(0);
}

void StringResonator::setFrequency(float hz) noexcept {
    hz_ = clampf(hz, 10.0f, 0.4f * fs_);
    retune();
}
void StringResonator::setDecay(float t) noexcept {
    t60_ = t;
    retune();
}
void StringResonator::setBrightness(float b) noexcept {
    bright_ = clampf(b, 0, 1);
    retune();
}
void StringResonator::setStiffness(float s) noexcept {
    stiff_ = clampf(s, 0, 1);
    retune();
}
void StringResonator::setExcitePosition(float p) noexcept {
    pos_ = clampf(p, 0, 0.5f);
    retune();
}

void StringResonator::retune() noexcept {
    const double w = 2.0 * std::numbers::pi * hz_ / fs_;
    lpB_ = 0.85f - 0.8f * bright_;
    apC_ = -0.7f * stiff_;
    double d = fs_ / hz_ - onePolePhaseDelay(lpB_, w);
    if (stiff_ > 0.0f) d -= 4.0 * allpassPhaseDelay(apC_, w);
    delay_ = static_cast<float>(std::clamp(d, 2.0, static_cast<double>(loop_.capacity() - 1)));
    const double mag = onePoleMagnitude(lpB_, w);
    g_ = static_cast<float>(std::min(0.9995, t60ToLoopGain(t60_, hz_) / mag));
    combDelay_ = pos_ > 0.01f ? std::max(2.0f, pos_ * fs_ / hz_) : 0.0f;
}

float StringResonator::process(float x) noexcept {
    float e = x;
    if (combDelay_ > 0.0f) {
        e = x - comb_.read(combDelay_);
        comb_.write(x);
    }
    float s = loop_.read(delay_);
    if (stiff_ > 0.0f) {
        for (int k = 0; k < 4; ++k) {
            const float y = apC_ * s + apX1_[k] - apC_ * apY1_[k];
            apX1_[k] = s;
            apY1_[k] = y;
            s = y;
        }
    }
    lpY_ = (1.0f - lpB_) * s + lpB_ * lpY_;
    const float v = e + g_ * lpY_;
    loop_.write(v);
    return v;
}

// ============================================================================
// SympatheticBank
// ============================================================================
void SympatheticBank::prepare(float sampleRate, std::span<const int> notes, float t60) {
    strings_.clear();
    strings_.resize(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const float hz = static_cast<float>(midiToHz(notes[i]));
        strings_[i].prepare(sampleRate, hz * 0.9f);
        strings_[i].setBrightness(0.45f);
        strings_[i].setExcitePosition(0.0f);
        strings_[i].setFrequency(hz);
        strings_[i].setDecay(t60);
    }
    norm_ = strings_.empty() ? 0.0f : 1.0f / std::sqrt(static_cast<float>(strings_.size()));
}
void SympatheticBank::reset() noexcept {
    for (auto& s : strings_) s.reset();
}
void SympatheticBank::setDecay(float t60) noexcept {
    for (auto& s : strings_) s.setDecay(t60);
}
void SympatheticBank::process(const float* in, float* out, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        float acc = 0.0f;
        for (auto& s : strings_) acc += s.process(in[i]);
        out[i] = acc * norm_;
    }
}

// ============================================================================
// BowFrictionShaper
// ============================================================================
void BowFrictionShaper::prepare(float sampleRate, float lowestHz) {
    fs_ = sampleRate;
    const int maxDelay = static_cast<int>(sampleRate / std::max(lowestHz, 10.0f)) + 16;
    neck_.prepare(maxDelay);
    bridge_.prepare(maxDelay);
    bridgeLp_.setCutoff(0.22f * sampleRate, sampleRate);
    retune();
    reset();
}
void BowFrictionShaper::reset() noexcept {
    neck_.reset();
    bridge_.reset();
    bridgeLp_.reset();
}
void BowFrictionShaper::setFrequency(float hz) noexcept {
    hz_ = clampf(hz, 10.0f, 0.3f * fs_);
    retune();
}
void BowFrictionShaper::setBowPosition(float b) noexcept {
    beta_ = clampf(b, 0.02f, 0.5f);
    retune();
}
void BowFrictionShaper::setPressure(float p) noexcept { slope_ = 5.0f - 4.0f * clampf(p, 0, 1); }
void BowFrictionShaper::setRosin(float r) noexcept { offset_ = 0.95f - 0.5f * clampf(r, 0, 1); }
void BowFrictionShaper::setDecay(float t) noexcept {
    t60_ = t;
    retune();
}
void BowFrictionShaper::retune() noexcept {
    const float L = std::max(6.0f, fs_ / hz_ - 2.0f);
    bridgeDelay_ = std::max(2.0f, beta_ * L);
    neckDelay_ = std::max(2.0f, L - bridgeDelay_);
    loss_ = std::sqrt(t60ToLoopGain(t60_, hz_));  // applied at both terminations
}

float BowFrictionShaper::process(float vBow) noexcept {
    const float bridgeOut = bridge_.read(bridgeDelay_);
    const float neckOut = neck_.read(neckDelay_);
    const float bridgeRefl = -loss_ * bridgeLp_.lp(bridgeOut);
    const float nutRefl = -loss_ * neckOut;
    const float vString = bridgeRefl + nutRefl;
    const float dv = vBow - vString;
    const float z = std::fabs(dv * slope_) + offset_;
    const float z2 = z * z;
    const float r = std::min(1.0f, 1.0f / (z2 * z2));  // stick (r = 1) … slip (r -> 0)
    const float f = dv * r;
    neck_.write(bridgeRefl + f);
    bridge_.write(nutRefl + f);
    return bridgeOut;
}

// ============================================================================
// AirColumnShaper
// ============================================================================
void AirColumnShaper::prepare(float sampleRate, float lowestHz) {
    fs_ = sampleRate;
    // Longest bore: closed bore at lowest pitch, times the 2x sculpt range.
    const float maxLen = boreLengthFor(std::max(lowestHz, 10.0f), Bore::Open) * 2.5f;
    const int maxDelay = static_cast<int>(2.0f * maxLen / static_cast<float>(kSpeedOfSound) * sampleRate) + 16;
    fund_.line.prepare(maxDelay);
    reg_.line.prepare(maxDelay);
    breathHp_.setCutoff(2500.0f, sampleRate);
    tongue_.prepare(sampleRate);
    tongue_.setWindow(15.0f);
    retune();
    reset();
}
void AirColumnShaper::reset() noexcept {
    fund_.line.reset();
    reg_.line.reset();
    fund_.bell.reset();
    reg_.bell.reset();
    breathHp_.reset();
    tongue_.reset();
}
void AirColumnShaper::setBore(Bore b) noexcept {
    bore_ = b;
    retune();
}
void AirColumnShaper::setBoreLength(float m) noexcept {
    length_ = clampf(m, 0.02f, 20.0f);
    retune();
}
void AirColumnShaper::setBellRadius(float m) noexcept {
    bellRadius_ = clampf(m, 0.002f, 0.5f);
    retune();
}
void AirColumnShaper::setOverblow(float o) noexcept { overblow_ = clampf(o, 0, 1); }
void AirColumnShaper::setTurbulence(float t) noexcept { turbulence_ = clampf(t, 0, 1); }
void AirColumnShaper::setTonguing(float t) noexcept { tonguing_ = clampf(t, 0, 1); }
void AirColumnShaper::setNonlinearity(float n) noexcept { k_ = 1.0f + 9.0f * clampf(n, 0, 1); }
void AirColumnShaper::setDecay(float t) noexcept {
    t60_ = t;
    retune();
}
void AirColumnShaper::retune() noexcept {
    const float roundTrip = 2.0f * length_ / static_cast<float>(kSpeedOfSound) * fs_;
    fund_.delay = clampf(roundTrip, 2.0f, static_cast<float>(fund_.line.capacity() - 1));
    reg_.delay = clampf(roundTrip / (bore_ == Bore::Open ? 2.0f : 3.0f), 2.0f, static_cast<float>(reg_.line.capacity() - 1));
    const float fc = static_cast<float>(kSpeedOfSound / (2.0 * std::numbers::pi * bellRadius_));  // ka = 1
    fund_.bell.setCutoff(fc, fs_);
    reg_.bell.setCutoff(fc, fs_);
    loss_ = std::pow(10.0f, -3.0f * fund_.delay / (std::max(t60_, 0.01f) * fs_));
}
float AirColumnShaper::runLoop(Loop& l, float in, float sign) noexcept {
    const float p = l.line.read(l.delay);
    const float low = l.bell.lp(p);
    const float radiated = p - low;  // high band leaves the bell
    float s = in + sign * loss_ * low;  // low band reflects back up the bore
    s = std::tanh(k_ * s) / k_;
    l.line.write(s);
    return radiated;
}
float AirColumnShaper::process(float x) noexcept {
    const float exc = x + 2.0f * turbulence_ * breathHp_.hp(x) + 2.0f * tonguing_ * tongue_.process(x);
    const float sign = bore_ == Bore::Open ? 1.0f : -1.0f;
    return runLoop(fund_, exc * (1.0f - overblow_), sign) + runLoop(reg_, exc * overblow_, sign);
}

// ============================================================================
// MembraneMesh
// ============================================================================
void MembraneMesh::prepare(float sampleRate, int cells) {
    fs_ = sampleRate;
    n_ = std::clamp(cells, 8, 96);
    stride_ = ((n_ + 2 + 15) & ~15) + 16;
    const std::size_t size = static_cast<std::size_t>(stride_) * static_cast<std::size_t>(n_ + 2);
    for (auto& b : u_) b.resize(size);
    mask_.resize(size);
    const double c = (n_ + 1) / 2.0, R = n_ / 2.0;
    for (int r = 1; r <= n_; ++r)
        for (int col = 1; col <= n_; ++col) {
            const double dx = (col - c) / R, dy = (r - c) / R;
            mask_[static_cast<std::size_t>(r * stride_ + col)] = (dx * dx + dy * dy <= 1.0) ? 1.0f : 0.0f;
        }
    auto cell = [&](double fx, double fy) {
        int col = static_cast<int>(std::lround(c + fx * R));
        int row = static_cast<int>(std::lround(c + fy * R));
        col = std::clamp(col, 1, n_);
        row = std::clamp(row, 1, n_);
        return row * stride_ + col;
    };
    pickIdx_ = {cell(0.1, -0.15), cell(-0.45, 0.3), cell(0.5, 0.42)};
    setStrike(0.3f, 0.0f);
    mallet_.setCutoff(6000.0f, sampleRate);
    updateCoefficients();
    reset();
}
void MembraneMesh::reset() noexcept {
    for (auto& b : u_) b.zero();
    cur_ = 0;
    mallet_.reset();
}
void MembraneMesh::setTension(float t) noexcept {
    tension_ = clampf(t, 0, 1);
    updateCoefficients();
}
void MembraneMesh::setDamping(float t60) noexcept {
    t60_ = std::max(0.02f, t60);
    updateCoefficients();
}
void MembraneMesh::setHighDamping(float h) noexcept {
    high_ = clampf(h, 0, 1);
    updateCoefficients();
}
void MembraneMesh::setMalletHardness(float h) noexcept {
    hard_ = clampf(h, 0, 1);
    mallet_.setCutoff(500.0f * std::pow(24.0f, hard_), fs_);  // 500 Hz (felt) … 12 kHz (wood stick)
}
void MembraneMesh::setStrike(float x, float y) noexcept {
    const double c = (n_ + 1) / 2.0, R = n_ / 2.0 * 0.85;
    double fx = clampf(x, -1, 1), fy = clampf(y, -1, 1);
    const double len = std::sqrt(fx * fx + fy * fy);
    if (len > 1.0) {
        fx /= len;
        fy /= len;
    }
    const int col = std::clamp(static_cast<int>(std::lround(c + fx * R)), 2, n_ - 1);
    const int row = std::clamp(static_cast<int>(std::lround(c + fy * R)), 2, n_ - 1);
    const int i = row * stride_ + col;
    strikeIdx_ = {i, i - 1, i + 1, i - stride_, i + stride_};
    strikeW_ = {0.5f, 0.125f, 0.125f, 0.125f, 0.125f};
}
void MembraneMesh::updateCoefficients() noexcept {
    const float kappa = 0.02f * high_;
    const float l2max = 0.5f - 2.0f * kappa - 0.005f;  // von Neumann stability bound
    const float l2 = 0.01f + tension_ * (l2max - 0.01f);
    const float q = std::pow(10.0f, -6.0f / (t60_ * fs_));
    const float sigma = (1.0f - q) / (1.0f + q);
    A_ = 2.0f / (1.0f + sigma);
    B_ = -(1.0f - sigma) / (1.0f + sigma);
    C_ = (l2 + kappa) / (1.0f + sigma);
    D_ = -kappa / (1.0f + sigma);
    outGain_ = 3.0f;
}
float MembraneMesh::fundamentalEstimateHz() const noexcept {
    // Lowest circular-membrane mode: f01 = 2.405·c / (2·pi·a), c = lambda cells/sample, a = n/2 cells.
    const float kappa = 0.02f * high_;
    const float l2 = 0.01f + tension_ * (0.5f - 2.0f * kappa - 0.005f - 0.01f);
    return static_cast<float>(2.405 * std::sqrt(l2) * fs_ / (std::numbers::pi * n_));
}
void MembraneMesh::process(const float* in, float* out, int n) noexcept {
    const SimdKernels& k = simd();
    int prev = (cur_ + 2) % 3;
    for (int t = 0; t < n; ++t) {
        const float e = mallet_.lp(in[t]);
        float* cur = u_[cur_].data();
        if (e != 0.0f)
            for (std::size_t j = 0; j < strikeIdx_.size(); ++j) cur[strikeIdx_[j]] += e * strikeW_[j] * mask_[static_cast<std::size_t>(strikeIdx_[j])];
        const int next = 3 - cur_ - prev;
        MeshStepArgs a{u_[next].data(), cur, u_[prev].data(), mask_.data(), n_, stride_, A_, B_, C_, D_};
        k.meshStep(a);
        // Radiated sound follows surface velocity, not displacement: pick up du/dt.
        const float* nx = u_[next].data();
        out[t] = ((nx[pickIdx_[0]] - cur[pickIdx_[0]]) + (nx[pickIdx_[1]] - cur[pickIdx_[1]]) + (nx[pickIdx_[2]] - cur[pickIdx_[2]])) *
                 (outGain_ / 3.0f);
        prev = cur_;
        cur_ = next;
    }
}

// ============================================================================
// BodyRevoicer
// ============================================================================
void BodyRevoicer::prepare(float sampleRate, std::span<const BodyMode> modes) {
    fs_ = sampleRate;
    modes_.assign(modes.begin(), modes.end());
    reference_.allocate(static_cast<int>(modes_.size()));
    sculpted_.allocate(static_cast<int>(modes_.size()));
    tmp_.resize(kMaxBlock);
    design(reference_, 1.0f);
    lastScale_ = -1.0f;
    setMaterial(density_, stiffness_);
    reset();
}
void BodyRevoicer::reset() noexcept {
    reference_.y1.zero();
    reference_.y2.zero();
    sculpted_.y1.zero();
    sculpted_.y2.zero();
}
void BodyRevoicer::design(ModalBankState& bank, float scale) noexcept {
    for (int i = 0; i < bank.padded; ++i) {
        if (i >= static_cast<int>(modes_.size())) {
            bank.a1[i] = bank.a2[i] = bank.gain[i] = 0.0f;
            continue;
        }
        const BodyMode& m = modes_[static_cast<std::size_t>(i)];
        const float f = clampf(m.hz * scale, 20.0f, 0.45f * fs_);
        const float r = std::pow(10.0f, -3.0f / (std::max(0.005f, m.t60 * damping_) * fs_));
        const float th = dsp::kTwoPi * f / fs_;
        bank.a1[i] = 2.0f * r * std::cos(th);
        bank.a2[i] = r * r;
        bank.gain[i] = m.gain * (1.0f - r) * 2.0f * std::sin(th);
    }
}
void BodyRevoicer::setMaterial(float densityRatio, float stiffnessRatio) noexcept {
    density_ = std::max(0.05f, densityRatio);
    stiffness_ = std::max(0.05f, stiffnessRatio);
    const float scale = std::sqrt(stiffness_ / density_);  // plate modes: f ∝ sqrt(E / rho)
    if (scale != lastScale_) {
        design(sculpted_, scale);
        lastScale_ = scale;
    }
}
void BodyRevoicer::setDampingScale(float s) noexcept {
    if (s == damping_) return;
    damping_ = std::max(0.05f, s);
    design(reference_, 1.0f);
    design(sculpted_, lastScale_ > 0 ? lastScale_ : 1.0f);
}
void BodyRevoicer::process(float* mono, float* correction, int n) noexcept {
    if (modes_.empty() || lastScale_ == 1.0f || amount_ == 0.0f) {
        std::memset(correction, 0, sizeof(float) * static_cast<std::size_t>(n));
        return;
    }
    const SimdKernels& k = simd();
    k.modalBank(sculpted_, mono, correction, n);
    k.modalBank(reference_, mono, tmp_.data(), n);
    for (int i = 0; i < n; ++i) {
        correction[i] = amount_ * (correction[i] - tmp_[i]);
        mono[i] += correction[i];
    }
}

// ============================================================================
// Body mode estimation from the recordings themselves
// ============================================================================
namespace {
void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -dsp::kTwoPi / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
}  // namespace

std::vector<BodyMode> estimateBodyModes(const SampleLibrary& lib, int maxModes, float loHz, float hiHz) {
    const Articulation* art = lib.primary();
    if (!art || art->zones.empty()) return {};
    constexpr int N = 4096;
    std::vector<float> win(N);
    for (int i = 0; i < N; ++i) win[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(dsp::kTwoPi * i / (N - 1));

    // One recording per distinct root note (loudest layer is most informative), up to 32.
    std::map<int, u32> perRoot;
    for (const Zone& z : art->zones) perRoot[z.rootKey] = z.roundRobins.front();
    std::vector<u32> picks;
    for (auto& [_, id] : perRoot) picks.push_back(id);
    if (picks.size() > 32) {
        std::vector<u32> thin;
        for (std::size_t i = 0; i < 32; ++i) thin.push_back(picks[i * picks.size() / 32]);
        picks = std::move(thin);
    }

    std::vector<double> logAvg(N / 2, 0.0);
    double sampleRate = 0;
    int used = 0;
    std::vector<std::complex<float>> buf(N);
    for (u32 id : picks) {
        const StoredSample& s = lib.store().get(id);
        sampleRate = s.header.sampleRate;
        const u32 C = s.channels();
        const u64 frames = std::min<u64>(s.frameCount(), static_cast<u64>(2.0 * s.header.sampleRate));
        if (frames < N) continue;
        std::vector<double> power(N / 2, 0.0);
        int segs = 0;
        for (u64 start = 0; start + N <= frames; start += N / 2, ++segs) {
            for (int i = 0; i < N; ++i) {
                float m = 0;
                for (u32 c = 0; c < C; ++c) m += s.frames[(start + static_cast<u64>(i)) * C + c];
                buf[static_cast<std::size_t>(i)] = {m / static_cast<float>(C) * win[static_cast<std::size_t>(i)], 0.0f};
            }
            fft(buf);
            for (int k = 0; k < N / 2; ++k) power[static_cast<std::size_t>(k)] += std::norm(buf[static_cast<std::size_t>(k)]);
        }
        double mean = 0;
        for (double p : power) mean += p;
        mean /= power.size();
        if (mean <= 0) continue;
        for (int k = 0; k < N / 2; ++k) logAvg[static_cast<std::size_t>(k)] += std::log(power[static_cast<std::size_t>(k)] / mean + 1e-12);
        ++used;
    }
    if (used == 0 || sampleRate <= 0) return {};
    for (auto& v : logAvg) v /= used;

    const double binHz = sampleRate / N;
    const int lo = std::max(3, static_cast<int>(loHz / binHz)), hi = std::min(N / 2 - 25, static_cast<int>(hiHz / binHz));
    struct Peak { double hz, prominenceDb; };
    std::vector<Peak> peaks;
    for (int k = lo; k <= hi; ++k) {
        const double v = logAvg[static_cast<std::size_t>(k)];
        bool isMax = true;
        for (int d = -2; d <= 2 && isMax; ++d)
            if (d != 0 && logAvg[static_cast<std::size_t>(k + d)] > v) isMax = false;
        if (!isMax) continue;
        std::vector<double> nb;
        for (int d = -20; d <= 20; ++d)
            if (k + d >= 0 && k + d < N / 2) nb.push_back(logAvg[static_cast<std::size_t>(k + d)]);
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
        const double prom = (v - nb[nb.size() / 2]) * 10.0 / std::log(10.0);  // natural-log power -> dB
        if (prom < 1.5) continue;
        // parabolic interpolation of the peak bin
        const double a = logAvg[static_cast<std::size_t>(k - 1)], b = v, c = logAvg[static_cast<std::size_t>(k + 1)];
        const double den = a - 2 * b + c;
        const double off = den != 0 ? 0.5 * (a - c) / den : 0.0;
        peaks.push_back({(k + std::clamp(off, -0.5, 0.5)) * binHz, prom});
    }
    std::ranges::sort(peaks, [](const Peak& x, const Peak& y) { return x.prominenceDb > y.prominenceDb; });
    std::vector<BodyMode> modes;
    for (const auto& p : peaks) {
        if (static_cast<int>(modes.size()) >= maxModes) break;
        bool tooClose = false;
        for (const auto& m : modes)
            if (std::fabs(m.hz - p.hz) / m.hz < 0.03) tooClose = true;
        if (tooClose) continue;
        modes.push_back(BodyMode{static_cast<float>(p.hz), static_cast<float>(std::clamp(p.prominenceDb / 12.0, 0.05, 1.0) * 0.25),
                                 p.hz < 500 ? 0.12f : 0.06f});
    }
    std::ranges::sort(modes, {}, &BodyMode::hz);
    return modes;
}

}  // namespace whe::hybrid
