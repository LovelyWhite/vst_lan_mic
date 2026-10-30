#pragma once

#include "Protocol.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace sos {
// One producer and one consumer. Only the consumer may discard queued data.
template <typename T, uint32_t Capacity>
class SpscRing {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
#if !defined(ARDUINO_ARCH_ESP32)
    // Keep DAW callbacks lock-free. ESP32 permits the SDK's atomic fallback:
    // is_always_lock_free == false does not invalidate acquire/release semantics.
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "Lock-free counters required");
#endif
    uint32_t size() const {
        const auto r = read_.load(std::memory_order_acquire);
        return write_.load(std::memory_order_acquire) - r;
    }
    bool push(const T* data, uint32_t count) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (count > Capacity - (w - read_.load(std::memory_order_acquire))) return false;
        for (uint32_t i = 0; i < count; ++i) data_[(w + i) & (Capacity - 1)] = data[i];
        write_.store(w + count, std::memory_order_release);
        return true;
    }
    bool pop(T& result) {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        result = data_[r & (Capacity - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    void discard(uint32_t count) {
        const auto r = read_.load(std::memory_order_relaxed);
        count = std::min(count, write_.load(std::memory_order_acquire) - r);
        read_.store(r + count, std::memory_order_release);
    }
    void clear() { discard(Capacity); }
private:
    std::array<T, Capacity> data_{};
    std::atomic<uint32_t> read_{0}, write_{0};
};

inline int16_t pcm16(float value) {
    if (!std::isfinite(value)) return 0;
    return int16_t(std::max(-32768.0f, std::min(32767.0f, value * 32768.0f)));
}

// Run on the network thread. Reject duplicates/late packets, conceal gaps.
class SequenceTracker {
public:
    void reset() { valid_ = false; lastSample_ = 0; prevSample_ = 0; }
    // A gap wider than this is a stream restart: the 500 ms audio timeout
    // resets this tracker long before the sequence can jump that far, so a
    // larger delta resumes without concealment like a fresh stream.
    static constexpr uint32_t kRestartGapFrames = 40;
    template <typename Sink>
    bool accept(uint32_t seq, const Frame& frame, Sink&& sink) {
        if (valid_) {
            const uint32_t delta = seq - next_;
            if (delta >= 0x80000000u) return false;
            if (delta > 0 && delta <= kRestartGapFrames) {
                lost += delta;
                maxGap = std::max(maxGap, delta);
                bridged += delta;
                next_ = seq + 1;
                const int16_t prior = lastSample_;
                lastSample_ = frame.back();
                constexpr size_t fade = 48;  // 1 ms at 48 kHz
                Frame scratch{};
                if (delta <= 2) {
                    // Glide linearly across a short gap. A bare ramp joins
                    // both ends in value but leaves a step in the first
                    // derivative, which cracks on loud passages, so the ramp
                    // also carries the outgoing per-sample slope (decaying
                    // exponentially: a pure linear extrapolation of a steep
                    // waveform would run away within a few ms) and the real
                    // frame fades in against the extended ramp, making the
                    // join at frame.front() slope-continuous as well.
                    const float start = float(prior);
                    const float slope = float(prior) - float(prevSample_);
                    const float bounded =
                        std::max(-8000.0f, std::min(8000.0f, slope));
                    const float span = float(frame.front()) - start;
                    const float positions =
                        float(delta * uint32_t(frame.size()) + 1);
                    for (uint32_t m = 0; m < delta; ++m) {
                        for (size_t i = 0; i < scratch.size(); ++i) {
                            const uint32_t position =
                                m * uint32_t(scratch.size()) + uint32_t(i) + 1;
                            const float base =
                                start + span * (float(position) / positions);
                            const float tail = bounded * float(position) *
                                std::exp(-float(position) / 12.0f);
                            scratch[i] = int16_t(std::max(-32768.0f,
                                std::min(32767.0f, base + tail)));
                        }
                        sink(scratch);
                    }
                    // Fade the real frame in over 1 ms against the extended
                    // ramp; both boundaries touch the ramp continuously.
                    Frame resume = frame;
                    const float lineSlope = span / positions;
                    for (size_t i = 0; i < fade; ++i) {
                        const float weight = float(i + 1) / float(fade);
                        const float extended =
                            float(frame.front()) + lineSlope * float(i + 1);
                        resume[i] = int16_t(float(frame[i]) * weight +
                                            extended * (1.0f - weight));
                    }
                    sink(resume);
                } else {
                    // Conceal a wide gap with a 1 ms taper out, silence, and
                    // a 1 ms taper back in. Tapering across a whole 14 ms
                    // frame sweeps the full scale in one frame and reads as
                    // a zip; 1 ms only rounds off the discontinuity. Both
                    // taper boundaries that touch silence are exact zeros;
                    // the residual taper step lands where signal is present
                    // and is masked by it.
                    for (size_t i = 0; i < fade; ++i)
                        scratch[i] = int16_t(float(prior) *
                            (1.0f - (float(i) + 1.0f) / float(fade)));
                    sink(scratch);
                    scratch.fill(0);
                    for (uint32_t m = 1; m < delta; ++m) sink(scratch);
                    // The real frame itself fades back in — sink it exactly
                    // once, here, never again below.
                    Frame resume = frame;
                    for (size_t i = 0; i < fade; ++i)
                        resume[i] = int16_t(float(frame[i]) *
                            (float(i) / float(fade)));
                    sink(resume);
                }
                // The slope estimate always comes from the last two samples
                // of real received audio, never from synthesized concealment.
                prevSample_ = frame[frame.size() - 2];
                return true;
            }
        }
        next_ = seq + 1;
        valid_ = true;
        prevSample_ = frame[frame.size() - 2];
        lastSample_ = frame.back();
        sink(frame);
        return true;
    }
    uint32_t lost = 0, maxGap = 0, bridged = 0;
private:
    bool valid_ = false;
    uint32_t next_ = 0;
    int16_t lastSample_ = 0, prevSample_ = 0;
};

// Consumer-side elastic playout. A small continuous rate correction compensates
// independent USB/DAW clocks; hard trimming bounds latency after a long stall.
// The queue holds interleaved samples: mono (Channels == 1) or stereo L,R pairs
// (Channels == 2). Both channels share one interpolation phase and one fade so
// L/R never decorrelate — independent ramps would smear the image and can crack.
template <int Channels>
class Playout {
public:
    static_assert(Channels == 1 || Channels == 2,
                  "Playout supports mono or interleaved stereo");
#if defined(ARDUINO_ARCH_ESP32)
    // Opus keeps the Wi-Fi/lwIP load low enough to restore the full stability
    // buffer while retaining more than 100 KB of measured internal heap.
    static constexpr size_t frameCapacity = 32768;
    static constexpr uint32_t targetMs = 300;
    static constexpr float maxRateCorrection = 0.002f;
#else
    static constexpr size_t frameCapacity = 32768;
    static constexpr uint32_t targetMs = 300;
    static constexpr float maxRateCorrection = 0.002f;
#endif
    // Queue lengths are in interleaved samples; targets stay in milliseconds
    // when the protocol frame duration changes.
    static constexpr size_t queueCapacity = frameCapacity * size_t(Channels);
    static constexpr uint32_t targetFrames =
        (targetMs + kFrameDurationMs - 1) / kFrameDurationMs;
    static constexpr uint32_t target =
        targetFrames * kFrameSamples * uint32_t(Channels);
    static constexpr uint32_t trimMargin =
        uint32_t(queueCapacity) - target - kFrameSamples * uint32_t(Channels);
    static_assert(target + kFrameSamples * uint32_t(Channels) < queueCapacity,
                  "Playout queue is too small");
    SpscRing<int16_t, queueCapacity> queue;
    std::atomic<uint32_t> underruns{0}, trims{0}, overflows{0};

    void push(const Frame& frame) {
        if (!queue.push(frame.data(), uint32_t(frame.size()))) ++overflows;
    }
    void setNominalRatio(float ratio) {
        nominalRatio_ = std::max(0.125f, std::min(8.0f, ratio));
        ratio_ = nominalRatio_;
    }
    void reset() {
        queue.clear();
        primed_ = false;
        phase_ = 0;
        ratio_ = nominalRatio_;
        for (int ch = 0; ch < Channels; ++ch) previous_[ch] = 0;
        fade_ = 0;
    }
    void prepare() {
        const auto size = queue.size();
        if (size > target + trimMargin) {
            queue.discard(size - target);
            primed_ = false;
            ++trims;
        }
        const float correction = std::max(-maxRateCorrection,
            std::min(maxRateCorrection,
                (float(queue.size()) - float(target)) * 0.000002f));
        const float desired = nominalRatio_ * (1.0f + correction);
        ratio_ += (desired - ratio_) * 0.02f;
    }
    // Mono convenience wrapper; out[0] is the only channel when Channels == 1.
    float next() {
        float out[Channels];
        next(out);
        return out[0];
    }
    // Writes Channels interleaved floats (out[0] = L, out[1] = R when stereo).
    void next(float* out) {
        if (!primed_) {
            if (queue.size() < target) {
                for (int ch = 0; ch < Channels; ++ch) {
                    previous_[ch] = std::abs(previous_[ch]) < 0.000001f
                        ? 0.0f : previous_[ch] * 0.98f;
                    out[ch] = previous_[ch];
                }
                return;
            }
            for (int ch = 0; ch < Channels; ++ch) queue.pop(a_[ch]);
            for (int ch = 0; ch < Channels; ++ch) queue.pop(b_[ch]);
            phase_ = 0;
            primed_ = true;
            fade_ = 0;
        }
        for (int ch = 0; ch < Channels; ++ch)
            out[ch] = (float(a_[ch]) + (float(b_[ch]) - float(a_[ch])) * phase_) /
                      32768.0f;
        fade_ = std::min(1.0f, fade_ + 1.0f / 96.0f);
        for (int ch = 0; ch < Channels; ++ch) out[ch] *= fade_;
        phase_ += ratio_;
        while (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            for (int ch = 0; ch < Channels; ++ch) a_[ch] = b_[ch];
            bool starved = false;
            // Interleaved order: popping the L sample must be followed by its
            // R partner, so a failed pop always ends the advance for all.
            for (int ch = 0; ch < Channels; ++ch)
                if (!queue.pop(b_[ch])) starved = true;
            if (starved) {
                primed_ = false;
                ++underruns;
                break;
            }
        }
        for (int ch = 0; ch < Channels; ++ch) previous_[ch] = out[ch];
    }
private:
    bool primed_ = false;
    int16_t a_[Channels] = {}, b_[Channels] = {};
    float phase_ = 0, nominalRatio_ = 1, ratio_ = 1;
    float previous_[Channels] = {}, fade_ = 0;
};
using MonoPlayout = Playout<1>;
using StereoPlayout = Playout<2>;
} // namespace sos
