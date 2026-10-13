
import { C, MSG, CODECS } from './constants.js';
import { S, $, mkBuf, log, safe, safeAsync,
    recordDecodeTime, recordAudioPacket, recordAudioDecoded, recordAudioBufferHealth,
    recordAudioUnderrun, recordAudioOverflow, logVideoDrop, logAudioDrop } from './state.js';
import { queueFrameForPresentation } from './renderer.js';
import { requestKeyframe, requestRecoveryKeyframe, sendAudioEnable, handleDecodePressure, requestCodecFallback } from './protocol.js';
let kfRetryInterval = null;
let lastCaptureTs = 0;
let workletNode = null;
let workletReady = false;
let lastWaitingKeyLogAt = 0;
let decodeQueuePressureCount = 0;
let lastDecodeQueuePressureAt = 0;

const notifyDecodeQueuePressure = queueSize => {
    const now = performance.now();
    if (now - lastDecodeQueuePressureAt > 1500) {
        decodeQueuePressureCount = 0;
    }
    lastDecodeQueuePressureAt = now;
    decodeQueuePressureCount++;

    log.warn('MEDIA', 'Decode queue pressure', {
        queueSize, pressureCount: decodeQueuePressureCount, threshold: 12
    });

    if (decodeQueuePressureCount >= 3 && !S.needKey) {
        log.warn('MEDIA', 'Sustained decode pressure - requesting recovery keyframe', {
            queueSize, pressureCount: decodeQueuePressureCount
        });
        requestRecoveryKeyframe('decode-pressure');
    }

    if (decodeQueuePressureCount >= 12) {
        decodeQueuePressureCount = 0;
        log.error('MEDIA', 'Decode queue pressure threshold exceeded - triggering adaptive quality');
        handleDecodePressure({ reason: 'decode-queue-full', queueSize });
    }
};

const stopKeyframeRetry = () => {
    if (kfRetryInterval) {
        clearInterval(kfRetryInterval);
        kfRetryInterval = null;
        log.debug('MEDIA', 'Keyframe retry stopped');
    }
};

const startKeyframeRetry = () => {
    if (kfRetryInterval) return;
    kfRetryInterval = setInterval(() => {
        if (S.needKey && S.ready) {
            log.debug('MEDIA', 'Requesting keyframe (retry)');
            requestKeyframe('retry');
        } else if (!S.needKey) {
            stopKeyframeRetry();
        }
    }, C.KEY_RETRY_INTERVAL_MS);
    log.debug('MEDIA', 'Keyframe retry started');
};
const tryReinitDecoder = () => {
    if (!S.reinit) {
        S.reinit = 1;
        log.info('MEDIA', 'Scheduling decoder reinit');
        setTimeout(async () => {
            await initDecoder(false, S.decoderCodec ?? S.currentCodec);
            S.reinit = 0;
        }, 100);
    }
};
// Max frames allowed in the WebCodecs decode submission queue before deltas are dropped for
// freshness. Kept low because queue depth is latency; a healthy client rarely exceeds 1.
const DECODE_QUEUE_DROP_THRESHOLD = 4;
const CODEC_MAP = Object.fromEntries(Object.values(CODECS).map(c => [c.id, c.codec]));

// Supported decoder configs per codec string, so a mid-stream codec switch can configure
// the new decoder without another async isConfigSupported round trip.
const decoderConfigCache = new Map();
// Keyframe that arrived while the decoder for its codec was being (re)initialized.
let pendingKeyframe = null;
let decoderInitSeq = 0;

const resolveDecoderConfig = async codec => {
    const cached = decoderConfigCache.get(codec);
    if (cached) return cached;
    for (const [preferHw, label] of [[true, 'HW'], [false, 'SW']]) {
        const config = {
            codec,
            optimizeForLatency: true,
            latencyMode: 'realtime',
            hardwareAcceleration: preferHw ? 'prefer-hardware' : 'prefer-software'
        };
        const supported = await safeAsync(() => VideoDecoder.isConfigSupported(config), null, 'MEDIA');
        if (supported?.supported) {
            const entry = { config: supported.config, accel: label };
            decoderConfigCache.set(codec, entry);
            return entry;
        }
    }
    return null;
};

