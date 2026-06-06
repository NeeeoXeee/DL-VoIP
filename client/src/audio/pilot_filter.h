#pragma once

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/// DSP chain simulating a military fighter pilot radio voice effect.
/// Apply only to the receive (playback) path — never to the capture path.
///
/// Chain order:
///   1. Oxygen-mask resonance  — peaking EQ +4 dB at 550 Hz
///   2. Military bandpass      — HPF 400 Hz  +  LPF 2500 Hz
///   3. Mild saturation        — soft tanh waveshaper (drive = 1.5)
///   4. Dynamic leveller       — compressor 5:1, threshold -24 dB
///
/// All state is maintained internally; call reset() when toggling the
/// filter on so old history doesn't produce a transient pop.
class PilotFilter {
public:
    static constexpr float kSampleRate = 48000.0f;

    PilotFilter() {
        computeCoefficients();
        reset();
    }

    void reset() {
        for (int i = 0; i < kNumFilters; ++i)
            x1[i] = x2[i] = y1[i] = y2[i] = 0.0f;
        m_envelope  = 0.0f;
        m_gainSmooth = 1.0f;
    }

    /// Process buffer in-place (mono float32).
    void process(float* buf, int frames) {
        for (int i = 0; i < frames; ++i) {
            float s = buf[i];
            s = applyBiquad(s, 0); // peaking EQ
            s = applyBiquad(s, 1); // HPF
            s = applyBiquad(s, 2); // LPF
            s = applySaturation(s);
            s = applyCompressor(s);
            buf[i] = s;
        }
    }

private:
    static constexpr int   kNumFilters    = 3;
    static constexpr float kSatDrive      = 1.5f;
    static constexpr float kThresholdDb   = -24.0f;
    static constexpr float kRatio         = 5.0f;
    static constexpr float kAttackMs      = 5.0f;
    static constexpr float kReleaseMs     = 100.0f;

    // Biquad coefficients
    float b0[kNumFilters]{}, b1[kNumFilters]{}, b2[kNumFilters]{};
    float a1[kNumFilters]{}, a2[kNumFilters]{};
    // Biquad delay-line state
    float x1[kNumFilters]{}, x2[kNumFilters]{};
    float y1[kNumFilters]{}, y2[kNumFilters]{};

    // Compressor state
    float m_envelope  = 0.0f;
    float m_gainSmooth = 1.0f;
    float m_attackCoef  = 0.0f;
    float m_releaseCoef = 0.0f;

    void computeCoefficients() {
        // ----------------------------------------------------------------
        // Filter 0: Peaking EQ — 550 Hz, +4 dB, Q = 1.0
        // ----------------------------------------------------------------
        {
            constexpr float f0    = 550.0f;
            constexpr float dBg   = 4.0f;
            constexpr float Q     = 1.0f;
            float A     = std::pow(10.0f, dBg / 40.0f);
            float w0    = 2.0f * (float)M_PI * f0 / kSampleRate;
            float sinw0 = std::sin(w0);
            float cosw0 = std::cos(w0);
            float alpha = sinw0 / (2.0f * Q);
            float a0    = 1.0f + alpha / A;
            b0[0] = (1.0f + alpha * A)  / a0;
            b1[0] = (-2.0f * cosw0)     / a0;
            b2[0] = (1.0f - alpha * A)  / a0;
            a1[0] = (-2.0f * cosw0)     / a0;
            a2[0] = (1.0f - alpha / A)  / a0;
        }

        // ----------------------------------------------------------------
        // Filter 1: High-pass — 400 Hz, Butterworth (Q = 0.707)
        // ----------------------------------------------------------------
        {
            constexpr float f0    = 400.0f;
            constexpr float Q     = 0.7071f;
            float w0    = 2.0f * (float)M_PI * f0 / kSampleRate;
            float sinw0 = std::sin(w0);
            float cosw0 = std::cos(w0);
            float alpha = sinw0 / (2.0f * Q);
            float a0    = 1.0f + alpha;
            b0[1] = ((1.0f + cosw0) * 0.5f)  / a0;
            b1[1] = (-(1.0f + cosw0))          / a0;
            b2[1] = ((1.0f + cosw0) * 0.5f)  / a0;
            a1[1] = (-2.0f * cosw0)           / a0;
            a2[1] = (1.0f - alpha)             / a0;
        }

        // ----------------------------------------------------------------
        // Filter 2: Low-pass — 2500 Hz, Butterworth (Q = 0.707)
        // ----------------------------------------------------------------
        {
            constexpr float f0    = 2500.0f;
            constexpr float Q     = 0.7071f;
            float w0    = 2.0f * (float)M_PI * f0 / kSampleRate;
            float sinw0 = std::sin(w0);
            float cosw0 = std::cos(w0);
            float alpha = sinw0 / (2.0f * Q);
            float a0    = 1.0f + alpha;
            b0[2] = ((1.0f - cosw0) * 0.5f) / a0;
            b1[2] = (1.0f - cosw0)           / a0;
            b2[2] = ((1.0f - cosw0) * 0.5f) / a0;
            a1[2] = (-2.0f * cosw0)          / a0;
            a2[2] = (1.0f - alpha)            / a0;
        }

        // Compressor time constants
        m_attackCoef  = std::exp(-1.0f / (kAttackMs  * 0.001f * kSampleRate));
        m_releaseCoef = std::exp(-1.0f / (kReleaseMs * 0.001f * kSampleRate));
    }

    inline float applyBiquad(float x, int idx) {
        float y = b0[idx] * x
                + b1[idx] * x1[idx]
                + b2[idx] * x2[idx]
                - a1[idx] * y1[idx]
                - a2[idx] * y2[idx];
        x2[idx] = x1[idx];  x1[idx] = x;
        y2[idx] = y1[idx];  y1[idx] = y;
        return y;
    }

    inline float applySaturation(float x) {
        // tanh soft-clip with low drive — adds harmonic richness, not crunch
        return std::tanh(kSatDrive * x) / kSatDrive;
    }

    inline float applyCompressor(float x) {
        // Envelope follower: fast attack, moderate release
        float level = std::abs(x);
        float coef  = (level > m_envelope) ? m_attackCoef : m_releaseCoef;
        m_envelope  = coef * m_envelope + (1.0f - coef) * level;

        // Gain in dB
        float level_dB = 20.0f * std::log10(m_envelope + 1e-9f);
        float gain_dB  = 0.0f;
        if (level_dB > kThresholdDb)
            gain_dB = (kThresholdDb - level_dB) * (1.0f - 1.0f / kRatio);

        float targetGain = std::pow(10.0f, gain_dB / 20.0f);

        // Smooth gain: fast to reduce (attack), slow to release
        float gCoef  = (targetGain < m_gainSmooth) ? m_attackCoef : m_releaseCoef;
        m_gainSmooth = gCoef * m_gainSmooth + (1.0f - gCoef) * targetGain;

        return x * m_gainSmooth;
    }
};
