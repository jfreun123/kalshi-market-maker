#pragma once

// Command-line argument parsing for kalshi_mm: maps argv into a CliArgs struct
// selecting run mode (paper/scan/reconcile/flatten/capture), verbosity, and the
// config path. Extracted from main so the parser can be unit tested.

#include <filesystem>
#include <span>

namespace kalshi {

struct CliArgs {
  bool paper_mode{false};
  bool scan_mode{false};
  bool reconcile_mode{false};
  bool capture_mode{false};
  bool flatten_mode{false};
  bool fv_replay_mode{false};
  bool verbose{false};
  std::filesystem::path capture_dir{"capture"};
  std::filesystem::path replay_path{"capture/session.jsonl"};
  std::filesystem::path config_path{"config.json"};
};

[[nodiscard]] CliArgs parse_args(std::span<char *> args);

} // namespace kalshi
