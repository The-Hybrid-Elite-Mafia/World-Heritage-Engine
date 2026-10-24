// Hybrid DSP hooks: physical-modeling micro-engines that ONLY shape real
// recorded audio. Each one is an input-driven system:
//
//   * it has no oscillator, noise generator, or self-starting state;
//   * with zero input from a reset state it outputs exact zeros;
//   * every feedback loop is passive (loop gain <= 1 by construction), so
//     when the recording stops feeding it, it decays to silence.
//
// The test suite checks these properties for every class in this file.
#pragma once

#include <array>
#include <complex>

#include "whe/dsp.hpp"
#include "whe/sample_library.hpp"
#include "whe/simd.hpp"

namespace whe::hybrid {

// ---------------------------------------------------------------------------
// Waveguide string (extended Karplus–Strong). Excited by the transient of a
// real recording (piano hammer, guitar pick, harp finger) it adds the
// physically modeled string resonance on top of the recorded note.
// ---------------------------------------------------------------------------
class StringResonator {
public:
    void prepare(float sampleRate, float lowestHz);
    void reset() noexcept;
    void setFrequency(float hz) noexcept;
    void setDecay(float t60Seconds) noexcept;
    void setBrightness(float b) noexcept;     // 0 dark … 1 bright (loop low-pass)
    void setStiffness(float s) noexcept;      // 0 ideal string … 1 stiff (allpass dispersion, sharp partials)
    void setExcitePosition(float p) noexcept; // 0 … 0.5 of string length (comb on the excitation)
    float process(float x) noexcept;
    [[nodiscard]] float frequency() const noexcept { return hz_; }

private:
    void retune() noexcept;
    dsp::DelayLine loop_, comb_;
    float fs_ = 48000, hz_ = 220, t60_ = 4, bright_ = 0.6f, stiff_ = 0, pos_ = 0.13f;
    float delay_ = 100, g_ = 0.99f, lpB_ = 0.3f, lpY_ = 0, apC_ = 0, combDelay_ = 0;
    std::array<float, 4> apX1_{}, apY1_{};
};

// Bank of open strings that resonate in sympathy with whatever the instrument plays.
class SympatheticBank {
public:
    void prepare(float sampleRate, std::span<const int> midiNotes, float t60Seconds);
    void reset() noexcept;
    void setDecay(float t60Seconds) noexcept;
    // out[i] = sympathetic response to in[i] (replaces out).
    void process(const float* in, float* out, int n) noexcept;

private:
    std::vector<StringResonator> strings_;
    float norm_ = 1.0f;
};

// ---------------------------------------------------------------------------
// Bow–string friction shaper. A bidirectional waveguide tuned to the note,
// with a pressure-dependent stick/slip friction junction (STK-style bow table
// form). The REAL multi-sampled bowed recording is fed in as the bow-velocity
// signal, so bow pressure, bow position and rosin grab re-articulate the
// recording's own harmonics. It cannot self-oscillate: the scattering matrix
// at the bow has norm <= 1 and both terminations lose energy.
// ---------------------------------------------------------------------------
class BowFrictionShaper {
public:
    void prepare(float sampleRate, float lowestHz);
    void reset() noexcept;
    void setFrequency(float hz) noexcept;
    void setBowPosition(float beta) noexcept;  // 0.02 … 0.5 (fraction from bridge)
    void setPressure(float p) noexcept;        // 0 … 1
    void setRosin(float r) noexcept;           // 0 … 1 (wider stick region)
    void setDecay(float t60Seconds) noexcept;
    float process(float bowVelocityFromRecording) noexcept;

private:
    void retune() noexcept;
    dsp::DelayLine neck_, bridge_;
    dsp::OnePole bridgeLp_;
    float fs_ = 48000, hz_ = 220, beta_ = 0.12f, slope_ = 3.0f, offset_ = 0.75f, t60_ = 1.5f;
    float neckDelay_ = 50, bridgeDelay_ = 10, loss_ = 0.995f;
};

// ---------------------------------------------------------------------------
// Air-column (bore) shaper for aerophones. A bore waveguide whose length is a
// live, sculptable parameter, with:
//   * bell diffraction: reflected low band / radiated high band split at the
//     ka = 1 transition (fc = c / (2·pi·bellRadius));
//   * overblowing: a register loop at 1/2 (open bore) or 1/3 (closed bore)
//     of the bore takes over the excitation as overblow rises;
//   * turbulence: the recording's own breath noise (high band) is emphasised;
//   * tonguing: the recording's own articulation transient is emphasised;
//   * a passive tanh(k·s)/k pressure nonlinearity (slope <= 1).
// ---------------------------------------------------------------------------
class AirColumnShaper {
public:
    enum class Bore { Open, Closed };  // open–open (flute) / closed–open (clarinet)
    void prepare(float sampleRate, float lowestHz);
    void reset() noexcept;
    void setBore(Bore b) noexcept;
    void setBoreLength(float meters) noexcept;
    void setBellRadius(float meters) noexcept;
    void setOverblow(float o) noexcept;
    void setTurbulence(float t) noexcept;
    void setTonguing(float t) noexcept;
    void setNonlinearity(float n) noexcept;
    void setDecay(float t60Seconds) noexcept;
    void noteOn() noexcept { tongue_.trigger(); }
    float process(float recordedBreath) noexcept;
    [[nodiscard]] static float boreLengthFor(float hz, Bore b) noexcept {
        return static_cast<float>(b == Bore::Open ? kSpeedOfSound / (2.0 * hz) : kSpeedOfSound / (4.0 * hz));
    }

private:
    void retune() noexcept;
    struct Loop {
        dsp::DelayLine line;
        dsp::OnePole bell;
        float delay = 100;
    };
    float runLoop(Loop& l, float in, float sign) noexcept;
    Loop fund_, reg_;
    dsp::OnePole breathHp_;
    dsp::TransientExtractor tongue_;
    Bore bore_ = Bore::Open;
    float fs_ = 48000, length_ = 0.3f, bellRadius_ = 0.03f, overblow_ = 0, turbulence_ = 0.3f, tonguing_ = 0.4f;
    float k_ = 2.0f, t60_ = 0.4f, loss_ = 0.99f;
};

// ---------------------------------------------------------------------------
// 2-D membrane (FDTD of the damped wave equation, the finite-difference twin of
// a rectilinear digital waveguide mesh) with a circular drumhead mask. ONE
// mesh per drum instrument: every recorded hit excites the same head.
//   tension        -> Courant number (wave speed), live and click-free
//   damping        -> frequency-independent loss (T60)
//   highDamping    -> frequency-dependent loss (upper modes die first)
//   strike x/y     -> where the recording's transient is injected
//   mallet hardness-> low-pass on the injected transient (contact time)
// Stability: lambda^2 + 2·kappa <= 0.5 is enforced (von Neumann bound).
// ---------------------------------------------------------------------------
class MembraneMesh {
public:
    void prepare(float sampleRate, int cellsPerSide = 24);
    void reset() noexcept;
    void setTension(float t) noexcept;       // 0 … 1
    void setDamping(float t60Seconds) noexcept;
    void setHighDamping(float h) noexcept;   // 0 … 1
    void setStrike(float x, float y) noexcept;  // -1 … 1 across the head
    void setMalletHardness(float h) noexcept;   // 0 … 1
    // out = membrane response to the injected recording transient (replaces out).
    void process(const float* in, float* out, int n) noexcept;
    [[nodiscard]] float fundamentalEstimateHz() const noexcept;

private:
    void updateCoefficients() noexcept;
    int n_ = 24, stride_ = 64;
    AlignedBuffer<float> u_[3];
    AlignedBuffer<float> mask_;
    int cur_ = 0;
    float fs_ = 48000, tension_ = 0.4f, t60_ = 0.6f, high_ = 0.3f, hard_ = 0.6f;
    float A_ = 0, B_ = 0, C_ = 0, D_ = 0;
    std::array<int, 5> strikeIdx_{};
    std::array<float, 5> strikeW_{};
    std::array<int, 3> pickIdx_{};
    dsp::OnePole mallet_;
    float outGain_ = 1.0f;
};

// ---------------------------------------------------------------------------
// Resonant body re-voicing. The recording already contains the real body; to
// *change* the wood we subtract the body's estimated modes at their measured
// frequencies and add them back at sculpted frequencies:
//     out = in + amount * (Body_sculpted(in) - Body_reference(in))
// Plate modes scale as f ∝ sqrt(E / rho), so density and stiffness move every
// mode coherently. At reference settings the two banks are identical and the
// difference is exactly zero: the recording passes untouched.
// ---------------------------------------------------------------------------
struct BodyMode {
    float hz = 0;
    float gain = 0;
    float t60 = 0.12f;
};

class BodyRevoicer {
public:
    void prepare(float sampleRate, std::span<const BodyMode> modes);
    void reset() noexcept;
    void setMaterial(float densityRatio, float stiffnessRatio) noexcept;  // rho/rho0, E/E0
    void setAmount(float a) noexcept { amount_ = a; }
    void setDampingScale(float s) noexcept;
    // In-place on a mono signal; returns the correction that was added.
    void process(float* mono, float* correction, int n) noexcept;
    [[nodiscard]] bool active() const noexcept { return !modes_.empty(); }

private:
    void design(ModalBankState& bank, float freqScale) noexcept;
    std::vector<BodyMode> modes_;
    ModalBankState reference_, sculpted_;
    AlignedBuffer<float> tmp_;
    float fs_ = 48000, density_ = 1, stiffness_ = 1, damping_ = 1, amount_ = 1, lastScale_ = -1;
};

// Estimates body modes from the library's own recordings: the geometric mean
// of normalised long-term spectra across many different notes keeps the peaks
// every note shares (the body) and suppresses note-specific harmonics.
// This is an approximation, not a laboratory modal analysis.
[[nodiscard]] std::vector<BodyMode> estimateBodyModes(const SampleLibrary& lib, int maxModes = 24, float loHz = 70, float hiHz = 4000);

}  // namespace whe::hybrid
