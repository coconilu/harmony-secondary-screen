export const KEYFRAME_INTERVAL_US = 2_000_000;

export function shouldRequestPeriodicKeyFrame(
  timestampUs,
  lastKeyFrameTimestampUs,
  intervalUs = KEYFRAME_INTERVAL_US
) {
  if (!Number.isFinite(timestampUs) || timestampUs < 0) {
    return true;
  }
  if (
    !Number.isFinite(lastKeyFrameTimestampUs) ||
    lastKeyFrameTimestampUs < 0 ||
    timestampUs < lastKeyFrameTimestampUs
  ) {
    return true;
  }
  return timestampUs - lastKeyFrameTimestampUs >= intervalUs;
}
