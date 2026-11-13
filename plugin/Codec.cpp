#include "Codec.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(__APPLE__)
 #define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
 #include <AudioToolbox/AudioToolbox.h>
#endif

namespace {
constexpr int kChunk = 1024;               // AAC's frame
constexpr int kRing = 1 << 16;             // frames each way
}  // namespace

#if defined(__APPLE__)
struct Codec::Impl {
    AudioConverterRef enc = nullptr, dec = nullptr;
    UInt32 maxPacket = 0;
    std::vector<char> packet;
    std::vector<float> pcmIn, pcmOut;

    ~Impl() { dispose(); }
    void dispose() {
        if (enc) { AudioConverterDispose(enc); enc = nullptr; }
        if (dec) { AudioConverterDispose(dec); dec = nullptr; }
    }

    static AudioStreamBasicDescription pcmFormat(double fs) {
        AudioStreamBasicDescription d{};
        d.mSampleRate = fs;
        d.mFormatID = kAudioFormatLinearPCM;
        d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        d.mBitsPerChannel = 32;
        d.mChannelsPerFrame = 2;
        d.mFramesPerPacket = 1;
        d.mBytesPerFrame = 8;
        d.mBytesPerPacket = 8;
        return d;
    }

    bool open(double fs, int kbps, std::string& why) {
        dispose();
        AudioStreamBasicDescription pcm = pcmFormat(fs), aac{};
        aac.mSampleRate = fs;
        aac.mFormatID = kAudioFormatMPEG4AAC;
        aac.mChannelsPerFrame = 2;
        aac.mFramesPerPacket = kChunk;
        UInt32 sz = sizeof(aac);
        AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &sz, &aac);
        if (AudioConverterNew(&pcm, &aac, &enc) != noErr || !enc) { why = "the AAC encoder won't run at this sample rate (it needs 48 kHz or less)"; return false; }
        UInt32 br = UInt32(kbps * 1000);
        AudioConverterSetProperty(enc, kAudioConverterEncodeBitRate, sizeof(br), &br);
        sz = sizeof(maxPacket);
        AudioConverterGetProperty(enc, kAudioConverterPropertyMaximumOutputPacketSize, &sz, &maxPacket);
        if (maxPacket == 0) maxPacket = 8192;
        packet.assign(maxPacket, 0);
        UInt32 cookieSize = 0;
        std::vector<char> cookie;
        if (AudioConverterGetPropertyInfo(enc, kAudioConverterCompressionMagicCookie, &cookieSize, nullptr) == noErr && cookieSize > 0) {
            cookie.resize(cookieSize);
            AudioConverterGetProperty(enc, kAudioConverterCompressionMagicCookie, &cookieSize, cookie.data());
        }
        if (AudioConverterNew(&aac, &pcm, &dec) != noErr || !dec) { why = "the AAC decoder wouldn't start"; dispose(); return false; }
        if (!cookie.empty()) AudioConverterSetProperty(dec, kAudioConverterDecompressionMagicCookie, UInt32(cookie.size()), cookie.data());
        pcmIn.assign(size_t(kChunk) * 2, 0.0f);
        pcmOut.assign(size_t(kChunk) * 2, 0.0f);
        return true;
    }

    // ---- encoding: the input callback hands over one chunk, then says "no more for now"
    struct EncFeed { const float* data; UInt32 frames; };
    static OSStatus encInput(AudioConverterRef, UInt32* io, AudioBufferList* list, AudioStreamPacketDescription** desc, void* user) {
        auto* f = static_cast<EncFeed*>(user);
        if (f->frames == 0) { *io = 0; return 1; }
        const UInt32 n = std::min(*io, f->frames);
        list->mNumberBuffers = 1;
        list->mBuffers[0].mNumberChannels = 2;
        list->mBuffers[0].mData = const_cast<float*>(f->data);
        list->mBuffers[0].mDataByteSize = n * 8;
        f->data += n * 2;
        f->frames -= n;
        *io = n;
        if (desc) *desc = nullptr;
        return noErr;
    }
    struct DecFeed { const void* data; UInt32 bytes; AudioStreamPacketDescription desc; bool given; };
    static OSStatus decInput(AudioConverterRef, UInt32* io, AudioBufferList* list, AudioStreamPacketDescription** desc, void* user) {
        auto* f = static_cast<DecFeed*>(user);
        if (f->given) { *io = 0; return 1; }
        list->mNumberBuffers = 1;
        list->mBuffers[0].mNumberChannels = 2;
        list->mBuffers[0].mData = const_cast<void*>(f->data);
        list->mBuffers[0].mDataByteSize = f->bytes;
        *io = 1;
        if (desc) *desc = &f->desc;
        f->given = true;
        return noErr;
    }

    // One chunk in; whatever decoded audio is ready comes out (into the ring).
    void chunk(FrameRing& out) {
        EncFeed feed{pcmIn.data(), UInt32(kChunk)};
        for (int guard = 0; guard < 8; ++guard) {
            UInt32 packets = 1;
            AudioBufferList list;
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = 2;
            list.mBuffers[0].mDataByteSize = maxPacket;
            list.mBuffers[0].mData = packet.data();
            AudioStreamPacketDescription pd{};
            const OSStatus st = AudioConverterFillComplexBuffer(enc, encInput, &feed, &packets, &list, &pd);
            if (packets > 0) decode(packet.data(), list.mBuffers[0].mDataByteSize, out);
            if (st != noErr || packets == 0) break;
        }
    }
    void decode(const void* data, UInt32 bytes, FrameRing& out) {
        DecFeed feed{data, bytes, {}, false};
        feed.desc.mStartOffset = 0;
        feed.desc.mVariableFramesInPacket = 0;
        feed.desc.mDataByteSize = bytes;
        for (int guard = 0; guard < 4; ++guard) {
            UInt32 frames = kChunk;
            AudioBufferList list;
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = 2;
            list.mBuffers[0].mDataByteSize = UInt32(kChunk * 8);
            list.mBuffers[0].mData = pcmOut.data();
            const OSStatus st = AudioConverterFillComplexBuffer(dec, decInput, &feed, &frames, &list, nullptr);
            if (frames > 0) out.writeInterleaved(pcmOut.data(), int(frames));
            if (st != noErr || frames == 0) break;
        }
    }
};
#else
struct Codec::Impl {
    std::vector<float> pcmIn;
    bool open(double, int, std::string& why) { why = "the codec preview needs a Mac (Apple's AAC encoder)"; return false; }
    void chunk(FrameRing&) {}
};
#endif

