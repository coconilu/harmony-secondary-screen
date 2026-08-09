#pragma once

namespace hss::receiver {

constexpr bool ShouldFitVideoByWidth(
    double mediaWidth, double mediaHeight,
    double containerWidth, double containerHeight) {
  if (mediaWidth <= 0 || mediaHeight <= 0 ||
      containerWidth <= 0 || containerHeight <= 0) {
    return false;
  }
  return mediaWidth * containerHeight >= containerWidth * mediaHeight;
}

}  // namespace hss::receiver
