class Component extends DCLogic {
takenNow() {
const st = this.state || {};
return st.taken || { m1: true, m2: true, m3: true, m4: true, m5: true, m6: false };
}
pageNow() {
return (this.state || {}).page || "bal";
}
secNow() {
const s = (this.state || {}).sec;
return s === undefined ? "song" : s;
}
balance(taken, sec) {
const AMT = 0.7;
const l2 = (v) => Math.log(v) / Math.LN2;
const bell = (f, fc, oct) => Math.exp(-Math.pow(l2(f / fc) / oct, 2));
const shelf = (f, fc) => 1 / (1 + Math.pow(fc / f, 2.5));
const HARSH = { 1: 0.6, 2: 1.0, 3: 3.4, 4: 1.4, 5: 0.8 };
const SHARE = { 1: 8 / 88, 2: 32 / 88, 3: 32 / 88, 4: 8 / 88, 5: 8 / 88 };
const harshOf = (k, withMoves) => HARSH[k] * (k === 3 && withMoves && taken.m1 ? 1 - AMT : 1);
const harsh = (withMoves) => sec === "song" ? [1, 2, 3, 4, 5].reduce((a, k) => a + SHARE[k] * harshOf(k, withMoves), 0) : harshOf(sec, withMoves);
const hNow = harsh(true);
const hRaw = harsh(false);
const N = 200;
const fs = [];
for (let i = 0; i <= N; i++) fs.push(20 * Math.pow(1000, i / N));
const X = (f) => 8 + 364 * Math.log(f / 20) / Math.log(1000);
const Y = (db) => 118 - db * 6;
const tgt = (f) => 0.4 + 2.0 * bell(f, 70, 1.4) - 0.9 * Math.max(0, l2(f / 1500));
const wid = (f) => 1.3 + 0.7 * Math.min(1, Math.pow(Math.abs(l2(f / 700)) / 5, 2));
const wig = (f) => 0.22 * Math.sin(l2(f) * 5.3) + 0.15 * Math.sin(l2(f) * 9.1 + 1.0);
const raw = (f) => hRaw * bell(f, 3200, 0.45) - 2.15 * shelf(f, 9500) + 3.2 * bell(f, 22, 0.6) + wig(f);
const dev = (f) => hNow * bell(f, 3200, 0.45) - 2.15 * (taken.m2 ? 1 - AMT : 1) * shelf(f, 9500)
+ (taken.m4 ? 0 : 3.2 * bell(f, 22, 0.6)) + wig(f);
const pathOf = (vals) => vals.map((v, i) => (i ? "L" : "M") + X(fs[i]).toFixed(1) + " " + Y(v).toFixed(1)).join(" ");
const up = fs.map((f) => tgt(f) + wid(f));
const lo = fs.map((f) => tgt(f) - wid(f));
const back = [];
for (let i = N; i >= 0; i--) back.push("L" + X(fs[i]).toFixed(1) + " " + Y(lo[i]).toFixed(1));
const corr = pathOf(up) + " " + back.join(" ") + " Z";
const out = fs.map((f) => Math.abs(dev(f)) > wid(f));
const inside = Math.round(100 * out.filter((o) => !o).length / fs.length);
const runs = [];
let s = -1;
for (let i = 0; i <= N + 1; i++) {
const o = i <= N && out[i];
if (o && s < 0) s = i;
if (!o && s >= 0) { runs.push([s, i - 1]); s = -1; }
}
const flags = runs.map((rn) => {
let k = rn[0];
for (let i = rn[0]; i <= rn[1]; i++) if (Math.abs(dev(fs[i])) > Math.abs(dev(fs[k]))) k = i;
const f = fs[k];
const d = dev(f);
const over = d > 0;
let word;
if (f < 50) word = over ? "RUMBLE" : "NO SUB";
else if (f < 250) word = over ? "BOOMY" : "THIN";
else if (f < 1200) word = over ? "MUDDY" : "HOLLOW";
else if (f < 6500) word = over ? "HARSH" : "DISTANT";
else word = over ? "SHARP" : "DULL";
const xa = X(fs[rn[0]]);
const xb = X(fs[rn[1]]);
const yv = Y(tgt(f) + d);
return { x: (xa - 2).toFixed(1), w: Math.max(5, xb - xa + 4).toFixed(1),
lx: Math.max(44, Math.min(336, X(f))).toFixed(1), ly: (over ? yv - 12 : yv + 20).toFixed(1),
text: word + " " + (over ? "+" : "−") + Math.abs(d).toFixed(1) + " dB" };
});
const freqs = [[50, "50"], [100, "100"], [200, "200"], [500, "500"], [1000, "1k"], [2000, "2k"], [5000, "5k"], [10000, "10k"]]
.map((p) => ({ x: X(p[0]).toFixed(1), t: p[1] }));
return { corr: corr, up: pathOf(up), lo: pathOf(lo), track: pathOf(fs.map((f) => tgt(f) + dev(f))),
before: pathOf(fs.map((f) => tgt(f) + raw(f))), flags: flags, freqs: freqs, inside: inside, nOut: runs.length };
}
loudness(taken) {
const XL = (v) => 8 + 364 * (v + 16) / 10;
const YK = (k) => 52 + 140 * (1 - (k - 40) / 60);
const knee = -8.6;
const kept = (v) => {
let k = 100 - 1.2 * (v + 16);
if (v > knee) k -= 12 * Math.pow(v - knee, 1.3);
return Math.max(40, k);
};
const pts = [];
for (let i = 0; i <= 120; i++) {
const v = -16 + 10 * i / 120;
pts.push((i ? "L" : "M") + XL(v).toFixed(1) + " " + YK(kept(v)).toFixed(1));
}
const now = taken.m5 ? -9.4 : -8.2;
const nx = XL(now);
const ny = YK(kept(now));
const fmt = (v) => "−" + Math.abs(v).toFixed(1);
const xs = [-16, -14, -12, -10, -8, -6].map((v, i) => ({ x: (i === 0 ? 9 : (i === 5 ? 371 : XL(v))).toFixed(1),
t: (i === 5 ? fmt(v).replace(".0", "") + " LUFS" : fmt(v).replace(".0", "")), cls: i === 0 ? "ax" : (i === 5 ? "ax-end" : "ax-mid") }));
const ys = [[100, "100%"], [70, "70%"], [40, "40%"]].map((p) => ({ y: (YK(p[0]) + 4).toFixed(1), t: p[1] }));
return { curve: pts.join(" "), kneeX: XL(knee).toFixed(1), warnW: (372 - XL(knee)).toFixed(1), streamX: XL(-14).toFixed(1),
nx: nx.toFixed(1), ny: ny.toFixed(1), nlx: nx.toFixed(1), nly: (ny - 14).toFixed(1),
kneeLx: (XL(knee) + 6).toFixed(1), kneeTxt: "at " + fmt(knee) + " LUFS",
streamLx: (XL(-14) + 5).toFixed(1), streamTxt: "turns it down " + (now + 14).toFixed(1) + " dB",
xs: xs, ys: ys, now: now, kept: Math.round(kept(now)), fmt: fmt };
}
sections(taken, sec, marking) {
const NAMES = ["INTRO", "VERSE", "CHORUS", "BRIDGE", "OUTRO"];
const MAP = [[1, 8], [2, 16], [3, 8], [2, 16], [3, 8], [4, 8], [3, 8], [3, 8], [5, 8]];
let bar = 0;
const blocks = MAP.map((p) => {
const x = 8 + 364 * bar / 88;
const w = 364 * p[1] / 88;
bar += p[1];
const on = sec !== "song" && p[0] === sec;
return { x: (x + 1).toFixed(1), w: (w - 2).toFixed(1), tx: (x + w / 2).toFixed(1), n: String(p[0]),
cls: on ? "blk-on" : "blk", tcls: on ? "blk-n-on" : "blk-n" };
});
const D = {
1: [["TONE", "AIR +1.5 → +0.5 dB"], ["SPACE", "AMOUNT −32 → −26 dB"]],
2: [["TONE", "PRESENCE −0.8 → 0.0 dB"], ["DYNAMICS", "GLUE 4.0 → 3.0"]],
3: [].concat(taken.m1 ? [["CLEAN", "HARSH 2.5 → 4.0   (move 1)"]] : [],
taken.m3 ? [["STEREO", "IN, WIDTH 108 → 116%   (move 3)"]] : [], [["COLOUR", "DRIVE 3.0 → 3.6"]]),
4: [["SPACE", "AMOUNT −32 → −24 dB, DECAY 1.4 → 2.0 s"], ["TONE", "LOW 0.0 → −0.5 dB"]],
5: []
};
let title, rows, foot;
if (sec === "song") {
title = "EVERY SECTION STARTS FROM THE SONG SETTINGS";
rows = [1, 2, 3, 4, 5].map((k) => ({ a: k + "  " + NAMES[k - 1],
b: D[k].length ? "changes " + D[k].length + (D[k].length > 1 ? " things" : " thing") : "follows the song" }));
foot = "Loudness and the output are set once, for the whole song.";
} else {
title = "WHAT THE " + NAMES[sec - 1] + " CHANGES";
rows = D[sec].length ? D[sec].map((r) => ({ a: r[0], b: r[1] })) : [{ a: "", b: "Nothing. It follows the song." }];
foot = "Everything else follows the song. Changes glide in over a beat.";
}
if (marking) foot = "Press MARK again to finish. Marks land on the nearest bar.";
rows = rows.map((r, i) => ({ a: r.a, b: r.b, y: (127 + i * 14.5).toFixed(1) }));
return { blocks: blocks, title: title, rows: rows, foot: foot, names: NAMES };
}
renderVals() {
const MOVES = [
{ k: "m1", n: "1", sec: "CLEAN", what: "HARSH  2.5 → 4.0", why: "The choruses get harsh around 3.2 kHz.", scope: "CHORUS", conf: 88 },
{ k: "m2", n: "2", sec: "TONE", what: "AIR  +1.5 dB", why: "The top end sits under the target.", scope: "SONG", conf: 81 },
{ k: "m3", n: "3", sec: "STEREO", what: "IN, WIDTH  116%", why: "The choruses can open up a little.", scope: "CHORUS", conf: 61 },
{ k: "m4", n: "4", sec: "CLEAN", what: "RUMBLE  CUT BELOW 25 Hz", why: "Sub rumble under the kick.", scope: "SONG", conf: 92 },
{ k: "m5", n: "5", sec: "LOUDNESS", what: "TARGET  −9.4 LUFS", why: "Any louder and the drums start to smear.", scope: "SONG", conf: 90 },
{ k: "m6", n: "6", sec: "STEREO", what: "BASS MONO  BELOW 120 Hz", why: "The low end drifts off centre.", scope: "SONG", conf: 58 }
];
const SONGWIDE = " Set once for the whole song: it stays the same in every section.";
const T = {
crt_bal: { name: "balance screen", range: "20 Hz to 20 kHz across.", text: "The shaded corridor is where this kind of track usually sits; the bright line is your track with the moves you have taken, the dotted line is how it came in. It shows the section whose key is lit, or the whole song. Anything outside the corridor is named." },
crt_loud: { name: "loudness screen", range: "−16 to −6 LUFS across. transients kept, up.", text: "How much of the drums' snap survives as the limiter pushes the song louder. Past the knee it falls away fast; GUARD stops the limiter just before it. Loudness is set once for the whole song, so it never jumps between sections." },
crt_moves: { name: "moves screen", range: "click a move to take or skip it.", text: "Every suggestion from the listening pass, with its reason, the section it's for and how sure the assistant is. A taken move sets the knobs in that section; AMOUNT scales them all. Try skipping one, then look at BALANCE, CHECKS or SECTIONS." },
crt_checks: { name: "checks screen", range: "run it before you bounce.", text: "The preflight: true peak after encoding, phase, mono, rumble, DC and clean starts and ends. LOOK means something needs your ears. A mix note means the fix belongs in the mix, not the master." },
crt_sect: { name: "sections screen", range: "the whole song, left to right.", text: "Each block is a marked section; the lit ones belong to the section key that's lit. Click a block to edit its section, drag its left edge to move its mark, double-click it to remove the mark, right-click it to change its section. Anything not listed below follows the song." },
key_bal: { name: "balance", range: "screen key. click it.", text: "Shows the tonal balance: your track against the target corridor, with anything outside it named." },
key_loud: { name: "loudness", range: "screen key. click it.", text: "Shows how much punch the song keeps as it gets louder, where the damage starts, and what streaming will do to it." },
key_moves: { name: "moves", range: "screen key. click it.", text: "Shows the assistant's suggested moves. Take or skip each one; nothing changes until you do." },
key_checks: { name: "checks", range: "screen key. click it.", text: "Shows the preflight checks and any note about something that should be fixed in the mix instead." },
key_sect: { name: "sections", range: "screen key. click it.", text: "Shows the song's sections and what each one changes from the song settings." },
sec_song: { name: "song", range: "section key. click it.", text: "Edit the settings for the whole song. Every section starts from these, and anything a section hasn't changed follows them." },
sec_mark: { name: "mark", range: "button. lit while marking. click it.", text: "Press MARK, play the song and tap a number as each section starts. Sections with the same number share their settings, so every chorus sounds alike. Press MARK again to finish." },
sect_tape: { name: "section names", range: "masking tape. click it to rename.", text: "Your names for the five section keys. Write your own, like pre-chorus or solo; the SECTIONS screen uses them too." },
latch: { name: "space latch", range: "quarter-turn. click it.", text: "Lets the hidden SPACE tray slide out from under the desk: long player's subtle space, under the whole mix. Click again to put it away. It keeps working while it's hidden." },
space_on: { name: "space lamp", range: "lit or dark. lit now.", text: "Lit while the hidden SPACE tray is adding something, so you know it's working even when it's put away." },
tray_plate: { name: "space tray", range: "set per section.", text: "The same space as long player: a level-matched, deliberately subtle reverb under the mix, between STEREO and LOUDNESS. Like everything else, a section can have its own settings." },
tray_table: { name: "type controls", range: "what a and b do for each type.", text: "A and B change job with the type: clean has diffusion and early reflections, studio damping and tail movement, plate damping and brightness, spring tension and drip, chamber absorption and size, room size and early reflections." },
sp_in: { name: "in", range: "lit or dark. lit now.", text: "Turns the space on or off. It can be on in some sections and off in others." },
sp_type: { name: "type", range: "clean, studio, plate, spring, chamber or room. set to plate.", text: "The kind of space. Every type is level-matched and made to stay subtle. A change of type lets the old tail ring out under the new one." },
sp_amount: { name: "amount", range: "−60 to −15 db. set to −32 db.", text: "How loud the space sits under the mix. Around −30 it's felt more than heard." },
sp_decay: { name: "decay", range: "0.3 to 2.5 s. set to 1.4 s.", text: "How long the tail lasts." },
sp_tone: { name: "tone", range: "dark to bright. set to the middle.", text: "Darkens or brightens the space." },
sp_pre: { name: "pre-delay", range: "0 to 100 ms. set to 20 ms.", text: "A gap before the space starts, to keep the dry hits clear." },
sp_low: { name: "low cut", range: "100 to 600 hz. set to 200 hz.", text: "Keeps low end out of the space, so the bass and kick stay tight." },
sp_high: { name: "high cut", range: "2 to 16 khz. set to 8 khz.", text: "Softens the top of the space." },
sp_gap: { name: "gap-fill", range: "0 to 100 percent. set to 60.", text: "The space ducks under the music and blooms in the gaps between phrases." },
sp_apply: { name: "apply to", range: "mid, side or both. set to side.", text: "Where the space sits. Side keeps the centre, the vocal, kick and bass, dry." },
sp_a: { name: "a", range: "0 to 10. set to 4.", text: "The type's first control. With plate it's damping: how fast the highs die away. The table on the right says what A does for each type." },
sp_b: { name: "b", range: "0 to 10. set to 6.", text: "The type's second control. With plate it's bright: the plate's sheen. The table on the right says what B does for each type." },
a_target: { name: "target", range: "genre, refs or mine. set to genre.", text: "What the assistant compares your track with. Genre uses a profile you pick on the screen. Refs uses reference tracks you drag onto the screen. Mine uses your own past masters, so suggestions lean toward your sound." },
a_dest: { name: "destination", range: "stream, cd, vinyl or club. set to stream.", text: "Where the master is going. It sets the loudness target, ceiling and dither, and which checks run: vinyl adds low-end and sibilance checks, CD sets 16-bit dither." },
a_amount: { name: "amount", range: "0 to 100 percent. set to 70.", text: "Scales every move you have taken, all at once. 100 makes the full suggested moves; 50 goes halfway. Start low and turn it up until it stops getting better." },
a_learn: { name: "learn", range: "on or off. set to on.", text: "On: Lacquer remembers which suggestions you take and skip, and its next suggestions lean toward your taste. Off: it only uses the target." },
a_listen: { name: "listen", range: "button. lit while it listens.", text: "Play the song through once. Lacquer listens section by section, so a move can be just for the choruses, then puts its suggestions on the MOVES screen." },
a_take: { name: "take all", range: "button.", text: "Takes every suggested move at once. You can still skip any of them on the MOVES screen." },
idea: { name: "idea lamp", range: "lit or dark.", text: "Lit while one of the assistant's moves is working in this module. The MOVES screen says what each move does and which section it's for." },
in_: { name: "in", range: "lit or dark.", text: "Turns this module on or off without losing its settings. It can be in for some sections and out for others. While MATCHED is lit the switch is loudness-matched, so you hear the change and not a level jump." },
c_res: { name: "resonance", range: "0 to 10. set to 3.5.", text: "Tames narrow ringing frequencies that build up across the whole mix and leaves everything around them alone." },
c_harsh: { name: "harsh", range: "0 to 10. set to 2.5.", text: "A cut in the 2 to 6 kHz range that only works when the music gets harsh, like a loud chorus, and leaves quieter parts alone." },
c_rumble: { name: "rumble", range: "off, or 10 to 80 Hz. set to 25 Hz.", text: "Cuts sub-bass rumble below the frequency you set, with a gentle slope so the kick keeps its weight." },
c_hear: { name: "hear", range: "removed or result. set to result.", text: "Removed lets you hear only what this module is taking out, so you can check it isn't eating the music." },
t_low: { name: "low", range: "−6 to +6 db. set to 0.", text: "A broad, gentle low shelf for the weight of the whole track." },
t_pres: { name: "presence", range: "−6 to +6 db. set to −0.8 db.", text: "A wide bell around 2.5 kHz, where vocals and guitars come forward or sit back." },
t_air: { name: "air", range: "−6 to +6 db. set to +1.5 db.", text: "A soft high shelf above about 10 kHz, for openness and sparkle." },
t_mode: { name: "mode", range: "dynamic or static. set to dynamic.", text: "Dynamic: each tone move only works as hard as the music needs at that moment. Static: a fixed curve, like a classic mastering EQ." },
d_glue: { name: "glue", range: "0 to 10. set to 4.", text: "A slow bus compressor that holds the mix together. One or two dB of gain reduction is usually plenty." },
d_punch: { name: "punch", range: "less to more, 0 in the middle. set a little past 0.", text: "Brings drum hits forward or rounds them off, without changing the overall level." },
d_even: { name: "even", range: "0 to 10. set to 2.", text: "A gentle multiband compressor that keeps the lows, mids and highs in the same balance through the song." },
d_chan: { name: "channels", range: "m / s or l / r. set to l / r.", text: "M / S compresses the middle and the sides separately, which can steady a wide mix. L / R treats both sides together." },
c_drive: { name: "drive", range: "0 to 10. set to 3.", text: "How hard the colour stage is pushed. Low settings add weight and warmth; high settings, audible saturation." },
c_shade: { name: "shade", range: "dark to bright. set just left of centre.", text: "Tilts the saturation's harmonics darker or brighter, so you can add warmth without fizz." },
c_mix: { name: "mix", range: "0 to 100 percent. set to 60.", text: "Blends the coloured signal with the clean one." },
c_type: { name: "type", range: "tape, iron or valve. set to tape.", text: "Tape softens peaks and rounds the top. Iron adds transformer weight in the lows. Valve adds a little warm second harmonic." },
s_width: { name: "width", range: "0 to 200 percent. set to 108.", text: "How wide the stereo picture is. Below 100 narrows it, above widens it. BASS MONO keeps the low end centred whatever this is set to." },
s_mono: { name: "bass mono", range: "off, or 40 to 300 Hz. set to 120 Hz.", text: "Puts everything below this frequency in the centre, which keeps the low end solid in clubs and safe on vinyl." },
s_centre: { name: "centre", range: "−3 to +3 db. set to 0.", text: "Lifts or lowers the middle of the stereo picture against the sides: a quick way to bring the vocal and kick forward." },
s_hear: { name: "hear", range: "mono or stereo. set to stereo.", text: "Mono folds the output to mono so you can check that nothing disappears." },
l_target: { name: "target", range: "−16 to −6 lufs. set to −9.4.", text: "The loudness of the whole song that the limiter aims for. Someone has ringed −11 in grease pencil and written plenty." + SONGWIDE },
l_ceiling: { name: "ceiling", range: "−2 to 0 dbtp. set to −1.0.", text: "The highest true peak allowed, measured between samples, so streaming encoders have room." + SONGWIDE },
l_clip: { name: "clipper", range: "0 to 10. set to 3.", text: "How much of the loudness comes from a soft clipper before the limiter. A little clipping keeps drums punchier than limiting alone." + SONGWIDE },
l_guard: { name: "guard", range: "on or off. set to on.", text: "On: the limiter stops pushing at the point where the drums start to smear, even if the target asks for more." + SONGWIDE },
m_loud: { name: "loudness meter", range: "−24 to −6 lufs, short-term.", text: "How loud the last three seconds were, measured the way streaming services measure it. The red band is where most songs start to lose punch." },
m_punch: { name: "punch meter", range: "3 to 18 db plr.", text: "How far the peaks stand above the average loudness. Higher is punchier; the red band at the left means squashed." },
guarding: { name: "guard lamp", range: "lit or dark. lit now.", text: "Lit while GUARD is stopping the limiter from pushing any harder." },
over: { name: "over lamp", range: "lit or dark. dark now.", text: "Lights if a true peak goes over the ceiling, including after the codec preview." },
r_lufs: { name: "integrated", range: "lufs, the whole song so far.", text: "The loudness of the whole song: the figure a streaming service uses to decide how far to turn it down." },
r_tp: { name: "true peak", range: "dbtp, the highest so far.", text: "The highest peak, including the ones that happen between samples. It should stay at or under the ceiling." },
o_listen: { name: "listen on", range: "desk, phone, buds, car or club. set to desk.", text: "Hear the master the way it will sound elsewhere: a phone speaker, earbuds, a car or a club system. Only what you hear changes; the master is untouched." },
o_codec: { name: "codec", range: "on or off. set to off.", text: "On: you hear the master after streaming encoding, and the true peak reading includes the encoder's overs." },
o_dither: { name: "dither", range: "off, 24 or 16 bit. set to 24.", text: "Dither for the final bit depth. The destination sets it for you; 16 is for CD." },
o_monitor: { name: "monitor", range: "−30 to 0 db. set to −8 db.", text: "Your listening level only. It never changes the master, so you can check at a quiet level without fooling yourself." },
o_match: { name: "matched", range: "lit or dark. lit now.", text: "While lit, every IN button is loudness-matched: switching a module out leaves its level behind, so you hear what it does and not a level jump." },
o_blind: { name: "blind", range: "button.", text: "A blind shoot-out: A and B are shuffled. Switch between them by ear, then press BLIND again to see which one you picked." },
power: { name: "power lamp", range: "lit or dark.", text: "Lit while Lacquer is processing. It goes out when the plugin is bypassed." },
preset: { name: "preset", range: "factory and your own.", text: "The preset's name is written on the tape. Click it for the list, or step through with the arrows. A preset holds the song settings and every section's." },
compare: { name: "compare", range: "a or b.", text: "Two complete settings to flip between: the song's and every section's." },
menu: { name: "set", range: "button.", text: "Opens the settings: save and load presets, remember this master for MINE, clear the references, and the window size." },
};
const st = this.state || {};
const page = this.pageNow();
const taken = this.takenNow();
const sec = this.secNow();
const marking = !!st.mark;
const SN = ["INTRO", "VERSE", "CHORUS", "BRIDGE", "OUTRO"];
[1, 2, 3, 4, 5].forEach((k) => {
T["sec_" + k] = { name: "section " + k + "  ·  " + SN[k - 1].toLowerCase(), range: "section key. click it.",
text: "Edit the " + SN[k - 1].toLowerCase() + " on its own: knobs you turn while this key is lit change only the " + SN[k - 1].toLowerCase() + ", and the rest follow the song. While the song plays, the key for the section you're in lights by itself and the knobs move to its settings." };
});
const IDEA_ON = { clean: true, tone: true, loud: true };
["clean", "tone", "dyn", "col", "st", "loud"].forEach((c) => {
T[c + "_idea"] = { name: T.idea.name, range: IDEA_ON[c] ? "lit or dark. lit now." : "lit or dark. dark now.", text: T.idea.text };
T[c + "_in"] = { name: T.in_.name, range: c === "st" ? "lit or dark. dark now." : "lit or dark. lit now.", text: T.in_.text };
});
const nTaken = MOVES.filter((m) => taken[m.k]).length;
const bal = this.balance(taken, sec);
const ld = this.loudness(taken);
const sc = this.sections(taken, sec, marking);
const secLabel = sec === "song" ? "WHOLE SONG" : SN[sec - 1];
const rows = MOVES.map((m, i) => {
const top = 50 + i * 25;
const on = !!taken[m.k];
const segs = [0, 1, 2, 3, 4].map((j) => ({ x: (318 + j * 6.5).toFixed(1), cls: (m.conf >= 20 * (j + 1) - 10) ? "seg-on" : "seg-off" }));
return { n: m.n, sec: m.sec, what: m.what, why: m.why, scope: m.scope, pct: m.conf + "%", on: on, off: !on, cls: on ? "" : "dim-row",
by: (top + 1).toFixed(1), tick: "M" + 14.5 + " " + (top + 6.5) + " L" + 17 + " " + (top + 9) + " L" + 21 + " " + (top + 3.5),
y1: (top + 10).toFixed(1), y2: (top + 22).toFixed(1), sy: (top + 3.5).toFixed(1), segs: segs,
aria: "move " + m.n + ", " + m.sec.toLowerCase() + ", " + m.scope.toLowerCase() + ": " + (on ? "taken" : "skipped"), pressed: on ? "true" : "false",
hit: "left:" + (@@SX@@ + 8) + "px;top:" + (@@SY@@ + top - 2) + "px;width:364px;height:25px",
toggle: () => { const t = Object.assign({}, this.takenNow()); t[m.k] = !t[m.k]; this.setState({ taken: t, hover: "row" + m.n }); },
hover: () => this.setState({ hover: "row" + m.n }) };
});
const ckRows = [
{ name: "TRUE PEAK", what: "−1.0 dBTP, still under after encoding", ok: true },
{ name: "PHASE", what: "correlation stays above +0.3", ok: true },
{ name: "MONO", what: taken.m6 ? "low end centred below 120 Hz (move 6)" : "the low end drifts off centre below 120 Hz", ok: !!taken.m6 },
{ name: "RUMBLE", what: taken.m4 ? "cut below 25 Hz (move 4)" : "rumble at 22 Hz under the kick", ok: !!taken.m4 },
{ name: "DC OFFSET", what: "none", ok: true },
{ name: "START AND END", what: "clean, no clicks, half a second of room", ok: true }
].map((c, i) => ({ name: c.name, what: c.what, ok: c.ok, look: !c.ok, y: (60 + i * 17).toFixed(1), ty: (60 + i * 17 - 10.5).toFixed(1) }));
const nLook = ckRows.filter((c) => c.look).length;
let scr;
if (page === "loud") {
scr = { h1: "NOW " + ld.fmt(ld.now) + " LUFS   ·   " + ld.kept + "% OF TRANSIENTS KEPT",
h2: sec !== "song" ? "SET FOR THE WHOLE SONG: THE SAME IN EVERY SECTION" : (taken.m5 ? "GUARD STOPS THE LIMITER JUST BEFORE THE KNEE" : "PAST THE KNEE: THE DRUMS ARE STARTING TO SMEAR"), label: "Loudness screen" };
} else if (page === "moves") {
scr = { h1: "MOVES   " + MOVES.length + " SUGGESTED   ·   " + nTaken + " TAKEN", h2: "AMOUNT 70%   ·   CLICK A MOVE TO TAKE OR SKIP IT", label: "Moves screen" };
} else if (page === "checks") {
scr = { h1: "CHECKS   BEFORE YOU BOUNCE", h2: (6 - nLook) + " OK   ·   " + (nLook ? nLook + " TO LOOK AT" : "NOTHING TO LOOK AT") + "   ·   1 MIX NOTE", label: "Checks screen" };
} else if (page === "sect") {
scr = marking ? { h1: "SECTIONS   MARKING", h2: "PLAY THE SONG AND TAP A NUMBER AS EACH SECTION STARTS", label: "Sections screen" }
: { h1: "SECTIONS   EDITING " + (sec === "song" ? "THE WHOLE SONG" : sec + " " + SN[sec - 1]), h2: "9 SECTIONS MARKED   ·   LOUDNESS STAYS THE SAME", label: "Sections screen" };
} else {
scr = { h1: "BALANCE   " + secLabel + "   ·   TARGET INDIE ROCK",
h2: bal.inside + "% INSIDE THE CORRIDOR   ·   " + (bal.nOut ? bal.nOut + (bal.nOut > 1 ? " AREAS" : " AREA") + " OUTSIDE" : "NOTHING OUTSIDE"), label: "Balance screen" };
}
const idle = { name: "point at a control", range: "", text: "Hover over anything and this strip says what it does. Press the SCREEN keys to change the tube, and the SECTIONS keys to edit one part of the song. On MOVES, click a suggestion to take or skip it. The quarter-turn latch at the bottom left lets out the hidden SPACE tray." };
const cur = st.hover || "";
let help = T[cur] || idle;
if (cur === "crt") help = T["crt_" + page];
if (cur.indexOf("row") === 0) {
const m = MOVES[Number(cur.slice(3)) - 1];
const on = !!taken[m.k];
help = { name: "move " + m.n + "  ·  " + m.sec.toLowerCase(), range: (on ? "taken" : "skipped") + ". " + (m.scope === "SONG" ? "whole song" : m.scope.toLowerCase() + " only") + ". " + m.conf + "% sure.",
text: m.why + " Taking it sets the " + m.sec + " knobs " + (m.scope === "SONG" ? "for the whole song" : "in the " + m.scope.toLowerCase() + " only") + ", scaled by AMOUNT; skipping it leaves them where they were." };
}
const hov = {};
Object.keys(T).forEach((k) => { hov[k] = () => this.setState({ hover: k }); });
hov.crt = () => this.setState({ hover: "crt" });
const go = {};
["bal", "loud", "moves", "checks", "sect"].forEach((p) => { go[p] = () => this.setState({ page: p, hover: "crt" }); });
const pg = { bal: page === "bal", loud: page === "loud", moves: page === "moves", checks: page === "checks", sect: page === "sect" };
const secv = { marking: marking, markPressed: marking ? "true" : "false",
toggleMark: () => this.setState({ mark: !(this.state || {}).mark, page: "sect", hover: "sec_mark" }) };
["song", 1, 2, 3, 4, 5].forEach((k) => {
secv["on_" + k] = sec === k;
secv["pressed_" + k] = sec === k ? "true" : "false";
secv["pick_" + k] = () => this.setState({ sec: k, hover: "sec_" + k });
});
const trayOpen = !!st.tray;
const tray = { open: trayOpen, expanded: trayOpen ? "true" : "false", toggle: () => this.setState({ tray: !(this.state || {}).tray, hover: "latch" }) };
return { help: help, hov: hov, go: go, pg: pg, scr: scr, bal: bal, ld: ld, sc: sc, sec: secv, tray: tray,
mv: { rows: rows }, ck: { rows: ckRows }, leave: () => this.setState({ hover: "" }) };
}
}
