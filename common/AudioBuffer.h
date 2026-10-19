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

// Run on the network thread. Reject duplicates/late packets, conceal short gaps.
class SequenceTracker {
public:
    void reset() { valid_ = false; lastSample_ = 0; }
    template <typename Sink>
    bool accept(uint32_t seq, const Frame& frame, Sink&& sink) {
        if (valid_) {
            const uint32_t delta = seq - next_;
            if (delta >= 0x80000000u) return false;
            if (delta > 0) {
                lost += delta;
                maxGap = std::max(maxGap, delta);
                if (delta <= 2) {
                    // Bridge isolated gaps without the audible fade-to-silence
                    // dip used for longer interruptions.
                    const uint32_t bridgeSamples =
                        delta * uint32_t(frame.size());
                    const float start = float(lastSample_);
                    const float change = float(frame.front()) - start;
                    Frame scratch{};
                    for (uint32_t missing = 0; missing < delta; ++missing) {
                        for (size_t i = 0; i < scratch.size(); ++i) {
                            const uint32_t position =
                                missing * uint32_t(scratch.size()) +
                                uint32_t(i) + 1;
                            scratch[i] = int16_t(start + change *
                                (float(position) /
                                 float(bridgeSamples + 1)));
                        }
                        sink(scratch);
                    }
                    bridged += delta;
                    next_ = seq + 1;
                    lastSample_ = frame.back();
                    sink(frame);
                    return true;
                }
                constexpr uint32_t concealFramesMax =
                    (300 + kFrameDurationMs - 1) / kFrameDurationMs;
                const uint32_t concealFrames = std::min(delta, concealFramesMax);
                Frame scratch{};
                for (size_t i = 0; i < scratch.size(); ++i)
                    scratch[i] = int16_t(float(lastSample_) *
                        (1.0f - float(i + 1) / float(scratch.size())));
                sink(scratch);
                scratch.fill(0);
                for (uint32_t i = 1; i < concealFrames; ++i) sink(scratch);

                // Resume from silence without a full-scale discontinuity.
                for (size_t i = 0; i < scratch.size(); ++i)
                    scratch[i] = int16_t(float(frame[i]) *
                        (float(i + 1) / float(scratch.size())));
                next_ = seq + 1;
                lastSample_ = frame.back();
                sink(scratch);
                return true;
            }
        }
        next_ = seq + 1;
        valid_ = true;
        lastSample_ = frame.back();
        sink(frame);
        return true;
    }
    uint32_t lost = 0, maxGap = 0, bridged = 0;
private:
    bool valid_ = false;
    uint32_t next_ = 0;
    int16_t lastSample_ = 0;
};

// Consumer-side elastic playout. A small continuous rate correction compensates
// independent USB/DAW clocks; hard trimming bounds latency after a long stall.
class Playout {
public:
#if defined(ARDUINO_ARCH_ESP32)
    // Opus keeps the Wi-Fi/lwIP load low enough to restore the full stability
    // buffer while retaining more than 100 KB of measured internal heap.
    static constexpr size_t queueCapacity = 32768;
    static constexpr uint32_t targetMs = 300;
    static constexpr float maxRateCorrection = 0.002f;
#else
    static constexpr size_t queueCapacity = 32768;
    static constexpr uint32_t targetMs = 300;
    static constexpr float maxRateCorrection = 0.002f;
#endif
    // Keep targets in milliseconds when the protocol frame duration changes.
    static constexpr uint32_t targetFrames =
        (targetMs + kFrameDurationMs - 1) / kFrameDurationMs;
    static constexpr uint32_t target = targetFrames * kFrameSamples;
    static constexpr uint32_t trimMargin = queueCapacity - target - kFrameSamples;
    static_assert(target + kFrameSamples < queueCapacity, "Playout queue is too small");
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
        previous_ = 0;
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
    float next() {
        if (!primed_) {
            if (queue.size() < target) {
                previous_ = std::abs(previous_) < 0.000001f ? 0.0f : previous_ * 0.98f;
                return previous_;
            }
            queue.pop(a_); queue.pop(b_);
            phase_ = 0;
            primed_ = true;
            fade_ = 0;
        }
        float out = (float(a_) + (float(b_) - float(a_)) * phase_) / 32768.0f;
        fade_ = std::min(1.0f, fade_ + 1.0f / 96.0f);
        out *= fade_;
        phase_ += ratio_;
        while (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            a_ = b_;
            if (!queue.pop(b_)) {
                primed_ = false;
                ++underruns;
                break;
            }
        }
        previous_ = out;
        return out;
    }
private:
    bool primed_ = false;
    int16_t a_ = 0, b_ = 0;
    float phase_ = 0, nominalRatio_ = 1, ratio_ = 1;
    float previous_ = 0, fade_ = 0;
};
} // namespace sos
