function nearestEven(value) {
    const lower = Math.floor(value);
    const fractional = value - lower;
    return fractional > 0.5 || (fractional === 0.5 && lower % 2 !== 0) ?
        lower + 1 : lower;
}

export function pcm16FromAudioBuffer(buffer, sampleCount) {
    if (!Number.isSafeInteger(sampleCount) || sampleCount < 0 ||
        sampleCount > buffer.length || ![1, 2].includes(buffer.numberOfChannels))
        throw new Error('Audio Vorbis no compatible con la conversión PCM16.');
    const first = buffer.getChannelData(0);
    const second = buffer.numberOfChannels === 2 ? buffer.getChannelData(1) : null;
    const output = new Int16Array(sampleCount);
    for (let i = 0; i < sampleCount; i++) {
        const mixed = second ? Math.fround((first[i] + second[i]) / 2) : first[i];
        output[i] = Math.max(-32768, Math.min(32767, nearestEven(mixed * 32768)));
    }
    return output;
}
