#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hss::receiver::protocol {

constexpr std::uint32_t kVideoMagic = 0x48574336U;
constexpr std::uint8_t kVersion = 6;
constexpr std::size_t kHeaderSize = 32;
constexpr std::uint32_t kMaxControlPayload = 64U * 1024U;
constexpr std::size_t kMaxFrameBytes = 8U * 1024U * 1024U;

enum VideoFlags : std::uint16_t {
  kKeyframe = 1U << 0U,
};

enum class PairingAuthorizationResult {
  kAccepted,
  kExpired,
  kReplayed,
  kMismatch,
};

enum class SourceContractDecision {
  kAccepted,
  kEpochStale,
  kEpochContractMismatch,
};

struct VideoHeader {
  std::uint32_t sourceEpoch = 0;
  std::uint32_t sequence = 0;
  std::uint16_t flags = 0;
  std::uint32_t payloadLength = 0;
  std::uint64_t timestampUs = 0;
};

struct MediaContract {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t maxFps = 0;

  constexpr bool operator==(const MediaContract&) const = default;
};

std::optional<VideoHeader> DecodeVideoHeader(const std::byte* data, std::size_t size);

constexpr bool IsAcceptedSourceEpoch(std::uint32_t candidate, std::uint32_t latest) {
  return candidate >= latest;
}

constexpr SourceContractDecision EvaluateSourceContract(
    std::uint32_t candidateEpoch, MediaContract candidateContract,
    std::uint32_t latestEpoch, MediaContract activeContract) {
  if (candidateEpoch < latestEpoch) {
    return SourceContractDecision::kEpochStale;
  }
  if (candidateEpoch == latestEpoch && candidateContract != activeContract) {
    return SourceContractDecision::kEpochContractMismatch;
  }
  return SourceContractDecision::kAccepted;
}

std::string PairingShortCode(std::string_view token);
std::string PairProofMessage(std::string_view proofMode,
                             std::string_view sessionId,
                             std::string_view senderId,
                             std::string_view nonce,
                             std::string_view deviceId);
std::string AuthProofMessage(std::string_view senderId,
                              std::string_view deviceId,
                              std::uint32_t sourceEpoch,
                              std::string_view nonce,
                              MediaContract contract);
std::string ComputePairProof(std::string_view secret,
                             std::string_view proofMode,
                             std::string_view sessionId,
                             std::string_view senderId,
                             std::string_view nonce,
                             std::string_view deviceId);
std::string ComputeAuthProof(std::string_view credential,
                              std::string_view senderId,
                              std::string_view deviceId,
                              std::uint32_t sourceEpoch,
                              std::string_view nonce,
                              MediaContract contract);
bool IsValidMediaContract(MediaContract contract);
bool ConstantTimeEqual(std::string_view first, std::string_view second);
PairingAuthorizationResult EvaluatePairingAuthorization(
    std::string_view sessionId, std::string_view token,
    std::string_view pendingSessionId, std::string_view pendingToken,
    std::string_view pendingShortCode, std::string_view consumedSessionId,
    std::int64_t expiresAtMs, std::int64_t nowMs);

std::optional<std::string> JsonString(std::string_view json, std::string_view key);
std::optional<std::int64_t> JsonInteger(std::string_view json, std::string_view key);
std::string EscapeJson(std::string_view value);

}  // namespace hss::receiver::protocol
