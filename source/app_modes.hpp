#pragma once
// Application-level operations extracted from main.cpp (refactor R2): the
// CLI mode runners (scan / reconcile / flatten / capture), the exchange
// reconciliation + flatten helpers they share, and in-session market
// rotation. main.cpp keeps only wiring and the trading loop.

#include "auth.hpp"
#include "config.hpp"
#include "rest_client.hpp"
#include "trading_session.hpp"
#include "websocket_client.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kalshi {

std::filesystem::path
trade_config_path_for(const std::filesystem::path &base_config_path);

int run_scan_mode(RestClient &rest, const ScannerConfig &scanner_config,
                  const std::filesystem::path &config_path,
                  std::shared_ptr<spdlog::logger> &log);

bool reconcile_against_exchange(RestClient &rest,
                                const IOrderManager &order_mgr,
                                const std::vector<std::string> &tickers,
                                RiskManager *risk_mgr,
                                std::shared_ptr<spdlog::logger> &log);

int run_reconcile_mode(RestClient &rest,
                       const std::vector<std::string> &tickers,
                       std::shared_ptr<spdlog::logger> &log);

int flatten_all_positions(RestClient &rest,
                          std::shared_ptr<spdlog::logger> &log,
                          TradingSession *session);

int run_flatten_mode(RestClient &rest, std::shared_ptr<spdlog::logger> &log);

int run_capture_mode(const std::atomic<bool> &shutdown_requested,
                     const Auth &auth, const AppConfig &app_config,
                     const std::filesystem::path &capture_dir,
                     std::shared_ptr<spdlog::logger> &log);

void rotate_markets(RestClient &rest, TradingSession &session,
                    WebSocketClient &ws_client, const AppConfig &app_config,
                    std::mutex &engine_mtx,
                    std::shared_ptr<spdlog::logger> &log);

} // namespace kalshi
