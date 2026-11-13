#pragma once
// CODEC: the master as a listener's player will hear it, through Apple's AAC encoder and back
// (the encoder Apple Music and iTunes use). For listening only: it never reaches a bounce.
//
// The audio thread hands the master to a worker thread through a lock-free ring, and plays back
// what the worker decoded through another, a fixed few thousand samples later. That delay is the
// monitoring's only: the host isn't told about it, so the meters and the screen stay with the song.
//
// Only on a Mac. Elsewhere `available()` is false and the master passes straight through.

#include "lq_engine.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// A single-producer, single-consumer ring of interleaved stereo frames.
class FrameRing {
public:
    void prepare(int frames) {
        int n = 1;
        while (n < frames) n <<= 1;
        buf_.assign(size_t(n) * 2, 0.0f);
        mask_ = n - 1;
        w_.store(0); r_.store(0);
    }
    int ready() const { return int(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed)); }
    int space() const { return int(mask_ + 1) - int(w_.load(std::memory_order_relaxed) - r_.load(std::memory_order_acquire)); }
    // Producer.
    int write(const float* L, const float* R, int n) {
        n = std::min(n, space());
        size_t w = w_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++w) { const size_t k = (w & size_t(mask_)) * 2; buf_[k] = L[i]; buf_[k + 1] = R[i]; }
        w_.store(w, std::memory_order_release);
        return n;
    }
    int writeInterleaved(const float* x, int n) {
        n = std::min(n, space());
        size_t w = w_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++w) { const size_t k = (w & size_t(mask_)) * 2; buf_[k] = x[2 * i]; buf_[k + 1] = x[2 * i + 1]; }
        w_.store(w, std::memory_order_release);
        return n;
    }
    // Consumer.
    int read(float* L, float* R, int n) {
        n = std::min(n, ready());
        size_t r = r_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++r) { const size_t k = (r & size_t(mask_)) * 2; L[i] = buf_[k]; R[i] = buf_[k + 1]; }
        r_.store(r, std::memory_order_release);
        return n;
    }
    int readInterleaved(float* x, int n) {
        n = std::min(n, ready());
        size_t r = r_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++r) { const size_t k = (r & size_t(mask_)) * 2; x[2 * i] = buf_[k]; x[2 * i + 1] = buf_[k + 1]; }
        r_.store(r, std::memory_order_release);
        return n;
    }
    void skipAll() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<float> buf_;
    int mask_ = 0;
    std::atomic<size_t> w_{0}, r_{0};
};

class Codec final : public lq::MonitorTap {
public:
    Codec();
    ~Codec() override;

    // Message thread (with the audio stopped, or from the processor's timer for the bit rate).
    void prepare(double sampleRate);
    void setBitrate(int kbps);
    bool available() const { return available_.load(); }
    std::string why() const { return why_; }           // when it isn't available
    int kbps() const { return kbps_; }

    // Audio thread.
    void process(float* L, float* R, int n) override;
    float peakDb() const { return peakDb_.load(); }    // the decoded master's true peak, held
    void resetPeak() { resetPeak_.store(true); }

    // For the tests: wait until the worker has caught up with what it was given.
    void drain(int timeoutMs);

    static constexpr int kPrime = 4096;   // decoded frames banked before playback starts

private:
    void start();
    void stop();
    void run();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    double fs_ = 48000;
    int kbps_ = 256;
    std::atomic<bool> available_{false}, running_{false}, resetPeak_{false};
    std::string why_;
    std::thread worker_;
    FrameRing in_, out_;
    bool playing_ = false;
    std::atomic<float> peakDb_{-120};
    lp::TruePeak tp_[2];
    double hold_ = 0;
};
