#include "app_modes.hpp"

#include "capture.hpp"
#include "fv_backtest.hpp"
#include "logger.hpp"
#include "order_manager.hpp"
#include "scan_output.hpp"
#include "ticker_scanner.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <ranges>
#include <thread>

namespace kalshi {

// ---- Scanner mode ----

constexpr auto kScanResultsPath = "scan_results.json";

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
    const std::filesystem::path empty_path{kScanResultsPath};
    if (kalshi::write_scan_results(empty_path, results, now)) {
      log->info("empty scan results written to {} — consumers must not "
                "reuse a stale file",
                empty_path.string());
    }
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
bool reconcile_against_exchange(
    kalshi::RestClient &rest, const kalshi::IOrderManager &order_mgr,
    const std::vector<std::string> &tickers, kalshi::RiskManager *risk_mgr,
    std::shared_ptr<spdlog::logger> &log,
    const std::vector<kalshi::MarketPosition> &baseline) {
  std::vector<kalshi::MarketPosition> exchange;
  try {
    exchange = rest.get_positions();
  } catch (const std::exception &ex) {
    log->error("reconcile: failed to fetch exchange positions: {}", ex.what());
    return false;
  }

  const auto result = kalshi::reconcile(order_mgr, tickers, exchange, baseline);
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
    log->critical("reconcile DRIFT ticker={} local={} exchange={} baseline={}",
                  diff.ticker, diff.local_position.to_fp_string(),
                  diff.exchange_position.to_fp_string(),
                  diff.baseline_position.to_fp_string());
  }
  if (risk_mgr != nullptr) {
    risk_mgr->set(kalshi::Constraint::kModelDiverge);
    log->critical("reconcile: {} position mismatch(es) — quoting halted",
                  result.diffs.size());
  }
  return false;
}

// ---- Market self-selection ----