const createVideoDecoder = codec => new VideoDecoder({
    output: frame => {
        const now = performance.now();
        const meta = S.frameMeta.get(frame.timestamp);

        if (meta?.decodeStartMs) {
            const decodeTime = now - meta.decodeStartMs;
            recordDecodeTime(decodeTime, S.decoder?.decodeQueueSize || 0);
            if (decodeTime > 50) {
                log.warn('MEDIA', 'Slow decode', {
                    decodeMs: decodeTime.toFixed(1),
                    queueSize: S.decoder?.decodeQueueSize || 0,
                    w: frame.displayWidth, h: frame.displayHeight,
                    ts: frame.timestamp
                });
            }
        }

        log.debug('MEDIA', 'Frame decoded', {
            ts: frame.timestamp, w: frame.displayWidth, h: frame.displayHeight,
            queueSize: S.decoder?.decodeQueueSize || 0
        });
        if (meta) meta.decodeOutputMs = now;

        queueFrameForPresentation({
            frame,
            meta,
            queuedAt: now,
            timestamp: frame.timestamp,
            sourceTs: meta?.sourceTs || meta?.capTs || frame.timestamp
        });
    },
    error: e => {
        log.error('MEDIA', 'Decoder error', {
            error: e.message, codec,
            decoderState: S.decoder?.state,
            queueSize: S.decoder?.decodeQueueSize || 0,
            decodeErrors: S.stats.decodeErrors
        });
        S.stats.decodeErrors++;
        S.ready = 1;
        S.needKey = 1;
        requestKeyframe('decoder-error');
        startKeyframeRetry();
        tryReinitDecoder();
    }
});

export const initDecoder = async (force = false, codecId = S.currentCodec) => {
    if (!window.VideoDecoder) {
        logVideoDrop('VideoDecoder API not available');
        return;
    }

    const seq = ++decoderInitSeq;
    S.ready = 0;
    S.needKey = 1;
    S.decoderCodec = codecId;
    S.decoderGeneration = null;
    lastCaptureTs = 0;
    decodeQueuePressureCount = 0;
    lastDecodeQueuePressureAt = 0;

    if (force) log.info('MEDIA', 'Force reinitializing decoder');
    if (S.decoder && S.decoder.state !== 'closed') {
        safe(() => S.decoder.close(), undefined, 'MEDIA');
    }
    S.decoder = null;

    const codec = CODEC_MAP[codecId] || CODEC_MAP[2];
    log.info('MEDIA', 'Initializing decoder', { codec, codecId });

    const resolved = await resolveDecoderConfig(codec);
    if (seq !== decoderInitSeq) return;  // superseded by a newer init (e.g. another codec switch)

    if (!resolved) {
        S.hwAccel = 'NONE';
        log.error('MEDIA', 'Decoder configuration failed', { codec });
        logVideoDrop('Decoder configuration failed', { codec });
        requestCodecFallback(codecId);
        return;
    }

    const decoder = S.decoder = createVideoDecoder(codec);
    const configured = safe(() => { decoder.configure(resolved.config); return true; }, false, 'MEDIA');
    if (!configured || decoder.state !== 'configured') {
        decoderConfigCache.delete(codec);
        S.hwAccel = 'NONE';
        log.error('MEDIA', 'Decoder configuration failed', { codec, state: decoder.state });
        logVideoDrop('Decoder configuration failed', { codec });
        requestCodecFallback(codecId);
        return;
    }

    S.hwAccel = resolved.accel;
    S.ready = 1;
    log.info('MEDIA', 'Decoder configured', { codec, accel: resolved.accel });

    const stashed = pendingKeyframe;
    pendingKeyframe = null;
    if (stashed && stashed.codec === codecId && decodeFrame(stashed)) return;
    requestKeyframe('decoder-init');
    startKeyframeRetry();
};

export const resetVideoDecoderState = () => {
    decoderInitSeq++;
    pendingKeyframe = null;
    S.decoderCodec = null;
    S.decoderGeneration = null;
};

