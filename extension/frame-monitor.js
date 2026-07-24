export class FrameMonitor {
  constructor({ now = () => performance.now(), stallThresholdMs = 2000 } = {}) {
    this.now = now;
    this.stallThresholdMs = stallThresholdMs;
    this.reset();
  }

  reset(startedAt = this.now()) {
    this.startedAt = startedAt;
    this.totalFrames = 0;
    this.lastFrameAt = null;
    this.frameTimes = [];
    this.stallEvents = 0;
    this.stalled = false;
    this.pendingTransition = null;
  }

  onFrame(frameAt = this.now()) {
    this.totalFrames += 1;
    this.lastFrameAt = frameAt;
    this.frameTimes.push(frameAt);
    this.prune(frameAt);

    if (this.stalled) {
      this.stalled = false;
      this.pendingTransition = "recovered";
    }
  }

  sample(sampledAt = this.now()) {
    this.prune(sampledAt);
    const lastFrameAgeMs =
      this.lastFrameAt === null ? null : Math.max(0, sampledAt - this.lastFrameAt);
    const waitingForFirstFrame =
      this.lastFrameAt === null && sampledAt - this.startedAt >= this.stallThresholdMs;
    const frameExpired =
      lastFrameAgeMs !== null && lastFrameAgeMs >= this.stallThresholdMs;

    if (!this.stalled && (waitingForFirstFrame || frameExpired)) {
      this.stalled = true;
      this.stallEvents += 1;
      this.pendingTransition = "stalled";
    }

    const transition = this.pendingTransition;
    this.pendingTransition = null;

    return {
      fps: this.calculateFps(),
      totalFrames: this.totalFrames,
      lastFrameAgeMs,
      stallEvents: this.stallEvents,
      stalled: this.stalled,
      transition
    };
  }

  prune(referenceTime) {
    const cutoff = referenceTime - 1000;
    while (this.frameTimes.length > 0 && this.frameTimes[0] < cutoff) {
      this.frameTimes.shift();
    }
  }

  calculateFps() {
    if (this.frameTimes.length < 2) {
      return this.frameTimes.length;
    }

    const first = this.frameTimes[0];
    const last = this.frameTimes[this.frameTimes.length - 1];
    const durationMs = last - first;
    if (durationMs <= 0) {
      return this.frameTimes.length;
    }

    return ((this.frameTimes.length - 1) * 1000) / durationMs;
  }
}
