#include "cli.hpp"

#include <cstddef>
#include <string_view>

namespace kalshi {

CliArgs parse_args(std::span<char *> args) {
  CliArgs result;
  for (std::size_t idx = 1U; idx < args.size(); ++idx) {
    const std::string_view arg{args[idx]};
    if (arg == "--paper") {
      result.paper_mode = true;
    } else if (arg == "--scan") {
      result.scan_mode = true;
    } else if (arg == "--reconcile") {
      result.reconcile_mode = true;
    } else if (arg == "--flatten") {
      result.flatten_mode = true;
    } else if (arg == "--verbose") {
      result.verbose = true;
    } else if (arg == "--capture") {
      result.capture_mode = true;
      if (idx + 1U < args.size() && args[idx + 1U][0] != '-') {
        result.capture_dir = std::filesystem::path{args[++idx]};
      }
    } else if (arg == "--fv-replay") {
      result.fv_replay_mode = true;
      if (idx + 1U < args.size() && args[idx + 1U][0] != '-') {
        result.replay_path = std::filesystem::path{args[++idx]};
      }
    } else {
      result.config_path = std::filesystem::path{args[idx]};
    }
  }
  return result;
}

} // namespace kalshi