export const decodeFrame = data => {
    // The host tags every frame with its codec, so a switch is driven by the stream itself:
    // stale frames from the old codec are dropped and the first new keyframe is kept.
    const frameCodec = data.codec ?? S.decoderCodec ?? S.currentCodec;
    if (frameCodec !== S.decoderCodec) {
        // Only a keyframe may switch codecs. A stray delta is either a straggler from the codec
        // we just left (tearing the new decoder down for it would thrash) or the new codec's
        // keyframe was lost, in which case ask for another.
        if (!data.isKey && S.decoderCodec != null) {
            const now = performance.now();
            if (now - lastWaitingKeyLogAt >= 1000) {
                logVideoDrop('Delta frame for another codec', { frameCodec, decoderCodec: S.decoderCodec });
                lastWaitingKeyLogAt = now;
            }
            if (frameCodec === S.currentCodec) requestKeyframe('codec-mismatch');
            return false;
        }
        if (data.isKey) pendingKeyframe = { ...data, codec: frameCodec };
        log.info('MEDIA', 'Stream codec changed - switching decoder', { from: S.decoderCodec, to: frameCodec, isKey: data.isKey ? 1 : 0 });
        S.currentCodec = frameCodec;
        initDecoder(false, frameCodec).catch(e => log.error('MEDIA', 'Decoder switch failed', { error: e?.message }));
        return false;
    }

    if (!S.ready) {
        if (data.isKey) {
            pendingKeyframe = { ...data, codec: frameCodec };
            return false;
        }
        logVideoDrop('Decoder not ready', { decoderState: S.decoder?.state, needKey: S.needKey });
        return false;
    }

    // Same codec, different encoder instance (resize rebuild): its deltas reference frames the
    // decoder never saw. Only a keyframe from the new instance may start it.
    if (data.generation != null && S.decoderGeneration != null && data.generation !== S.decoderGeneration && !data.isKey) {
        if (!S.needKey) log.info('MEDIA', 'Encoder generation changed - waiting for its keyframe', { from: S.decoderGeneration, to: data.generation });
        S.needKey = 1;
    }

    if (!data.isKey && S.needKey) {
        const now = performance.now();
        if (now - lastWaitingKeyLogAt >= 1000) {
            logVideoDrop('Waiting for keyframe', {
                capTs: data.capTs, timeSinceLastLog: (now - lastWaitingKeyLogAt).toFixed(0)
            });
            lastWaitingKeyLogAt = now;
        }
        startKeyframeRetry();
        return false;
    }

    if (!S.decoder || S.decoder.state !== 'configured') {
        logVideoDrop('Decoder not configured', {
            hasDecoder: !!S.decoder, state: S.decoder?.state,
            needKey: S.needKey, ready: S.ready
        });
        tryReinitDecoder();
        return false;
    }

    // Drop a delta rather than let decode fall further behind. A deep submission queue is pure
    // latency: every queued frame must decode before the newest one reaches the screen. A capable
    // client sits at 0-1 so this never fires; a client that can't keep up sheds load (and triggers
    // a recovery keyframe via notifyDecodeQueuePressure) sooner instead of accumulating ~100 ms of
    // backlog. Keyframes are never dropped here.
    const queueSize = S.decoder.decodeQueueSize || 0;
    if (queueSize > DECODE_QUEUE_DROP_THRESHOLD && !data.isKey) {
        logVideoDrop('Decode queue full', {
            queueSize, isKey: data.isKey ? 1 : 0, capTs: data.capTs
        });
        notifyDecodeQueuePressure(queueSize);
        return false;
    }

    if (!Number.isFinite(data.capTs) || data.capTs < 0) {
        logVideoDrop('Invalid timestamp', {
            capTs: data.capTs, isKey: data.isKey ? 1 : 0,
            bufSize: data.buf?.byteLength
        });
        return false;
    }
    S.frameMeta.set(data.capTs, {
        capTs: data.capTs,
        sourceTs: data.sourceTs || data.capTs,
        encodeEndTs: data.encodeEndTs || 0,
        enqueueTs: data.enqueueTs || 0,
        firstPacketMs: data.firstPacketMs,
        lastPacketMs: data.lastPacketMs,
        reassemblyCompleteMs: data.reassemblyCompleteMs,
        frameKey: data.capTs,
        decodeStartMs: performance.now(),
        arrivalMs: data.arrivalMs
    });
    if (S.frameMeta.size > 30) {
        while (S.frameMeta.size > 20) {
            const oldest = S.frameMeta.keys().next().value;
            if (oldest === undefined) break;
            S.frameMeta.delete(oldest);
        }
    }
    const duration = (lastCaptureTs > 0 && data.capTs > lastCaptureTs)
        ? data.capTs - lastCaptureTs
        : 16667;
    lastCaptureTs = data.capTs;

    try {
        const chunk = new EncodedVideoChunk({
            type: data.isKey ? 'key' : 'delta',
            timestamp: data.capTs,
            duration,
            data: data.buf
        });
        S.decoder.decode(chunk);

        if (data.generation != null && (data.isKey || S.decoderGeneration === null)) S.decoderGeneration = data.generation;
        // Set here, not in processFrame, so a keyframe decoded from the init stash counts too.
        if (Number.isFinite(data.frameId) && data.frameId > S.lastDecodedFrameId) S.lastDecodedFrameId = data.frameId;
        if (data.isKey) {
            S.needKey = 0;
            lastWaitingKeyLogAt = 0;
            stopKeyframeRetry();
            log.info('MEDIA', 'Keyframe decoded', {
                capTs: data.capTs, size: data.buf?.byteLength,
                queueSize: S.decoder?.decodeQueueSize || 0
            });
        } else {
            log.debug('MEDIA', 'Delta frame submitted', {
                capTs: data.capTs, size: data.buf?.byteLength,
                duration, queueSize: S.decoder?.decodeQueueSize || 0
            });
        }
        return true;
    } catch (e) {
        logVideoDrop('Decode exception', {
            error: e.message, isKey: data.isKey ? 1 : 0,
            capTs: data.capTs, size: data.buf?.byteLength,
            decoderState: S.decoder?.state,
            queueSize: S.decoder?.decodeQueueSize || 0
        }, { countDropped: false });
        S.stats.decodeErrors++;
        S.needKey = 1;
        S.ready = 1;
        requestKeyframe('decode-exception');
        startKeyframeRetry();
        return false;
    }
};
// Packet ids become decoder timestamps (10 ms apart), so output gaps show exactly which
// packets never arrived and the worklet can conceal them in order.
const AUDIO_PACKET_US = 10000;
let lastAudioOutputTs = null;
let audioDecoderRecreateTimer = null;

