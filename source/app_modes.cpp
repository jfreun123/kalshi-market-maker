#include "app_modes.hpp"

#include "capture.hpp"
#include "logger.hpp"
#include "scan_output.hpp"
#include "ticker_scanner.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <ranges>
#include <thread>

namespace kalshi {

// ---- Scanner mode ----

constexpr auto kScanResultsPath = "scan_results.json";

// Derives the generated trade-config path from the base config path by
// inserting ".trade" before the extension: config.json -> config.trade.json,
// config.demo.json -> config.demo.trade.json.
int run_scan_mode(kalshi::RestClient &rest,
                  const kalshi::ScannerConfig &scanner_config,
                  std::shared_ptr<spdlog::logger> &log) {
  constexpr int kScanTopN = 20;
  log->info(
      "scanner mode — scanning all markets "
      "(price=[{},{}]c spread=[{},{}]c min_vol_24h={:.0f} max_days={:.0f})",
      scanner_config.min_price_cents, scanner_config.max_price_cents,
      scanner_config.min_spread_cents, scanner_config.max_spread_cents,
      scanner_config.min_volume_24h, scanner_config.max_days_to_close);

  const auto now = std::chrono::system_clock::now();
  kalshi::TickerScanner scanner{rest, scanner_config};
  const auto results = scanner.scan(kScanTopN, now);
  if (results.empty()) {
    log->warn("no markets passed scanner filters");
    return 0;
  }
  log->info("scanner results (top {}):", results.size());
  for (std::size_t rank = 0U; rank < results.size(); ++rank) {
    const auto &market = results[rank];
    log->info(
        "  {:>2}. ticker={} mid={}c spread={}c vol_24h={:.0f} days={:.1f} "
        "reward=${:.0f} score={:.3f} cat={} \"{}\"",
        rank + 1U, market.ticker, market.mid_price_cents, market.spread_cents,
        market.volume_24h, market.days_to_close,
        market.incentive_reward_dollars, market.score, market.category,
        market.title);
  }

  const std::filesystem::path results_path{kScanResultsPath};
  if (kalshi::write_scan_results(results_path, results, now)) {
    log->info("scan results written to {} ({} markets)", results_path.string(),
              results.size());
  } else {
    log->warn("failed to write scan results to {}", results_path.string());
  }

  return 0;
}

// ---- Reconciliation against the exchange ----

// Fetches the exchange's authoritative positions and compares them to local
// accounting. On drift, logs every mismatch; if risk_mgr is non-null (live
// trading) it trips kModelDiverge to halt all quoting. Returns true if in sync.
bool reconcile_against_exchange(kalshi::RestClient &rest,
                                const kalshi::IOrderManager &order_mgr,
                                const std::vector<std::string> &tickers,
                                kalshi::RiskManager *risk_mgr,
                                std::shared_ptr<spdlog::logger> &log) {
  std::vector<kalshi::MarketPosition> exchange;
  try {
    exchange = rest.get_positions();
  } catch (const std::exception &ex) {
    log->error("reconcile: failed to fetch exchange positions: {}", ex.what());
    return false;
  }

  const auto result = kalshi::reconcile(order_mgr, tickers, exchange);
  if (result.in_sync) {
    log->info("reconcile: in sync ({} exchange positions checked)",
              exchange.size());
    if (risk_mgr != nullptr &&
        risk_mgr->is_set(kalshi::Constraint::kModelDiverge)) {
      risk_mgr->clear(kalshi::Constraint::kModelDiverge);
      log->info("reconcile back in sync — drift halt cleared");
    }
    return true;
  }

  for (const auto &diff : result.diffs) {
    log->critical("reconcile DRIFT ticker={} local={} exchange={}", diff.ticker,
                  diff.local_position.to_fp_string(),
                  diff.exchange_position.to_fp_string());
  }
  if (risk_mgr != nullptr) {
    risk_mgr->set(kalshi::Constraint::kModelDiverge);
    log->critical("reconcile: {} position mismatch(es) — quoting halted",
                  result.diffs.size());
  }
  return false;
}

// ---- In-session market rotation (item 52) ----

// Re-scans and swaps dead-idle markets for currently-live ones. The scan and
// snapshot fetches run WITHOUT the engine lock (they are seconds of REST);
// only session mutations take it. Markets holding a position or resting
// orders are never rotated out.
void rotate_markets(kalshi::RestClient &rest, kalshi::TradingSession &session,
                    kalshi::WebSocketClient &ws_client,
                    const kalshi::AppConfig &app_config, std::mutex &engine_mtx,
                    std::shared_ptr<spdlog::logger> &log) {
  try {
    kalshi::TickerScanner scanner{rest, app_config.scanner};
    const auto picks = scanner.scan(app_config.scanner.trade_top_n);
    std::vector<std::string> pick_tickers;
    pick_tickers.reserve(picks.size());
    for (const auto &pick : picks) {
      pick_tickers.push_back(pick.ticker);
    }

    std::vector<std::string> to_add;
    {
      const std::lock_guard<std::mutex> lock{engine_mtx};
      const auto tracked = session.tickers();
      for (const auto &ticker : tracked) {
        if (std::ranges::find(pick_tickers, ticker) == pick_tickers.end()) {
          (void)session.remove_market_if_idle(ticker);
        }
      }
      for (const auto &ticker : pick_tickers) {
        if (!session.is_tracked(ticker)) {
          to_add.push_back(ticker);
        }
      }
    }

    for (const auto &ticker : to_add) {
      try {
        const auto snapshot = rest.get_orderbook(ticker);
        {
          const std::lock_guard<std::mutex> lock{engine_mtx};
          session.add_market(snapshot);
        }
        ws_client.subscribe(ticker);
      } catch (const std::exception &ex) {
        log->error("rotation: adopt failed ticker={}: {}", ticker, ex.what());
      }
    }
  } catch (const std::exception &ex) {
    log->error("rotation scan failed: {}", ex.what());
  }
}

// Standalone --reconcile: compares local (flat at startup) against the exchange
// and exits non-zero on any mismatch. Useful as a pre-trade / CI sanity check.
int run_reconcile_mode(kalshi::RestClient &rest,
                       const std::vector<std::string> &tickers,
                       std::shared_ptr<spdlog::logger> &log) {
  log->info("reconcile mode — comparing local state against exchange");
  kalshi::OrderManager order_mgr{rest};
  const bool in_sync =
      reconcile_against_exchange(rest, order_mgr, tickers, nullptr, log);
  return in_sync ? 0 : 1;
}

// Closes every open position with an aggressive IOC taker order so we end flat.
// Best-effort: logs and continues past a single-ticker failure. Returns the
// number of positions it attempted to close.
int flatten_all_positions(kalshi::RestClient &rest,
                          std::shared_ptr<spdlog::logger> &log,
                          kalshi::TradingSession *session) {
  int closed = 0;
  for (const auto &position : rest.get_positions()) {
    if (position.position.is_zero()) {
      continue;
    }
    try {
      const auto order = rest.flatten(position.ticker, position.position);
      log->info("flatten ticker={} net={} filled={} order_id={}",
                position.ticker, position.position.to_fp_string(),
                order.filled_quantity.to_fp_string(), order.id);
      if (session != nullptr) {
        session->record_flatten(order);
      }
      ++closed;
    } catch (const std::exception &ex) {
      log->error("flatten ticker={} failed: {}", position.ticker, ex.what());
    }
  }
  return closed;
}

// Standalone --flatten: close all open positions and exit. Cleans up inventory
// left by a prior run (cancel-on-exit stops orders, not positions).
int run_flatten_mode(kalshi::RestClient &rest,
                     std::shared_ptr<spdlog::logger> &log) {
  log->info("flatten mode — closing all open positions");
  const int closed = flatten_all_positions(rest, log, nullptr);
  log->info("flatten mode — closed {} position(s)", closed);
  return 0;
}

// ---- Capture mode ----

// Records a live exchange session for later replay: raw inbound WS frames are
// teed to <dir>/session.jsonl (one per line, replay-compatible with the
// integration test) and the seed REST responses (orderbooks, positions) to
// <dir>/rest.jsonl for field-shape inspection. Runs until SIGINT/SIGTERM.
int run_capture_mode(const std::atomic<bool> &shutdown_requested,
                     const kalshi::Auth &auth,
                     const kalshi::AppConfig &app_config,
                     const std::filesystem::path &capture_dir,
                     std::shared_ptr<spdlog::logger> &log) {
  if (app_config.target_tickers.empty()) {
    log->critical("capture mode — target_tickers is empty; nothing to record");
    return 1;
  }

  std::filesystem::create_directories(capture_dir);
  const auto ws_path = capture_dir / "session.jsonl";
  const auto rest_path = capture_dir / "rest.jsonl";
  std::ofstream ws_file{ws_path};
  std::ofstream rest_file{rest_path};
  if (!ws_file || !rest_file) {
    log->critical("capture mode — cannot open output files in {}",
                  capture_dir.string());
    return 1;
  }

  auto rest_transport = std::make_unique<kalshi::CapturingHttpTransport>(
      std::make_unique<kalshi::HttpTransport>(), rest_file);
  auto *rest_capture = rest_transport.get();
  kalshi::RestClient rest{auth, std::move(rest_transport), app_config.base_url};

  auto ws_transport = std::make_unique<kalshi::CapturingWebSocket>(
      std::make_unique<kalshi::IxWebSocket>(), ws_file);
  auto *ws_capture = ws_transport.get();
  kalshi::WebSocketClient ws_client{auth, std::move(ws_transport),
                                    app_config.ws_url};

  ws_client.on_orderbook_snapshot([&log](const kalshi::Orderbook &snap) {
    log->info("capture snapshot ticker={} yes_levels={} no_levels={}",
              snap.ticker, snap.yes.size(), snap.no.size());
  });

  // Seed the REST capture and subscribe each ticker to the live WS stream.
  for (const auto &ticker : app_config.target_tickers) {
    try {
      (void)rest.get_orderbook(ticker);
    } catch (const std::exception &ex) {
      log->warn("capture — get_orderbook ticker={} failed: {}", ticker,
                ex.what());
    }
    ws_client.subscribe(ticker);
  }
  try {
    (void)rest.get_positions();
  } catch (const std::exception &ex) {
    log->warn("capture — get_positions failed: {}", ex.what());
  }

  std::thread ws_thread([&ws_client]() { ws_client.run(); });

  log->info("capturing — ws={} rest={} — Ctrl-C to stop", ws_path.string(),
            rest_path.string());

  constexpr auto kPollInterval = std::chrono::milliseconds{200};
  constexpr int kProgressInterval = 50; // 50 × 200ms = 10s
  int poll_count = 0;
  while (!shutdown_requested.load()) {
    std::this_thread::sleep_for(kPollInterval);
    if (++poll_count % kProgressInterval == 0) {
      log->info("capture progress ws_frames={} rest_calls={}",
                ws_capture->captured_count(), rest_capture->captured_count());
    }
  }

  log->info("capture stopping");
  ws_client.stop();
  ws_thread.join();
  log->info("capture complete ws_frames={} rest_calls={} dir={}",
            ws_capture->captured_count(), rest_capture->captured_count(),
            capture_dir.string());
  return 0;
}

} // namespace kalshi
