#pragma once
// Lacquer's parameters: one table for the engine, the plugin (host parameters) and the faceplate.
// Values are in their own units; every knob's angle follows `toNorm`, so the printed scales match.

#include <algorithm>
#include <cmath>

namespace lq {

enum Kind { kLin, kLog, kOffLog /* 0 = off, else lo * (hi/lo)^v */, kChoice, kBool };

// How a value moves when a section glides in: in its own units, in log frequency, or as a blend weight.
enum Glide { gLin, gLog, gBlend, gStep };

enum P {
    // assistant
    aTarget, aDest, aAmount, aLearn,
    // clean
    cleanIn, cRes, cHarsh, cRumble, cHear,
    // tone
    toneIn, tLow, tPres, tAir, tMode,
    // dynamics
    dynIn, dGlue, dPunch, dEven, dChan,
    // colour
    colIn, cDrive, cShade, cMix, cType,
    // stereo
    stIn, sWidth, sMono, sCentre, sHear,
    // space (the hidden tray)
    spIn, spType, spAmount, spDecay, spTone, spPre, spLow, spHigh, spGap, spApply, spA, spB,
    // loudness
    loudIn, lTarget, lCeiling, lClip, lGuard,
    // output
    oListen, oCodec, oDither, oMonitor, oMatch,
    // the DETAIL page: tone's frequencies, its fourth band and where each band works; glue's
    // timing; the limiter's style; BEFORE; the codec's bit rate
    tMud, tLowHz, tMudHz, tPresHz, tAirHz, tLowPl, tMudPl, tPresPl, tAirPl, tPhase,
    dAttack, dRelease, dRatio,
    lStyle, lRelease,
    oBefore, oCodecRate,
    kNumParams
};

struct ParamDef {
    P p;
    const char* id;
    const char* name;
    Kind kind;
    float lo, hi, def;
    int nChoices;
    const char* choices;   // '|' separated
    const char* unit;
    bool perSection;       // may differ between sections
    Glide glide;
};

inline const ParamDef* paramTable() {
    static const ParamDef t[kNumParams] = {
        {aTarget, "a_target", "target", kChoice, 0, 2, 0, 3, "genre|refs|mine", "", false, gStep},
        {aDest, "a_dest", "destination", kChoice, 0, 3, 0, 4, "stream|cd|vinyl|club", "", false, gStep},
        {aAmount, "a_amount", "amount", kLin, 0, 100, 70, 0, nullptr, "%", false, gLin},
        {aLearn, "a_learn", "learn", kBool, 0, 1, 1, 0, nullptr, "", false, gStep},

        {cleanIn, "clean_in", "clean in", kBool, 0, 1, 1, 0, nullptr, "", true, gBlend},
        {cRes, "c_res", "resonance", kLin, 0, 10, 3.5f, 0, nullptr, "", true, gLin},
        {cHarsh, "c_harsh", "harsh", kLin, 0, 10, 2.5f, 0, nullptr, "", true, gLin},
        {cRumble, "c_rumble", "rumble", kOffLog, 11.25f, 80, 25, 0, nullptr, "Hz", true, gLog},   // 30 Hz at the middle, as printed
        {cHear, "c_hear", "clean hear", kBool, 0, 1, 0, 0, "result|removed", "", false, gStep},

        {toneIn, "tone_in", "tone in", kBool, 0, 1, 1, 0, nullptr, "", true, gBlend},
        {tLow, "t_low", "low", kLin, -6, 6, 0, 0, nullptr, "dB", true, gLin},
        {tPres, "t_pres", "presence", kLin, -6, 6, -0.8f, 0, nullptr, "dB", true, gLin},
        {tAir, "t_air", "air", kLin, -6, 6, 1.5f, 0, nullptr, "dB", true, gLin},
        {tMode, "t_mode", "tone mode", kBool, 0, 1, 1, 0, "static|dynamic", "", true, gBlend},

        {dynIn, "dyn_in", "dynamics in", kBool, 0, 1, 1, 0, nullptr, "", true, gBlend},
        {dGlue, "d_glue", "glue", kLin, 0, 10, 4, 0, nullptr, "", true, gLin},
        {dPunch, "d_punch", "punch", kLin, -5, 5, 1, 0, nullptr, "", true, gLin},
        {dEven, "d_even", "even", kLin, 0, 10, 2, 0, nullptr, "", true, gLin},
        {dChan, "d_chan", "channels", kBool, 0, 1, 0, 0, "l / r|m / s", "", false, gStep},

        {colIn, "col_in", "colour in", kBool, 0, 1, 1, 0, nullptr, "", true, gBlend},
        {cDrive, "c_drive", "drive", kLin, 0, 10, 3, 0, nullptr, "", true, gLin},
        {cShade, "c_shade", "shade", kLin, 0, 10, 4.5f, 0, nullptr, "", true, gLin},
        {cMix, "c_mix", "mix", kLin, 0, 100, 60, 0, nullptr, "%", true, gLin},
        {cType, "c_type", "type", kChoice, 0, 2, 0, 3, "tape|iron|valve", "", true, gBlend},

        {stIn, "st_in", "stereo in", kBool, 0, 1, 0, 0, nullptr, "", true, gBlend},
        {sWidth, "s_width", "width", kLin, 0, 200, 108, 0, nullptr, "%", true, gLin},
        {sMono, "s_mono", "bass mono", kOffLog, 48, 300, 120, 0, nullptr, "Hz", true, gLog},   // 120 Hz at the middle, as printed
        {sCentre, "s_centre", "centre", kLin, -3, 3, 0, 0, nullptr, "dB", true, gLin},
        {sHear, "s_hear", "stereo hear", kBool, 0, 1, 0, 0, "stereo|mono", "", false, gStep},

        {spIn, "sp_in", "space in", kBool, 0, 1, 1, 0, nullptr, "", true, gBlend},
        {spType, "sp_type", "space type", kChoice, 0, 5, 2, 6, "clean|studio|plate|spring|chamber|room", "", true, gStep},
        {spAmount, "sp_amount", "space amount", kLin, -60, -15, -32, 0, nullptr, "dB", true, gLin},
        {spDecay, "sp_decay", "decay", kLog, 0.3f, 2.5f, 1.4f, 0, nullptr, "s", true, gLog},
        {spTone, "sp_tone", "space tone", kLin, 0, 100, 50, 0, nullptr, "%", true, gLin},
        {spPre, "sp_pre", "pre-delay", kLin, 0, 100, 20, 0, nullptr, "ms", true, gLin},
        {spLow, "sp_low", "space low cut", kLog, 100, 600, 200, 0, nullptr, "Hz", true, gLog},
        {spHigh, "sp_high", "space high cut", kLog, 2, 16, 8, 0, nullptr, "kHz", true, gLog},
        {spGap, "sp_gap", "gap-fill", kLin, 0, 100, 60, 0, nullptr, "%", true, gLin},
        {spApply, "sp_apply", "apply to", kChoice, 0, 2, 1, 3, "mid|side|both", "", true, gStep},
        {spA, "sp_a", "space a", kLin, 0, 10, 4, 0, nullptr, "", true, gLin},
        {spB, "sp_b", "space b", kLin, 0, 10, 6, 0, nullptr, "", true, gLin},

        {loudIn, "loud_in", "loudness in", kBool, 0, 1, 1, 0, nullptr, "", false, gBlend},
        {lTarget, "l_target", "target", kLin, -16, -6, -9.4f, 0, nullptr, "LUFS", true, gLin},
        {lCeiling, "l_ceiling", "ceiling", kLin, -2, 0, -1, 0, nullptr, "dBTP", false, gLin},
        {lClip, "l_clip", "clipper", kLin, 0, 10, 3, 0, nullptr, "", false, gLin},
        {lGuard, "l_guard", "guard", kBool, 0, 1, 1, 0, nullptr, "", false, gStep},

        {oListen, "o_listen", "listen on", kChoice, 0, 4, 0, 5, "desk|phone|buds|car|club", "", false, gStep},
        {oCodec, "o_codec", "codec", kBool, 0, 1, 0, 0, nullptr, "", false, gStep},
        {oDither, "o_dither", "dither", kChoice, 0, 2, 1, 3, "off|24|16", "", false, gStep},
        {oMonitor, "o_monitor", "monitor", kLin, -30, 0, 0, 0, nullptr, "dB", false, gLin},
        {oMatch, "o_match", "matched", kBool, 0, 1, 1, 0, nullptr, "", false, gStep},

        {tMud, "t_mud", "mud", kLin, -6, 6, 0, 0, nullptr, "dB", true, gLin},
        {tLowHz, "t_low_hz", "low freq", kLog, 40, 250, 110, 0, nullptr, "Hz", true, gLog},
        {tMudHz, "t_mud_hz", "mud freq", kLog, 120, 800, 300, 0, nullptr, "Hz", true, gLog},
        {tPresHz, "t_pres_hz", "presence freq", kLog, 1000, 6000, 2500, 0, nullptr, "Hz", true, gLog},
        {tAirHz, "t_air_hz", "air freq", kLog, 5000, 18000, 10000, 0, nullptr, "Hz", true, gLog},
        {tLowPl, "t_low_pl", "low on", kChoice, 0, 2, 0, 3, "stereo|mid|side", "", true, gBlend},
        {tMudPl, "t_mud_pl", "mud on", kChoice, 0, 2, 0, 3, "stereo|mid|side", "", true, gBlend},
        {tPresPl, "t_pres_pl", "presence on", kChoice, 0, 2, 0, 3, "stereo|mid|side", "", true, gBlend},
        {tAirPl, "t_air_pl", "air on", kChoice, 0, 2, 0, 3, "stereo|mid|side", "", true, gBlend},
        {tPhase, "t_phase", "tone phase", kChoice, 0, 1, 0, 2, "natural|linear", "", false, gStep},
        {dAttack, "d_attack", "glue attack", kChoice, 0, 4, 3, 5, "1 ms|3 ms|10 ms|30 ms|80 ms", "", true, gStep},
        {dRelease, "d_release", "glue release", kChoice, 0, 3, 0, 4, "auto|100 ms|300 ms|800 ms", "", true, gStep},
        {dRatio, "d_ratio", "glue ratio", kChoice, 0, 3, 1, 4, "1.5 : 1|2 : 1|3 : 1|4 : 1", "", true, gStep},
        {lStyle, "l_style", "limiter style", kChoice, 0, 2, 0, 3, "clean|punchy|dense", "", false, gStep},
        {lRelease, "l_release", "limiter release", kChoice, 0, 3, 0, 4, "auto|fast|medium|slow", "", false, gStep},
        {oBefore, "o_before", "before", kBool, 0, 1, 0, 0, nullptr, "", false, gStep},
        {oCodecRate, "o_codec_rate", "codec rate", kChoice, 0, 1, 0, 2, "aac 256|aac 128", "", false, gStep},
    };
    return t;
}

inline const ParamDef& def(int p) { return paramTable()[p]; }

// Knob position (0..1) <-> value.
inline float fromNorm(int p, float v) {
    const auto& d = def(p);
    v = std::clamp(v, 0.0f, 1.0f);
    switch (d.kind) {
        case kLin: return d.lo + (d.hi - d.lo) * v;
        case kLog: return d.lo * std::pow(d.hi / d.lo, v);
        case kOffLog: return v <= 0.0005f ? 0.0f : d.lo * std::pow(d.hi / d.lo, v);
        case kChoice: return std::round(v * float(d.nChoices - 1));
        case kBool: return v >= 0.5f ? 1.0f : 0.0f;
    }
    return v;
}

inline float toNorm(int p, float x) {
    const auto& d = def(p);
    switch (d.kind) {
        case kLin: return std::clamp((x - d.lo) / (d.hi - d.lo), 0.0f, 1.0f);
        case kLog: return std::clamp(std::log(std::max(x, d.lo) / d.lo) / std::log(d.hi / d.lo), 0.0f, 1.0f);
        case kOffLog: return x <= 0 ? 0.0f : std::clamp(std::log(std::max(x, d.lo) / d.lo) / std::log(d.hi / d.lo), 0.0005f, 1.0f);
        case kChoice: return d.nChoices > 1 ? std::clamp(x / float(d.nChoices - 1), 0.0f, 1.0f) : 0.0f;
        case kBool: return x >= 0.5f ? 1.0f : 0.0f;
    }
    return x;
}

inline int findParam(const char* id) {
    for (int i = 0; i < kNumParams; ++i) {
        const char* a = def(i).id;
        int k = 0;
        while (a[k] && a[k] == id[k]) ++k;
        if (!a[k] && !id[k]) return i;
    }
    return -1;
}

// The section keys: 0 = the song, 1..8 = the eight sections.
constexpr int kNumKeys = 9;   // the song and eight section keys

// Values of every parameter, in their own units.
struct Values {
    float v[kNumParams];
    Values() { for (int i = 0; i < kNumParams; ++i) v[i] = def(i).def; }
    float operator[](int i) const { return v[i]; }
    float& operator[](int i) { return v[i]; }
};

}  // namespace lq
