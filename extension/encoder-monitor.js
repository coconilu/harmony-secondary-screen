export class EncoderMonitor {
  constructor({ now = () => performance.now(), sampleWindowMs = 1000 } = {}) {
    this.now = now;
    this.sampleWindowMs = sampleWindowMs;
    this.reset();
  }

  reset(startedAt = this.now()) {
    this.startedAt = startedAt;
    this.submittedFrames = 0;
    this.encodedFrames = 0;
    this.encodedBytes = 0;
    this.keyFrames = 0;
    this.droppedFrames = 0;
    this.encodeErrors = 0;
    this.lastEncodedAt = null;
    this.chunkSamples = [];
  }

  onSubmitted() {
    this.submittedFrames += 1;
  }

  onChunk({ byteLength, type }, encodedAt = this.now()) {
    const safeByteLength = Math.max(0, Number(byteLength) || 0);
    this.encodedFrames += 1;
    this.encodedBytes += safeByteLength;
    this.lastEncodedAt = encodedAt;
    if (type === "key") {
      this.keyFrames += 1;
    }
    this.chunkSamples.push({
      at: encodedAt,
      byteLength: safeByteLength
    });
    this.prune(encodedAt);
  }

  onDropped() {
    this.droppedFrames += 1;
  }

  onError() {
    this.encodeErrors += 1;
  }

  sample(encodeQueueSize = 0, sampledAt = this.now()) {
    this.prune(sampledAt);
    const windowBytes = this.chunkSamples.reduce(
      (total, sample) => total + sample.byteLength,
      0
    );
    const elapsedMs = Math.max(0, sampledAt - this.startedAt);
    const lastEncodedAgeMs =
      this.lastEncodedAt === null ? null : Math.max(0, sampledAt - this.lastEncodedAt);

    return {
      encodingFps: this.calculateFps(),
      submittedFrames: this.submittedFrames,
      encodedFrames: this.encodedFrames,
      encodedBytes: this.encodedBytes,
      keyFrames: this.keyFrames,
      droppedFrames: this.droppedFrames,
      encodeErrors: this.encodeErrors,
      encodeQueueSize: Math.max(0, Number(encodeQueueSize) || 0),
      bitrateKbps: (windowBytes * 8) / this.sampleWindowMs,
      averageBitrateKbps:
        elapsedMs > 0 ? (this.encodedBytes * 8) / elapsedMs : 0,
      lastEncodedAgeMs
    };
  }

  prune(referenceTime) {
    const cutoff = referenceTime - this.sampleWindowMs;
    while (this.chunkSamples.length > 0 && this.chunkSamples[0].at < cutoff) {
      this.chunkSamples.shift();
    }
  }

  calculateFps() {
    if (this.chunkSamples.length < 2) {
      return this.chunkSamples.length;
    }

    const first = this.chunkSamples[0].at;
    const last = this.chunkSamples[this.chunkSamples.length - 1].at;
    const durationMs = last - first;
    if (durationMs <= 0) {
      return this.chunkSamples.length;
    }
    return ((this.chunkSamples.length - 1) * 1000) / durationMs;
  }
}
