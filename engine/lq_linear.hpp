#pragma once
// TONE in linear phase: the same four bands as one symmetric FIR on the mid and one on the side,
// run as a partitioned FFT convolution. Nothing moves in time against anything else, at the cost
// of a fixed delay (about 48 ms at 44.1 and 48 kHz).
//
// When the settings move (a knob, a section gliding in), a new kernel is built on a worker thread
// and crossfaded in, so the audio thread never designs filters or allocates.

#include "kt_phase.hpp"
#include "lq_modules.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace lq {

class LinearTone {
public:
    ~LinearTone() { stop(); }

    void prepare(double fs) {
        stop();
        fs_ = fs;
        delay_ = fs > 100000 ? 8192 : fs > 50000 ? 4096 : 2048;
        part_ = fs > 100000 ? 1024 : fs > 50000 ? 512 : 256;
        ToneParams flat;   // weight 0: a plain delay
        const kt::Kernels k = design(flat);
        conv_[0].set(k, part_);
        conv_[1].set(k, part_);
        built_.set(k, part_);
        asked_ = flat;
        cur_ = 0; fade_ = -1; wait_ = 0;
        state_.store(0);
        running_.store(true);
        worker_ = std::thread([this] { run(); });
    }
    void reset() { conv_[0].reset(); conv_[1].reset(); fade_ = -1; }
    int latency() const { return delay_ + part_; }

    void process(float* L, float* R, int n, const ToneParams& p) {
        // A new kernel when the settings have moved and the last one is in.
        if (fade_ < 0) {
            const int st = state_.load(std::memory_order_acquire);
            if (st == 2) {
                conv_[1 - cur_].adopt(built_);
                state_.store(0, std::memory_order_release);
                wait_ = part_;   // the new kernel's output is whole after one partition
                fade_ = 0;
            } else if (st == 0 && differs(p, asked_)) {
                req_ = p;
                asked_ = p;
                state_.store(1, std::memory_order_release);
            }
        }
        double m[kSub], s[kSub], m2[kSub], s2[kSub];
        for (int i = 0; i < n; ++i) {
            m[i] = m2[i] = 0.5 * (double(L[i]) + R[i]);
            s[i] = s2[i] = 0.5 * (double(L[i]) - R[i]);
        }
        conv_[cur_].process(m, s, n);
        conv_[1 - cur_].process(m2, s2, n);
        for (int i = 0; i < n; ++i) {
            double x = 0;
            if (fade_ >= 0) {
                if (wait_ > 0) --wait_;
                else { x = std::min(1.0, double(fade_) / kFade); ++fade_; }
            }
            const double mo = m[i] + x * (m2[i] - m[i]), so = s[i] + x * (s2[i] - s[i]);
            L[i] = float(mo + so);
            R[i] = float(mo - so);
        }
        if (fade_ >= kFade) { cur_ = 1 - cur_; fade_ = -1; }
    }

private:
    static constexpr int kFade = 1024;

    kt::Kernels design(const ToneParams& p) const {
        return kt::designKernels([&](double f) {
            std::complex<double> hm, hs;
            toneResponseMS(p, f, fs_, hm, hs);
            kt::Mat2 m;
            m.ll = kt::cplx(std::abs(hm), 0);
            m.rr = kt::cplx(std::abs(hs), 0);
            return m;
        }, fs_, 4 * delay_, delay_);
    }
    static bool differs(const ToneParams& a, const ToneParams& b) {
        if (std::abs(a.weight - b.weight) > 1e-3f) return true;
        for (int i = 0; i < kToneBands; ++i)
            if (std::abs(a.b[i].db - b.b[i].db) > 0.01f || std::abs(a.b[i].hz - b.b[i].hz) > 0.002f * b.b[i].hz || std::abs(a.b[i].place - b.b[i].place) > 1e-3f) return true;
        return false;
    }
    void run() {
        while (running_.load()) {
            if (state_.load(std::memory_order_acquire) == 1) {
                built_.set(design(req_), part_);
                state_.store(2, std::memory_order_release);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
    void stop() {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
    }

    double fs_ = 48000;
    int delay_ = 2048, part_ = 256;
    kt::PartConv conv_[2], built_;
    ToneParams req_, asked_;
    int cur_ = 0, fade_ = -1, wait_ = 0;
    std::atomic<int> state_{0};   // 0 idle, 1 asked for (the worker builds), 2 built (the audio thread takes it)
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace lq