const createAudioDecoder = () => {
    const decoder = S.audioDecoder = new AudioDecoder({
        output: audioData => {
            if (S.audioEnabled && workletReady) {
                recordAudioDecoded();
                const ts = audioData.timestamp;
                if (lastAudioOutputTs !== null && ts > lastAudioOutputTs) {
                    const missing = Math.round((ts - lastAudioOutputTs) / AUDIO_PACKET_US) - 1;
                    if (missing > 0 && missing <= 8) {
                        workletNode.port.postMessage({ type: 'gap', frames: missing * (C.AUDIO_RATE / 100) });
                    }
                }
                lastAudioOutputTs = ts;
                sendToWorklet(audioData);
            } else {
                logAudioDrop('Discarded (disabled)');
                audioData.close();
            }
        },
        error: e => {
            log.error('AUDIO', 'AudioDecoder error', {
                error: e.message,
                decoderState: decoder.state,
                queueSize: decoder.decodeQueueSize || 0
            });
            logAudioDrop('Decoder error', { error: e.message });
            scheduleAudioDecoderRecreate();
        }
    });

    decoder.configure({
        codec: 'opus',
        sampleRate: C.AUDIO_RATE,
        numberOfChannels: C.AUDIO_CH
    });
    lastAudioOutputTs = null;
    log.debug('AUDIO', 'Decoder configured');
    return decoder;
};

// A decoder error closes the AudioDecoder for good; without this, audio stays dead until reconnect.
const scheduleAudioDecoderRecreate = () => {
    if (audioDecoderRecreateTimer || !window.AudioDecoder) return;
    audioDecoderRecreateTimer = setTimeout(() => {
        audioDecoderRecreateTimer = null;
        if (S.audioDecoder && S.audioDecoder.state !== 'closed') safe(() => S.audioDecoder.close(), undefined, 'AUDIO');
        if (safe(createAudioDecoder, null, 'AUDIO')) log.info('AUDIO', 'AudioDecoder recreated');
        resetAudioState();
    }, 100);
};

