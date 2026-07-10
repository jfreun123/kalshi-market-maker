#include "cli.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

kalshi::CliArgs parse(std::vector<std::string> tokens) {
  std::vector<char *> argv;
  argv.reserve(tokens.size());
  for (auto &token : tokens) {
    argv.push_back(token.data());
  }
  return kalshi::parse_args(std::span<char *>(argv.data(), argv.size()));
}

} // namespace

TEST(CliArgsTest, DefaultsWhenOnlyProgramName) {
  const auto args = parse({"kalshi_mm"});
  EXPECT_FALSE(args.paper_mode);
  EXPECT_FALSE(args.scan_mode);
  EXPECT_FALSE(args.reconcile_mode);
  EXPECT_FALSE(args.capture_mode);
  EXPECT_FALSE(args.flatten_mode);
  EXPECT_FALSE(args.verbose);
  EXPECT_EQ(args.config_path, std::filesystem::path{"config.json"});
}

TEST(CliArgsTest, VerboseFlagEnablesVerbose) {
  const auto args = parse({"kalshi_mm", "--verbose"});
  EXPECT_TRUE(args.verbose);
}

TEST(CliArgsTest, VerboseFlagDoesNotConsumeConfigPath) {
  const auto args = parse({"kalshi_mm", "--verbose", "demo.json"});
  EXPECT_TRUE(args.verbose);
  EXPECT_EQ(args.config_path, std::filesystem::path{"demo.json"});
}

TEST(CliArgsTest, PaperAndVerboseCombine) {
  const auto args = parse({"kalshi_mm", "--paper", "--verbose", "demo.json"});
  EXPECT_TRUE(args.paper_mode);
  EXPECT_TRUE(args.verbose);
  EXPECT_EQ(args.config_path, std::filesystem::path{"demo.json"});
}

TEST(CliArgsTest, ScanFlagParses) {
  const auto args = parse({"kalshi_mm", "--scan", "demo.json"});
  EXPECT_TRUE(args.scan_mode);
  EXPECT_FALSE(args.verbose);
  EXPECT_EQ(args.config_path, std::filesystem::path{"demo.json"});
}

TEST(CliArgsTest, FlattenFlagParses) {
  const auto args = parse({"kalshi_mm", "--flatten", "demo.json"});
  EXPECT_TRUE(args.flatten_mode);
  EXPECT_EQ(args.config_path, std::filesystem::path{"demo.json"});
}

TEST(CliArgsTest, CaptureFlagTakesOptionalDirectory) {
  const auto args = parse({"kalshi_mm", "--capture", "recordings"});
  EXPECT_TRUE(args.capture_mode);
  EXPECT_EQ(args.capture_dir, std::filesystem::path{"recordings"});
}

TEST(CliArgsTest, CaptureFlagWithoutDirectoryKeepsDefault) {
  const auto args = parse({"kalshi_mm", "--capture"});
  EXPECT_TRUE(args.capture_mode);
  EXPECT_EQ(args.capture_dir, std::filesystem::path{"capture"});
}

TEST(CliArgsTest, FvReplayFlagTakesCapturePath) {
  const auto args = parse({"kalshi_mm", "--fv-replay", "rec/session.jsonl"});
  EXPECT_TRUE(args.fv_replay_mode);
  EXPECT_EQ(args.replay_path, std::filesystem::path{"rec/session.jsonl"});
}

TEST(CliArgsTest, FvReplayWithoutPathKeepsDefault) {
  const auto args = parse({"kalshi_mm", "--fv-replay"});
  EXPECT_TRUE(args.fv_replay_mode);
  EXPECT_EQ(args.replay_path, std::filesystem::path{"capture/session.jsonl"});
}

TEST(CliArgsTest, PositionalArgumentSetsConfigPath) {
  const auto args = parse({"kalshi_mm", "custom-config.json"});
  EXPECT_EQ(args.config_path, std::filesystem::path{"custom-config.json"});
}
