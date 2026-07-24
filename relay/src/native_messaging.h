#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace hwc::relay {

constexpr std::size_t kMaxNativeMessageBytes = 1024U * 1024U;

void ConfigureNativeMessagingStdio();
[[nodiscard]] std::optional<std::string> ReadNativeMessage();
void WriteNativeMessage(std::string_view message);

}  // namespace hwc::relay
