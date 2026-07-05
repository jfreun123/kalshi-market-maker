#include "scan_output.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

const auto kScannedAt =
    std::chrono::system_clock::from_time_t(1782000000); // 2026-06-21

constexpr int kFirstMidCents = 48;
constexpr int kFirstSpreadCents = 4;
constexpr double kFirstVolume24h = 123456.0;
constexpr double kFirstDaysToClose = 12.5;
constexpr double kFirstScore = 0.873;
constexpr double kSecondVolume24h = 1000.0;
constexpr double kSecondScore = 0.5;

std::vector<kalshi::MarketScore> sample_results() {
  kalshi::MarketScore first;
  first.ticker = "KXFED-26SEP-T3.00";
  first.title = "Fed rate?";
  first.category = "Financials";
  first.mid_price_cents = kFirstMidCents;
  first.spread_cents = kFirstSpreadCents;
  first.volume_24h = kFirstVolume24h;
  first.days_to_close = kFirstDaysToClose;
  first.score = kFirstScore;

  kalshi::MarketScore second;
  second.ticker = "KXCPI-26SEP-T0.3";
  second.volume_24h = kSecondVolume24h;
  second.score = kSecondScore;

  return {first, second};
}

} // namespace

TEST(ScanOutputTest, SerializesMetadataAndCount) {
  const auto json = nlohmann::json::parse(
      kalshi::scan_results_to_json(sample_results(), kScannedAt));

  EXPECT_EQ(json.at("count").get<int>(), 2);
  EXPECT_TRUE(json.contains("scanned_at"));
  EXPECT_EQ(json.at("markets").size(), 2U);
}

TEST(ScanOutputTest, TopLevelTickersArrayPreservesRankOrder) {
  const auto json = nlohmann::json::parse(
      kalshi::scan_results_to_json(sample_results(), kScannedAt));

  const auto &tickers = json.at("tickers");
  ASSERT_EQ(tickers.size(), 2U);
  EXPECT_EQ(tickers.at(0).get<std::string>(), "KXFED-26SEP-T3.00");
  EXPECT_EQ(tickers.at(1).get<std::string>(), "KXCPI-26SEP-T0.3");
}

TEST(ScanOutputTest, MarketEntryCarriesAllScoreFields) {
  const auto json = nlohmann::json::parse(
      kalshi::scan_results_to_json(sample_results(), kScannedAt));

  const auto &first = json.at("markets").at(0);
  EXPECT_EQ(first.at("ticker").get<std::string>(), "KXFED-26SEP-T3.00");
  EXPECT_EQ(first.at("title").get<std::string>(), "Fed rate?");
  EXPECT_EQ(first.at("mid_price_cents").get<int>(), kFirstMidCents);
  EXPECT_EQ(first.at("spread_cents").get<int>(), kFirstSpreadCents);
  EXPECT_DOUBLE_EQ(first.at("volume_24h").get<double>(), kFirstVolume24h);
  EXPECT_DOUBLE_EQ(first.at("days_to_close").get<double>(), kFirstDaysToClose);
  EXPECT_DOUBLE_EQ(first.at("score").get<double>(), kFirstScore);
}

TEST(ScanOutputTest, WriteScanResultsCreatesReadableFile) {
  const auto path =
      std::filesystem::temp_directory_path() / "kalshi_scan_test.json";
  std::filesystem::remove(path);

  ASSERT_TRUE(kalshi::write_scan_results(path, sample_results(), kScannedAt));
  ASSERT_TRUE(std::filesystem::exists(path));

  std::ifstream file{path};
  const auto json = nlohmann::json::parse(file);
  EXPECT_EQ(json.at("count").get<int>(), 2);
  std::filesystem::remove(path);
}
