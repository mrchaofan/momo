#include "embedder/common/app_config.h"

#include <cstdlib>

#include <charconv>

#include "base/logging.h"

namespace momo::embedder {

namespace {

StatusOr<double> ParseDimension(std::string_view key, std::string_view value) {
  int parsed = 0;
  const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (ec != std::errc() || end != value.data() + value.size() || parsed <= 0 || parsed > 32768) {
    return InvalidArgumentError("--" + std::string(key) + " expects a positive integer, got '" +
                                std::string(value) + "'");
  }
  return static_cast<double>(parsed);
}

}  // namespace

StatusOr<AppConfig> AppConfig::Parse(std::span<const std::string> args) {
  AppConfig config;
  for (const std::string& raw : args) {
    std::string_view arg(raw);
    if (!arg.starts_with("--")) {
      continue;  // 位置参数与系统参数(如 -psn_0_123、-NSDocumentRevisionsDebugMode)
    }
    arg.remove_prefix(2);
    const size_t eq = arg.find('=');
    const std::string_view key = arg.substr(0, eq);
    const bool has_value = eq != std::string_view::npos;
    const std::string_view value = has_value ? arg.substr(eq + 1) : std::string_view();

    auto require_value = [&]() -> Status {
      if (!has_value || value.empty()) {
        return InvalidArgumentError("--" + std::string(key) + " requires a value (--" +
                                    std::string(key) + "=...)");
      }
      return Status::Ok();
    };

    if (key == "dev") {
      config.dev_mode = true;
    } else if (key == "inspect-brk") {
      config.dev_mode = true;
      config.inspector_wait = true;
    } else if (key == "inspect-port") {
      MOMO_RETURN_IF_ERROR(require_value());
      char* end = nullptr;
      const std::string text(value);
      const long port = std::strtol(text.c_str(), &end, 10);
      if (end == text.c_str() || *end != '\0' || port < -1 || port > 65535) {
        return InvalidArgumentError("--inspect-port must be -1..65535");
      }
      config.inspector_port = static_cast<int>(port);
    } else if (key == "bundle") {
      MOMO_RETURN_IF_ERROR(require_value());
      config.bundle_path = std::filesystem::path(std::u8string(value.begin(), value.end()));
    } else if (key == "assets") {
      MOMO_RETURN_IF_ERROR(require_value());
      config.assets_dir = std::filesystem::path(std::u8string(value.begin(), value.end()));
    } else if (key == "title") {
      MOMO_RETURN_IF_ERROR(require_value());
      config.window.title = std::string(value);
    } else if (key == "width") {
      MOMO_RETURN_IF_ERROR(require_value());
      MOMO_ASSIGN_OR_RETURN(config.window.size.width, ParseDimension(key, value));
    } else if (key == "height") {
      MOMO_RETURN_IF_ERROR(require_value());
      MOMO_ASSIGN_OR_RETURN(config.window.size.height, ParseDimension(key, value));
    } else {
      MOMO_LOG(WARNING) << "Ignoring unknown argument: " << raw;
    }
  }
  return config;
}

}  // namespace momo::embedder
