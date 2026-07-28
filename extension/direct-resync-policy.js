export class DirectResyncPolicy {
  constructor() {
    this.reset();
  }

  reset() {
    this.keyFrameRequired = true;
    this.awaitingKeyFrameDelivery = true;
    this.resyncEvents = 0;
  }

  shouldEncodeKeyFrame(periodicKeyFrameRequired = false) {
    return this.keyFrameRequired || periodicKeyFrameRequired;
  }

  onKeyFrameSubmitted() {
    this.keyFrameRequired = false;
  }

  canDeliverEncodedChunk(type) {
    return !this.awaitingKeyFrameDelivery || type === "key";
  }

  onEncodedChunkDelivered(type) {
    if (type === "key") {
      this.awaitingKeyFrameDelivery = false;
    }
  }

  requireKeyFrame() {
    const startedResync = !this.awaitingKeyFrameDelivery;
    this.keyFrameRequired = true;
    this.awaitingKeyFrameDelivery = true;
    if (startedResync) {
      this.resyncEvents += 1;
    }
    return startedResync;
  }
}
