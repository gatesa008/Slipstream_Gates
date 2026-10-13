// Stream audio arrives at 48 kHz. The context may run at another rate, and even at 48 kHz the
// host and client clocks drift apart, so playback goes through a variable-rate reader that
// nudges speed (at most +/-0.5%, inaudible) to hold the buffer at a jitter-adaptive target
// instead of hard-skipping or stalling.
const SRC_RATE = 48000;
const ms = v => Math.floor(SRC_RATE * v / 1000);

const TARGET_INIT = ms(60), TARGET_MIN = ms(30), TARGET_MAX = ms(180);
const TARGET_STEP_UP = ms(10), TARGET_STEP_DOWN = ms(2);
const MAX_RATE_ADJUST = 0.005, RATE_GAIN = 0.02, RATE_DEADBAND = 0.05;
const FADE = ms(2);
const MAX_CONCEAL = ms(80);

class RingBuffer {
    constructor(channels, capacity) {
        this.ch = channels; this.cap = capacity;
        this.bufs = []; for (let i = 0; i < channels; i++) this.bufs.push(new Float32Array(capacity));
        this.rp = 0; this.wp = 0; this.avail = 0;
    }
    write(channelData, frames, gainAt) {
        if (frames <= 0) return { written: 0, overflow: 0 };
        let overflow = 0, start = 0;
        if (frames > this.cap) { overflow = frames - this.cap; start = overflow; frames = this.cap; }
        const space = this.cap - this.avail;
        if (frames > space) { overflow += frames - space; this.skip(frames - space); }
        for (let c = 0; c < this.ch; c++) {
            const src = channelData[c] || channelData[0];
            const dst = this.bufs[c];
            let w = this.wp;
            for (let i = 0; i < frames; i++) {
                dst[w] = gainAt ? src[start + i] * gainAt(i) : src[start + i];
                if (++w === this.cap) w = 0;
            }
        }
        this.wp = (this.wp + frames) % this.cap; this.avail += frames;
        return { written: frames, overflow };
    }
    // Sample at offset from the read position; negative offsets read already-played history.
    at(c, offset) {
        let i = this.rp + offset;
        if (i < 0) i += this.cap; else if (i >= this.cap) i -= this.cap;
        return this.bufs[c][i];
    }
    // Sample relative to the write position (offset < 0 is the most recently written data).
    fromWrite(c, offset) {
        let i = (this.wp + offset) % this.cap;
        if (i < 0) i += this.cap;
        return this.bufs[c][i];
    }
    scaleTail(frames, gainAt) {
        const n = Math.min(frames, this.avail);
        for (let c = 0; c < this.ch; c++) {
            for (let k = 0; k < n; k++) {
                let i = this.wp - n + k;
                if (i < 0) i += this.cap;
                this.bufs[c][i] *= gainAt(k, n);
            }
        }
    }
    skip(n) { const toSkip = Math.min(n, this.avail); if (toSkip > 0) { this.rp = (this.rp + toSkip) % this.cap; this.avail -= toSkip; } return toSkip; }
    get length() { return this.avail; }
    clear() {
        this.rp = 0; this.wp = 0; this.avail = 0;
        for (const b of this.bufs) b.fill(0);
    }
}

// 4-point Hermite: transparent at frac=0 and far less high-frequency loss than linear.
const hermite = (xm1, x0, x1, x2, t) => {
    const c1 = 0.5 * (x1 - xm1);
    const c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2;
    const c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
};

class StreamAudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.rb = new RingBuffer(2, SRC_RATE);
        this.baseStep = SRC_RATE / sampleRate;
        this.volume = 1; this.muted = false;
        this.resetPlayback();
        this.samplesProcessed = 0; this.lastReport = 0; this.lastUnderrunAt = 0; this.lastTargetDecay = 0;
        this.underruns = 0; this.overflows = 0; this.concealed = 0;
        this.port.onmessage = e => {
            const { type, data } = e.data;
            if (type === 'audio') this.onAudio(data.channels, data.frames);
            else if (type === 'gap') this.conceal(e.data.frames);
            else if (type === 'volume') this.volume = Math.max(0, Math.min(1, data));
            else if (type === 'mute') this.muted = data;
            else if (type === 'clear') {
                this.rb.clear();
                this.resetPlayback();
                this.underruns = 0; this.overflows = 0; this.concealed = 0;
            }
        };
        this.port.postMessage({ type: 'ready' });
    }

    resetPlayback() {
        this.target = TARGET_INIT;
        this.level = TARGET_INIT;
        this.frac = 0;
        this.prebuffering = true;
        this.fadeIn = 0;
        this.fadeInNextWrite = false;
        this.lastOut = [0, 0];
    }

    get maxLevel() { return this.target * 2 + ms(40); }

    onAudio(channels, frames) {
        let gainAt = null;
        if (this.fadeInNextWrite) {
            this.fadeInNextWrite = false;
            gainAt = i => (i < FADE ? (i + 1) / FADE : 1);
        }
        const result = this.rb.write(channels, frames, gainAt);
        if (result.overflow > 0) { this.overflows++; this.port.postMessage({ type: 'drop', reason: 'Write overflow' }); }
    }

    // Lost packets: replay the last packet-length of audio under a fade-out, then fade the next
    // real packet in. Keeps buffer timing intact and avoids the click of a hard discontinuity.
    conceal(frames) {
        const n = Math.min(frames, MAX_CONCEAL);
        if (n <= 0 || this.rb.length + n >= this.rb.cap) return;
        const period = ms(10);
        const channels = [];
        for (let c = 0; c < this.rb.ch; c++) {
            const out = new Float32Array(n);
            for (let i = 0; i < n; i++) out[i] = this.rb.fromWrite(c, -period + (i % period)) * (1 - (i + 1) / n);
            channels.push(out);
        }
        // Smooth the seam into the repeated block, then write it.
        this.rb.scaleTail(FADE, (k, len) => 1 - 0.5 * (k + 1) / len);
        this.rb.write(channels, n, i => (i < FADE ? 0.5 + 0.5 * (i + 1) / FADE : 1));
        this.fadeInNextWrite = true;
        this.concealed++;
    }

    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || !out.length) return true;
        const frames = out[0].length;
        const chs = Math.min(out.length, this.rb.ch);
        this.samplesProcessed += frames;

        if (this.muted || this.prebuffering) {
            for (let c = 0; c < out.length; c++) out[c].fill(0);
            if (!this.muted && this.rb.length >= Math.floor(this.target * 0.8)) {
                this.prebuffering = false;
                this.fadeIn = FADE;
                this.level = this.rb.length;
            }
            this.report();
            return true;
        }

        // Far behind (tab was throttled, burst after a stall): jump to the target, fade back in.
        if (this.rb.length > this.maxLevel) {
            this.rb.skip(this.rb.length - this.target);
            this.level = this.target;
            this.frac = 0;
            this.fadeIn = FADE;
            this.overflows++;
            this.port.postMessage({ type: 'drop', reason: 'Catch-up' });
        }

        this.level += (this.rb.length - this.level) * 0.02;
        const err = (this.level - this.target) / this.target;
        const adjust = Math.abs(err) < RATE_DEADBAND ? 0
            : Math.max(-MAX_RATE_ADJUST, Math.min(MAX_RATE_ADJUST, err * RATE_GAIN));
        const step = this.baseStep * (1 + adjust);

        let i = 0;
        for (; i < frames; i++) {
            if (this.rb.length < 3) break;
            let g = this.volume;
            if (this.fadeIn > 0) { g *= 1 - this.fadeIn / FADE; this.fadeIn--; }
            const t = this.frac;
            for (let c = 0; c < chs; c++) {
                const y = hermite(this.rb.at(c, -1), this.rb.at(c, 0), this.rb.at(c, 1), this.rb.at(c, 2), t) * g;
                out[c][i] = y;
                this.lastOut[c] = y;
            }
            this.frac += step;
            const adv = Math.floor(this.frac);
            if (adv > 0) { this.rb.skip(adv); this.frac -= adv; }
        }
        for (let c = chs; c < out.length; c++) out[c].set(out[0]);

        if (i < frames) {
            // Underrun: ramp from the last sample to silence instead of holding a DC level.
            const remaining = frames - i;
            for (let c = 0; c < out.length; c++) {
                const from = this.lastOut[Math.min(c, chs - 1)] || 0;
                for (let k = 0; k < remaining; k++) out[c][i + k] = from * (1 - (k + 1) / remaining);
            }
            this.lastOut = [0, 0];
            this.underruns++;
            this.port.postMessage({ type: 'drop', reason: 'Underrun' });
            // Network jitter outran the buffer: hold more before resuming.
            this.target = Math.min(TARGET_MAX, this.target + TARGET_STEP_UP);
            this.prebuffering = true;
            this.lastUnderrunAt = this.samplesProcessed;
        } else if (this.samplesProcessed - this.lastUnderrunAt > SRC_RATE * 10 &&
                   this.samplesProcessed - this.lastTargetDecay > SRC_RATE * 5) {
            // Stable for a while: creep latency back down.
            this.lastTargetDecay = this.samplesProcessed;
            this.target = Math.max(TARGET_MIN, this.target - TARGET_STEP_DOWN);
        }

        this.report();
        return true;
    }

    report() {
        if (this.samplesProcessed - this.lastReport < 4800) return;
        this.port.postMessage({
            type: 'stats',
            bufferMs: (this.rb.length / SRC_RATE) * 1000,
            targetMs: (this.target / SRC_RATE) * 1000,
            underruns: this.underruns, overflows: this.overflows, concealed: this.concealed
        });
        this.lastReport = this.samplesProcessed; this.underruns = 0; this.overflows = 0; this.concealed = 0;
    }
}
registerProcessor('stream-audio-processor', StreamAudioProcessor);
