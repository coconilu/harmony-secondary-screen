import {
  createDirectVideoMessage,
  DIRECT_VIDEO_MAX_PAYLOAD_BYTES
} from "./direct-protocol.js";
import { DirectResyncPolicy } from "./direct-resync-policy.js";

export class DirectDeliveryOrchestrator {
  constructor() {
    this.policy = new DirectResyncPolicy();
    this.sequence = 0;
  }

  reset() {
    this.sequence = 0;
    this.policy.reset();
  }

  shouldEncodeKeyFrame(periodicKeyFrameRequired = false) {
    return this.policy.shouldEncodeKeyFrame(periodicKeyFrameRequired);
  }

  onKeyFrameSubmitted() {
    this.policy.onKeyFrameSubmitted();
  }

  requireKeyFrame(telemetry) {
    if (this.policy.requireKeyFrame()) {
      telemetry.directResyncEvents += 1;
    }
  }

  deliver(chunk, connection, sourceEpoch, telemetry) {
    if (chunk.byteLength > DIRECT_VIDEO_MAX_PAYLOAD_BYTES) {
      throw new Error("单个 H.264 编码块超过 8 MiB");
    }
    if (
      !connection?.connected ||
      !this.policy.canDeliverEncodedChunk(chunk.type)
    ) {
      this.dropForResync(telemetry);
      return false;
    }

    const message = createDirectVideoMessage(
      chunk,
      sourceEpoch,
      this.sequence
    );
    if (!connection.sendVideo(message)) {
      this.dropForResync(telemetry);
      return false;
    }

    this.sequence = (this.sequence + 1) >>> 0;
    this.policy.onEncodedChunkDelivered(chunk.type);
    telemetry.directSentFrames += 1;
    telemetry.directSentBytes += chunk.byteLength;
    return true;
  }

  dropForResync(telemetry) {
    telemetry.directDroppedFrames += 1;
    this.requireKeyFrame(telemetry);
  }
}