// Scans for the top live markets exactly as the trading session does at
// startup — one shared selection path for run, capture, and rotation modes.
std::vector<std::string>
scan_top_tickers(kalshi::RestClient &rest, const kalshi::AppConfig &app_config,
                 std::shared_ptr<spdlog::logger> &log) {
  kalshi::TickerScanner scanner{rest, app_config.scanner};
  std::vector<std::string> tickers;
  for (const auto &pick : scanner.scan(app_config.scanner.trade_top_n)) {
    tickers.push_back(pick.ticker);
    log->info("selected ticker={} score={:.3f}", pick.ticker, pick.score);
  }
  return tickers;
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
                          kalshi::TradingSession *session,
                          const std::vector<std::string> *only_tickers) {
  int closed = 0;
  for (const auto &position : rest.get_positions()) {
    if (position.position.is_zero()) {
      continue;
    }
    if (only_tickers != nullptr &&
        std::ranges::find(*only_tickers, position.ticker) ==
            only_tickers->end()) {
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

// ---- Backtest mode ----

namespace {

// Ephemeral RSA key so RestClient's request signing works against the paper
// exchange — backtests must never need real credentials.
std::string ephemeral_rsa_pem() {
  constexpr unsigned int kRsaBits = 2048U;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  EVP_PKEY *pkey = EVP_RSA_gen(kRsaBits);
  if (pkey == nullptr) {
    return "";
  }
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  char *pem_data = nullptr;
  const long pem_len = BIO_get_mem_data(bio, &pem_data); // NOLINT(runtime/int)
  std::string pem(pem_data, static_cast<std::size_t>(pem_len));
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return pem;
}

} // namespace

// Conservative print-through fill model: a recorded public print fills our
// resting paper order only when the taker traded STRICTLY through our level
// (at-level prints are queue-position-dependent and never simulated). The
// print's size is consumed across crossable orders. Fills are applied to the
// paper exchange and, when a session is given, forwarded through the same
// on_fill path live fills take. Returns the number of orders filled.
int simulate_maker_fills(kalshi::PaperTransport &paper,
                         kalshi::TradingSession *session,
                         const kalshi::PublicTrade &print) {
  struct Crossable {
    std::string order_id;
    kalshi::Side side;
    int price_cents;
    int remaining;
  };
  std::vector<Crossable> crossable;
  for (const auto &order : paper.open_orders()) {
    if (order.market_ticker != print.market_ticker || !order.is_active()) {
      continue;
    }
    bool crossed = false;
    if (print.taker_side == kalshi::Side::Yes &&
        order.side == kalshi::Side::No) {
      crossed =
          print.yes_price_cents > kalshi::complement_price(order.price_cents);
    } else if (print.taker_side == kalshi::Side::No &&
               order.side == kalshi::Side::Yes) {
      crossed = print.yes_price_cents < order.price_cents;
    }
    if (!crossed) {
      continue;
    }
    const int remaining =
        static_cast<int>((order.quantity - order.filled_quantity).contracts());
    if (remaining > 0) {
      crossable.push_back({order.id, order.side, order.price_cents, remaining});
    }
  }

  int print_remaining = static_cast<int>(print.quantity.contracts());
  int orders_filled = 0;
  for (const auto &target : crossable) {
    if (print_remaining <= 0) {
      break;
    }
    const int fill_qty = std::min(print_remaining, target.remaining);
    if (fill_qty <= 0 || !paper.simulate_fill(target.order_id, fill_qty)) {
      continue;
    }
    print_remaining -= fill_qty;
    ++orders_filled;
    if (session != nullptr) {
      kalshi::Fill fill;
      fill.trade_id = "sim-" + std::to_string(paper.fills().size());
      fill.order_id = target.order_id;
      fill.market_ticker = print.market_ticker;
      fill.side = target.side;
      fill.price_cents = target.price_cents;
      fill.quantity = kalshi::Quantity::from_contracts(fill_qty);
      fill.is_taker = false;
      fill.timestamp = print.timestamp;
      session->on_fill(fill);
    }
  }
  return orders_filled;
}

// Replays a capture through the full production stack (parser -> session ->
// quoter -> risk) against the paper exchange, crossing recorded prints with
// our simulated quotes. The strategy dev loop in seconds: change config,
// re-run, read the per-market PnL — no live slate required.
int run_backtest_mode(const std::filesystem::path &capture_path,
                      const kalshi::AppConfig &app_config, std::ostream &out,
                      std::shared_ptr<spdlog::logger> &log) {
  std::ifstream capture_file{capture_path};
  if (!capture_file) {
    log->critical("backtest — cannot open capture file {}",
                  capture_path.string());
    return 1;
  }

  auto transport = std::make_unique<kalshi::PaperTransport>();
  kalshi::PaperTransport *paper = transport.get();
  kalshi::RestClient rest{kalshi::Auth{"paper", ephemeral_rsa_pem()},
                          std::move(transport), app_config.base_url};
  kalshi::OrderManager order_mgr{rest};
  kalshi::RiskManager risk_mgr{app_config.risk};
  kalshi::FlowImbalanceGuard flow_guard{app_config.flow};
  auto pricing_model = [&app_config]() -> kalshi::FairValueEngine {
    if (app_config.quoter.use_clearing_pricing) {
      return kalshi::FairValueEngine{
          std::make_unique<kalshi::ClearingPriceModel>(
              app_config.quoter.clearing_tape_weight)};
    }
    if (app_config.quoter.use_view_based_pricing) {
      return kalshi::FairValueEngine{std::make_unique<kalshi::ViewBasedModel>(
          app_config.quoter.view_debias_beta)};
    }
    return kalshi::FairValueEngine{std::make_unique<kalshi::HeuristicModel>()};
  }();
  kalshi::Quoter quoter{app_config.quoter, std::move(pricing_model), order_mgr,
                        risk_mgr, &flow_guard};
  kalshi::TradingSession session{{}, order_mgr, risk_mgr, quoter, &flow_guard};
  kalshi::TradeTape trade_tape{kalshi::TradeTapeConfig{}};
  session.set_trade_tape(&trade_tape);
  quoter.set_trade_tape(&trade_tape);

  std::unordered_map<std::string, double> realized_cents;
  session.set_pnl_listener(
      [&realized_cents](const kalshi::TradingSession::PnlMap &pnl) {
        for (const auto &[ticker, cents] : pnl) {
          realized_cents[ticker] = cents;
        }
      });

  kalshi::WebSocketClient client{kalshi::Auth{"replay", ""},
                                 std::make_unique<kalshi::NullWebSocket>(),
                                 "wss://replay.invalid/ws", 0};
  client.on_orderbook_snapshot([&session](const kalshi::Orderbook &snapshot) {
    if (session.is_tracked(snapshot.ticker)) {
      session.on_snapshot(snapshot);
    } else {
      session.add_market(snapshot);
    }
  });
  client.on_orderbook_delta([&session](const std::string &ticker,
                                       kalshi::Side side, int price_cents,
                                       kalshi::Quantity delta) {
    session.on_delta(ticker, side, price_cents, delta);
  });
  long long prints = 0;
  int sim_fills = 0;
  client.on_trade([&](const kalshi::PublicTrade &print) {
    ++prints;
    sim_fills += simulate_maker_fills(*paper, &session, print);
    session.on_trade(print);
  });

  long long frames = 0;
  std::string line;
  while (std::getline(capture_file, line)) {
    if (!line.empty()) {
      client.inject_frame(line);
      ++frames;
    }
  }

  constexpr double kCentsPerDollarOut = 100.0;
  out << std::format("backtest {}  frames={} prints={} sim_fills={}\n",
                     capture_path.string(), frames, prints, sim_fills);
  out << std::format("{:<42}{:>10}{:>14}\n", "ticker", "net_pos", "realized_$");
  for (const auto &ticker : session.tickers()) {
    const auto realized = realized_cents.contains(ticker)
                              ? realized_cents.at(ticker) / kCentsPerDollarOut
                              : 0.0;
    out << std::format("{:<42}{:>10}{:>14.2f}\n", ticker,
                       order_mgr.net_position(ticker).to_fp_string(), realized);
  }
  log->info("backtest — {} frames, {} prints, {} simulated fills", frames,
            prints, sim_fills);
  return 0;
}

// ---- FV replay mode ----

// Replays a captured session (raw WS frames, one per line) through the
// production parse path and the FvBacktest candidate grid, printing the
// tick-scale scoreboard (BETTER_PRICING.md Phase 3b). No credentials or
// config needed — the capture file is the only input.
int run_fv_replay_mode(const std::filesystem::path &capture_path,
                       std::ostream &out,
                       std::shared_ptr<spdlog::logger> &log) {
  std::ifstream capture_file{capture_path};
  if (!capture_file) {
    log->critical("fv-replay — cannot open capture file {}",
                  capture_path.string());
    return 1;
  }

  kalshi::FvBacktest backtest{kalshi::FvBacktestConfig::defaults()};
  kalshi::WebSocketClient client{kalshi::Auth{"replay", ""},
                                 std::make_unique<kalshi::NullWebSocket>(),
                                 "wss://replay.invalid/ws", 0};
  client.on_orderbook_snapshot([&backtest](const kalshi::Orderbook &snapshot) {
    backtest.on_snapshot(snapshot);
  });
  client.on_orderbook_delta([&backtest](const std::string &ticker,
                                        kalshi::Side side, int price_cents,
                                        kalshi::Quantity delta) {
    backtest.on_delta(ticker, side, price_cents, delta);
  });
  client.on_trade([&backtest](const kalshi::PublicTrade &trade) {
    backtest.on_trade(trade);
  });
  client.on_fill(
      [&backtest](const kalshi::Fill &fill) { backtest.on_fill(fill); });

  long long frames = 0;
  std::string line;
  while (std::getline(capture_file, line)) {
    if (!line.empty()) {
      client.inject_frame(line);
      ++frames;
    }
  }

  const auto scores = backtest.scores();
  out << std::format("{:<40}{:>8}{:>10}{:>10}\n", "candidate", "events",
                     "MAE(c)", "bias(c)");
  for (const auto &score : scores) {
    out << std::format("{:<40}{:>8}{:>10.3f}{:>+10.3f}\n", score.candidate,
                       score.events, score.mae_cents, score.bias_cents);
  }
  log->info("fv-replay — {} frames from {}, {} candidates", frames,
            capture_path.string(), scores.size());
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
  std::vector<std::string> tickers = app_config.target_tickers;
  if (tickers.empty()) {
    log->info("capture mode — selecting top {} live market(s)",
              app_config.scanner.trade_top_n);
    kalshi::RestClient scan_rest{
        auth, std::make_unique<kalshi::HttpTransport>(), app_config.base_url};
    tickers = scan_top_tickers(scan_rest, app_config, log);
  }
  if (tickers.empty()) {
    log->critical("capture mode — scanner found no live markets and "
                  "target_tickers is empty; nothing to record");
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
  for (const auto &ticker : tickers) {
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