Codec::Codec() : impl_(std::make_unique<Impl>()) {
    in_.prepare(kRing);
    out_.prepare(kRing);
}

Codec::~Codec() { stop(); }

void Codec::prepare(double sampleRate) {
    stop();
    fs_ = sampleRate;
    in_.prepare(kRing);
    out_.prepare(kRing);
    playing_ = false;
    for (auto& t : tp_) t.reset();
    hold_ = 0;
    peakDb_.store(-120);
    start();
}

void Codec::setBitrate(int kbps) {
    if (kbps == kbps_ && available_.load()) return;
    kbps_ = kbps;
    stop();
    start();
}

void Codec::start() {
    available_.store(impl_->open(fs_, kbps_, why_));
    if (!available_.load()) return;
    running_.store(true);
    worker_ = std::thread([this] { run(); });
}

void Codec::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    available_.store(false);
}

void Codec::run() {
    in_.skipAll();   // whatever was waiting belongs to the last encoder
    while (running_.load()) {
        if (in_.ready() >= kChunk && out_.space() >= 4 * kChunk) {
            in_.readInterleaved(impl_->pcmIn.data(), kChunk);
            impl_->chunk(out_);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void Codec::process(float* L, float* R, int n) {
    if (!available_.load(std::memory_order_relaxed)) return;
    in_.write(L, R, n);
    // Bank a little decoded audio before playing, so the worker's timing never shows.
    if (!playing_ && out_.ready() >= kPrime) playing_ = true;
    int got = 0;
    if (playing_) got = out_.read(L, R, n);
    if (got < n) {   // nothing (yet): silence, and bank again
        for (int i = got; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
        if (playing_ && got == 0) playing_ = false;
    }
    if (resetPeak_.exchange(false)) hold_ = 0;
    double pk = 0;
    for (int i = 0; i < n; ++i) pk = std::max({pk, tp_[0].process(L[i]), tp_[1].process(R[i])});
    hold_ = std::max(hold_, pk);
    peakDb_.store(float(lp::gainToDb(hold_ + 1e-12)));
}

void Codec::drain(int timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (available_.load() && in_.ready() >= kChunk && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
