#pragma once
// Application-level operations extracted from main.cpp (refactor R2): the
// CLI mode runners (scan / reconcile / flatten / capture), the exchange
// reconciliation + flatten helpers they share, and in-session market
// rotation. main.cpp keeps only wiring and the trading loop.

#include "app/config.hpp"
#include "engine/trading_session.hpp"
#include "exchange/rest_client.hpp"
#include "exchange/ticker_scanner.hpp"
#include "net/auth.hpp"
#include "net/paper_transport.hpp"
#include "net/websocket_client.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace kalshi {

int run_scan_mode(RestClient &rest, const ScannerConfig &scanner_config,
                  std::shared_ptr<spdlog::logger> &log);

// Fetches fills from the exchange since min_ts_seconds and replays the ones
// for tracked markets through the session's fill path (idempotent — the WS
// fill channel drops fills during disconnects and, per run 22, sometimes
// while connected). Returns the number of fills newly recorded, or -1 when
// the fetch failed. advance (optional) is pushed forward to the latest
// tracked fill timestamp seen. Caller must hold the engine lock.
int backfill_fills(RestClient &rest, TradingSession &session,
                   long long min_ts_seconds,
                   std::chrono::system_clock::time_point *advance,
                   std::shared_ptr<spdlog::logger> &log);

// On drift, when a backfill hook is provided, it runs once (returning newly
// recorded fills, or -1 on fetch failure) and positions are re-fetched and
// re-compared before halting — missed WS fills then heal in place instead of
// halting the session for good (item 73).
bool reconcile_against_exchange(
    RestClient &rest, const IOrderManager &order_mgr,
    const std::vector<std::string> &tickers, RiskManager *risk_mgr,
    std::shared_ptr<spdlog::logger> &log,
    const std::vector<MarketPosition> &baseline = {},
    const std::function<int()> &backfill = {});

int run_reconcile_mode(RestClient &rest,
                       const std::vector<std::string> &tickers,
                       std::shared_ptr<spdlog::logger> &log);

int flatten_all_positions(
    RestClient &rest, std::shared_ptr<spdlog::logger> &log,
    TradingSession *session,
    const std::vector<std::string> *only_tickers = nullptr);

int run_flatten_mode(RestClient &rest, std::shared_ptr<spdlog::logger> &log);

std::vector<std::string> scan_top_tickers(TickerScanner &scanner,
                                          const AppConfig &app_config,
                                          std::shared_ptr<spdlog::logger> &log);

int run_capture_mode(const std::atomic<bool> &shutdown_requested,
                     const Auth &auth, const AppConfig &app_config,
                     const std::filesystem::path &capture_dir,
                     std::shared_ptr<spdlog::logger> &log);

int simulate_maker_fills(PaperTransport &paper, TradingSession *session,
                         const PublicTrade &print);

int run_backtest_mode(const std::filesystem::path &capture_path,
                      const AppConfig &app_config, std::ostream &out,
                      std::shared_ptr<spdlog::logger> &log);

int run_fv_replay_mode(const std::filesystem::path &capture_path,
                       std::ostream &out, std::shared_ptr<spdlog::logger> &log);

void rotate_markets(TickerScanner &scanner, RestClient &rest,
                    TradingSession &session, WebSocketClient &ws_client,
                    const AppConfig &app_config, std::mutex &engine_mtx,
                    std::shared_ptr<spdlog::logger> &log);

} // namespace kalshi