const resetAudioState = () => {
    lastAudioOutputTs = null;
    if (workletNode) {
        safe(() => workletNode.port.postMessage({ type: 'clear' }), undefined, 'AUDIO');
        log.debug('AUDIO', 'Worklet state cleared');
    }
};
const AUDIO_WORKLET_URL = new URL('./audio-worklet.js', import.meta.url).href;

export const initAudio = async () => {
    if (S.audioCtx && workletNode) {
        if (S.audioCtx.state === 'suspended') {
            await safeAsync(() => S.audioCtx.resume(), undefined, 'AUDIO');
        }
        log.debug('AUDIO', 'Resumed existing context');
        return true;
    }

    try {
        log.info('AUDIO', 'Initializing audio system');
        const ctx = S.audioCtx = new AudioContext({
            sampleRate: C.AUDIO_RATE,
            latencyHint: 'interactive'
        });
        const gain = S.audioGain = ctx.createGain();
        gain.gain.value = 1;
        gain.connect(ctx.destination);
        if (ctx.state === 'suspended') {
            await ctx.resume();
            log.debug('AUDIO', 'Context resumed');
        }
        await ctx.audioWorklet.addModule(AUDIO_WORKLET_URL);
        log.debug('AUDIO', 'Worklet module loaded');
        workletNode = new AudioWorkletNode(ctx, 'stream-audio-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });

        workletNode.port.onmessage = e => {
            const { type } = e.data;
            if (type === 'ready') {
                workletReady = true;
                log.info('AUDIO', 'Worklet ready');
            } else if (type === 'stats') {
                recordAudioBufferHealth(e.data.bufferMs);
                if (e.data.underruns > 0) {
                    log.warn('AUDIO', 'Worklet underruns', {
                        count: e.data.underruns, bufferMs: e.data.bufferMs
                    });
                }
                for (let i = 0; i < e.data.underruns; i++) recordAudioUnderrun();
                if ((e.data.overflows || 0) > 0) {
                    log.warn('AUDIO', 'Worklet overflows', {
                        count: e.data.overflows, bufferMs: e.data.bufferMs
                    });
                }
                for (let i = 0; i < (e.data.overflows || 0); i++) recordAudioOverflow();
                if ((e.data.concealed || 0) > 0) {
                    log.debug('AUDIO', 'Concealed lost packets', { count: e.data.concealed, targetMs: e.data.targetMs });
                }
            } else if (type === 'drop') {
                logAudioDrop(e.data.reason);
            } else {
                log.warn('AUDIO', 'Unknown worklet message type', { type });
            }
        };

        workletNode.connect(gain);
        ctx.onstatechange = () => {
            log.info('AUDIO', 'AudioContext state', { state: ctx.state });
            // Interrupted (device change, OS audio session loss, backgrounded tab): resume and
            // drop whatever piled up so playback restarts at the live edge.
            if (ctx.state === 'interrupted' || ctx.state === 'suspended') {
                if (S.audioEnabled) safeAsync(() => ctx.resume(), undefined, 'AUDIO');
            } else if (ctx.state === 'running') {
                resetAudioState();
            }
        };
        if (window.AudioDecoder) {
            createAudioDecoder();
        } else {
            log.warn('AUDIO', 'AudioDecoder API not available');
        }

        log.info('AUDIO', 'Audio system initialized');
        return true;

    } catch (e) {
        logAudioDrop('Init failed', { error: e.message });
        return false;
    }
};
const extractChannel = (audioData, frames, numChannels, planar, isS16, channel) => {
    const size = audioData.allocationSize({ planeIndex: planar ? channel : 0 });
    const output = new Float32Array(frames);

    if (planar) {
        if (isS16) {
            const temp = new Int16Array(size / 2);
            audioData.copyTo(temp, { planeIndex: channel });
            for (let i = 0; i < frames; i++) output[i] = temp[i] / 32768;
        } else {
            audioData.copyTo(output, { planeIndex: channel });
        }
    } else {
        if (isS16) {
            const temp = new Int16Array(size / 2);
            audioData.copyTo(temp, { planeIndex: 0 });
            for (let i = 0; i < frames; i++) output[i] = temp[i * numChannels + channel] / 32768;
        } else {
            const temp = new Float32Array(size / 4);
            audioData.copyTo(temp, { planeIndex: 0 });
            for (let i = 0; i < frames; i++) output[i] = temp[i * numChannels + channel];
        }
    }

    return output;
};
const sendToWorklet = audioData => {
    if (!workletNode || !workletReady) {
        logAudioDrop('Worklet not ready');
        audioData.close();
        return;
    }

    try {
        const { numberOfFrames: frames, numberOfChannels: numCh, format } = audioData;
        const planar = format.includes('planar');
        const isS16 = format.includes('s16');

        const channels = [];
        for (let c = 0; c < numCh && c < 2; c++) {
            channels.push(extractChannel(audioData, frames, numCh, planar, isS16, c));
        }

        workletNode.port.postMessage(
            { type: 'audio', data: { channels, frames } },
            channels.map(c => c.buffer)
        );
    } catch (e) {
        logAudioDrop('Send to worklet failed', { error: e.message });
    } finally {
        audioData.close();
    }
};

export const handleAudioPacket = data => {
    if (!S.audioEnabled || !S.audioCtx) {
        log.debug('AUDIO', 'Packet dropped - audio not enabled', {
            enabled: S.audioEnabled, hasCtx: !!S.audioCtx
        });
        return;
    }

    recordAudioPacket();

    const view = new DataView(data);
    const packetId = view.getUint32(12, true);
    const samples = view.getUint16(16, true);
    const payloadLength = view.getUint16(18, true);

    if (payloadLength > data.byteLength - C.AUDIO_HEADER) {
        logAudioDrop('Invalid packet length', { length: payloadLength, total: data.byteLength });
        return;
    }

    if (S.audioDecoder?.state !== 'configured') {
        logAudioDrop('Decoder not configured', {
            state: S.audioDecoder?.state,
            hasDecoder: !!S.audioDecoder
        });
        if (S.audioDecoder?.state === 'closed') scheduleAudioDecoderRecreate();
        return;
    }

    const timestamp = packetId * AUDIO_PACKET_US;
    const result = safe(() => {
        const chunk = new EncodedAudioChunk({
            type: 'key',
            timestamp,
            duration: Math.round((samples / C.AUDIO_RATE) * 1e6),
            data: new Uint8Array(data, C.AUDIO_HEADER, payloadLength)
        });
        S.audioDecoder.decode(chunk);
        return true;
    }, false, 'AUDIO');

    if (!result) {
        logAudioDrop('Decode failed', {
            timestamp, samples, length: payloadLength,
            decoderState: S.audioDecoder?.state,
            queueSize: S.audioDecoder?.decodeQueueSize || 0
        });
    }
};

export const toggleAudio = async () => {
    const btn = $('aBtn');
    const txt = $('aTxt');

    if (!S.audioEnabled) {
        try {
            const ok = await initAudio();
            if (ok) {
                S.audioEnabled = 1;
                btn.classList.add('on');
                txt.textContent = 'Mute';
                sendAudioEnable(1, { suppressIfClosed: true });
                log.info('MEDIA', 'Audio enabled');
            } else {
                log.error('MEDIA', 'Failed to enable audio');
            }
        } catch (e) {
            log.error('MEDIA', 'Failed to enable audio', { error: e?.message });
        }
    } else {
        S.audioEnabled = 0;
        btn.classList.remove('on');
        txt.textContent = 'Enable';
        resetAudioState();
        sendAudioEnable(0, { suppressIfClosed: true });
        log.info('MEDIA', 'Audio disabled');
    }
};

export const closeAudio = () => {
    if (workletNode) {
        safe(() => workletNode.disconnect(), undefined, 'AUDIO');
        workletNode = null;
        workletReady = false;
    }

    safe(() => S.audioCtx?.close(), undefined, 'AUDIO');

    resetAudioState();
    log.info('MEDIA', 'Audio closed');
};

export { stopKeyframeRetry };
