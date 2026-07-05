#include "analytics.hpp"
#include "auth.hpp"
#include "capture.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "ensure.hpp"
#include "fair_value.hpp"
#include "flow_imbalance.hpp"
#include "http_transport.hpp"
#include "logger.hpp"
#include "order_manager.hpp"
#include "paper_transport.hpp"
#include "portfolio.hpp"
#include "pricing_model.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "scan_output.hpp"
#include "ticker_scanner.hpp"
#include "trading_session.hpp"
#include "websocket_client.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---- Shutdown flag ----

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<bool> g_shutdown{false};

extern "C" void handle_signal(int /*sig*/) { g_shutdown.store(true); }

// ---- Logger setup ----

static void setup_logger(const std::filesystem::path &log_dir, bool verbose) {
  std::filesystem::create_directories(log_dir);
  const auto log_path = log_dir / "app.log";

  constexpr int kRotationHour = 0;
  constexpr int kRotationMinute = 0;
  constexpr bool kTruncate = false;
  constexpr std::uint16_t kMaxLogFiles = 14U;

  auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
      log_path.string(), kRotationHour, kRotationMinute, kTruncate,
      kMaxLogFiles);
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  auto logger = std::make_shared<spdlog::logger>(
      "kalshi", spdlog::sinks_init_list{console_sink, file_sink});
  logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
  logger->flush_on(spdlog::level::warn);

  constexpr auto kFlushInterval = std::chrono::seconds{3};
  spdlog::drop(logger->name());
  spdlog::register_logger(logger);
  spdlog::flush_every(kFlushInterval);

  kalshi::set_logger(logger);
}

// Dedicated raw-line JSONL sink for AnalyticsLogger events (PLAN item 31),
// kept out of the human log so offline analysis never parses log prose.
static std::shared_ptr<spdlog::logger>
make_analytics_spdlog(const std::filesystem::path &log_dir) {
  std::filesystem::create_directories(log_dir);
  constexpr int kRotationHour = 0;
  constexpr int kRotationMinute = 0;
  constexpr bool kTruncate = false;
  constexpr std::uint16_t kMaxLogFiles = 14U;
  auto sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
      (log_dir / "analytics.jsonl").string(), kRotationHour, kRotationMinute,
      kTruncate, kMaxLogFiles);
  auto logger = std::make_shared<spdlog::logger>("analytics", sink);
  logger->set_pattern("%v");
  spdlog::drop(logger->name());
  spdlog::register_logger(logger);
  return logger;
}

// ---- Helpers ----

static constexpr double kCentsPerDollar = 100.0;

// Runs a cleanup action when it leaves scope, on every path (return, throw,
// signal-driven shutdown). Non-copyable, non-movable.
template <typename Fn> class ScopeGuard {
public:
  explicit ScopeGuard(Fn action) : fn_{std::move(action)} {}
  ScopeGuard(const ScopeGuard &) = delete;
  ScopeGuard &operator=(const ScopeGuard &) = delete;
  ScopeGuard(ScopeGuard &&) = delete;
  ScopeGuard &operator=(ScopeGuard &&) = delete;
  ~ScopeGuard() { fn_(); }

private:
  Fn fn_;
};

// Returns {transport, paper_ptr}. paper_ptr is non-null only in paper mode.
static auto make_http_transport(bool paper_mode,
                                std::shared_ptr<spdlog::logger> &log)
    -> std::pair<std::unique_ptr<kalshi::IHttpTransport>,
                 kalshi::PaperTransport *> {
  if (paper_mode) {
    log->info("paper trading mode — no live orders will be placed");
    auto paper = std::make_unique<kalshi::PaperTransport>();
    auto *ptr = paper.get();
    return {std::move(paper), ptr};
  }
  return {std::make_unique<kalshi::HttpTransport>(), nullptr};
}

// ---- PnL persistence ----

using PnlMap = std::unordered_map<std::string, double>;

// Persists the carried per-ticker PnL map; wired as the session's fill
// listener.
static void persist_pnl(const std::filesystem::path &pnl_path,
                        const PnlMap &pnl,
                        std::shared_ptr<spdlog::logger> &log) {
  try {
    const nlohmann::json json_pnl = pnl;
    std::ofstream file{pnl_path};
    file << json_pnl.dump(2);
  } catch (...) {
    log->warn("pnl_state: failed to write {}", pnl_path.string());
  }
}

static void check_ws_staleness(const kalshi::WebSocketClient &ws_client,
                               kalshi::RiskManager &risk_mgr,
                               std::shared_ptr<spdlog::logger> &log,
                               bool &stale_logged) {
  constexpr auto kStalenessThreshold = std::chrono::seconds{30};
  const auto since_last =
      std::chrono::steady_clock::now() - ws_client.last_message_time();
  if (since_last > kStalenessThreshold) {
    risk_mgr.set(kalshi::Constraint::kStaleBook);
    if (!stale_logged) {
      log->critical(
          "ws stale — no message in {}s, quoter halted",
          std::chrono::duration_cast<std::chrono::seconds>(since_last).count());
      stale_logged = true;
    }
  } else if (risk_mgr.is_set(kalshi::Constraint::kStaleBook)) {
    risk_mgr.clear(kalshi::Constraint::kStaleBook);
    stale_logged = false;
    log->info("ws recovered — stale constraint cleared");
  }
}

// ---- Scanner mode ----

static constexpr auto kScanResultsPath = "scan_results.json";

// Derives the generated trade-config path from the base config path by
// inserting ".trade" before the extension: config.json -> config.trade.json,
// config.demo.json -> config.demo.trade.json.
static std::filesystem::path
trade_config_path_for(const std::filesystem::path &base_config_path) {
  const auto stem = base_config_path.stem().string();
  const auto ext = base_config_path.extension().string();
  return base_config_path.parent_path() / (stem + ".trade" + ext);
}

static int run_scan_mode(kalshi::RestClient &rest,
                         const kalshi::ScannerConfig &scanner_config,
                         const std::filesystem::path &config_path,
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

  // Generate a ready-to-run trade config with the top-N tickers filled in.
  const auto top_n = std::min(
      static_cast<std::size_t>(scanner_config.trade_top_n), results.size());
  std::vector<std::string> top_tickers;
  top_tickers.reserve(top_n);
  for (std::size_t idx = 0U; idx < top_n; ++idx) {
    top_tickers.push_back(results[idx].ticker);
  }

  const auto trade_path = trade_config_path_for(config_path);
  if (kalshi::write_trade_config(config_path, trade_path, top_tickers)) {
    log->info("trade config written to {} (top {} tickers) — run: {} {}",
              trade_path.string(), top_tickers.size(), "kalshi_mm",
              trade_path.string());
  } else {
    log->warn("failed to write trade config to {}", trade_path.string());
  }
  return 0;
}

// ---- Reconciliation against the exchange ----

// Fetches the exchange's authoritative positions and compares them to local
// accounting. On drift, logs every mismatch; if risk_mgr is non-null (live
// trading) it trips kModelDiverge to halt all quoting. Returns true if in sync.
static bool reconcile_against_exchange(kalshi::RestClient &rest,
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
static void rotate_markets(kalshi::RestClient &rest,
                           kalshi::TradingSession &session,
                           kalshi::WebSocketClient &ws_client,
                           const kalshi::AppConfig &app_config,
                           std::mutex &engine_mtx,
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
static int run_reconcile_mode(kalshi::RestClient &rest,
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
static int flatten_all_positions(kalshi::RestClient &rest,
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
static int run_flatten_mode(kalshi::RestClient &rest,
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
static int run_capture_mode(const kalshi::Auth &auth,
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

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::thread ws_thread([&ws_client]() { ws_client.run(); });

  log->info("capturing — ws={} rest={} — Ctrl-C to stop", ws_path.string(),
            rest_path.string());

  constexpr auto kPollInterval = std::chrono::milliseconds{200};
  constexpr int kProgressInterval = 50; // 50 × 200ms = 10s
  int poll_count = 0;
  while (!g_shutdown.load()) {
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

// ---- PnL persistence ----

static PnlMap load_pnl(const std::filesystem::path &path) {
  PnlMap result;
  if (!std::filesystem::exists(path)) {
    return result;
  }
  try {
    std::ifstream file{path};
    const auto json = nlohmann::json::parse(file);
    for (const auto &[ticker, val] : json.items()) {
      result[ticker] = val.get<double>();
    }
  } catch (...) {
    kalshi::get_logger()->warn("pnl_state: failed to load {}, starting fresh",
                               path.string());
  }
  return result;
}

// ---- Entry point ----

int main(int argc, char *argv[]) {
  try {
    const auto cli = kalshi::parse_args(
        std::span<char *>(argv, static_cast<std::size_t>(argc)));
    const kalshi::AppConfig app_config = kalshi::load_config(cli.config_path);

    setup_logger(std::filesystem::path{app_config.log_dir}, cli.verbose);
    auto log = kalshi::get_logger();

    log->info("startup mode={} tickers={} base_url={}",
              cli.paper_mode ? "paper" : "live",
              app_config.target_tickers.size(), app_config.base_url);
    for (const auto &ticker : app_config.target_tickers) {
      log->info("startup ticker={}", ticker);
    }

    std::ifstream key_file{app_config.private_key_path};
    if (!key_file) {
      log->critical("cannot open private key path={}",
                    app_config.private_key_path);
      return 1;
    }
    const std::string private_key_pem{std::istreambuf_iterator<char>{key_file},
                                      std::istreambuf_iterator<char>{}};

    kalshi::Auth auth{app_config.api_key, private_key_pem};

    auto analytics_logger = make_analytics_spdlog(app_config.log_dir);
    kalshi::AnalyticsLogger analytics{
        [analytics_logger](const std::string &line) {
          analytics_logger->info(line);
        }};

    auto [http_transport, paper_ptr] = make_http_transport(cli.paper_mode, log);
    if (auto *live_transport =
            dynamic_cast<kalshi::HttpTransport *>(http_transport.get())) {
      live_transport->set_latency_observer(
          [&analytics](std::string_view method, std::string_view path,
                       int status, long long rtt_ms) {
            analytics.http_latency(method, path, status, rtt_ms);
          });
    }
    kalshi::RestClient rest{auth, std::move(http_transport),
                            app_config.base_url};

    if (cli.scan_mode) {
      return run_scan_mode(rest, app_config.scanner, cli.config_path, log);
    }

    if (cli.reconcile_mode) {
      return run_reconcile_mode(rest, app_config.target_tickers, log);
    }

    if (cli.flatten_mode) {
      return run_flatten_mode(rest, log);
    }

    if (cli.capture_mode) {
      return run_capture_mode(auth, app_config, cli.capture_dir, log);
    }

    if (app_config.target_tickers.empty()) {
      log->critical(
          "target_tickers is empty — add tickers to config or use --scan");
      return 1;
    }

    kalshi::WebSocketClient ws_client{
        auth, std::make_unique<kalshi::IxWebSocket>(), app_config.ws_url};

    kalshi::OrderManager order_mgr{rest};
    kalshi::RiskManager risk_mgr{app_config.risk};
    kalshi::FlowImbalanceGuard flow_guard{app_config.flow};
    // Pricing model: debiased "view" pricing when enabled, else the heuristic.
    auto pricing_model =
        app_config.quoter.use_view_based_pricing
            ? kalshi::FairValueEngine{std::make_unique<kalshi::ViewBasedModel>(
                  app_config.quoter.view_debias_beta)}
            : kalshi::FairValueEngine{
                  std::make_unique<kalshi::HeuristicModel>()};
    log->info("pricing model={}", app_config.quoter.use_view_based_pricing
                                      ? "view_based"
                                      : "heuristic");
    kalshi::Quoter quoter{app_config.quoter, std::move(pricing_model),
                          order_mgr, risk_mgr, &flow_guard};
    kalshi::TradingSession session{app_config.target_tickers, order_mgr,
                                   risk_mgr, quoter, &flow_guard};

    quoter.set_analytics(&analytics);
    session.set_analytics(&analytics);

    kalshi::set_panic_handler([&session]() { session.cancel_all_quotes(); });
    kalshi::install_crash_flatten_handlers();

    const std::filesystem::path pnl_path{"pnl_state.json"};
    auto prior_pnl = load_pnl(pnl_path);
    for (const auto &[ticker, cents] : prior_pnl) {
      log->info("pnl_state loaded ticker={} prior_pnl_dollars={:.2f}", ticker,
                cents / kCentsPerDollar);
    }
    session.set_prior_pnl(std::move(prior_pnl));
    session.set_pnl_listener([&pnl_path, &log](const PnlMap &pnl) {
      persist_pnl(pnl_path, pnl, log);
    });

    if (paper_ptr == nullptr) {
      try {
        session.cancel_preexisting_orders(rest.get_open_orders());
      } catch (const std::exception &ex) {
        log->warn("startup: could not fetch/cancel pre-existing orders: {}",
                  ex.what());
      }
    }

    for (const auto &ticker : app_config.target_tickers) {
      // Contain per-ticker startup failures (closed/invalid market, transient
      // REST error): skip that ticker rather than aborting the whole session.
      try {
        auto snap = rest.get_orderbook(ticker);
        session.seed_orderbook(snap);
        ws_client.subscribe(ticker);
      } catch (const std::exception &ex) {
        log->error("startup seed failed ticker={}: {} — skipping", ticker,
                   ex.what());
      }
    }

    // The WS callbacks fire on the WebSocket thread and mutate the engine
    // (orderbook, orders, risk) on every message; the main loop reads and
    // flattens it. One mutex serializes the two threads so that shared access
    // is well-defined.
    std::mutex engine_mtx;

    ws_client.on_orderbook_snapshot(
        [&session, &engine_mtx](const kalshi::Orderbook &snap) {
          const std::lock_guard<std::mutex> lock{engine_mtx};
          session.on_snapshot(snap);
        });

    ws_client.on_orderbook_delta(
        [&session, &engine_mtx](const std::string &ticker, kalshi::Side side,
                                int price, kalshi::Quantity qty) {
          const std::lock_guard<std::mutex> lock{engine_mtx};
          session.on_delta(ticker, side, price, qty);
        });

    // Timestamp floor for the reconnect fill backfill: fills can only be
    // missed after the session started, and after the last fill the WS
    // channel delivered.
    auto last_fill_time =
        std::make_shared<std::chrono::system_clock::time_point>(
            std::chrono::system_clock::now());

    ws_client.on_fill(
        [&session, &engine_mtx, last_fill_time](const kalshi::Fill &fill) {
          const std::lock_guard<std::mutex> lock{engine_mtx};
          *last_fill_time = std::max(*last_fill_time, fill.timestamp);
          session.on_fill(fill);
        });

    ws_client.on_disconnect([&session, &engine_mtx]() {
      const std::lock_guard<std::mutex> lock{engine_mtx};
      session.on_disconnect();
    });

    // Fills that land while the WS is down are never re-pushed; without a
    // backfill the local model silently diverges until reconcile halts on
    // kModelDiverge (demo finding D10). The dedup by trade_id makes the
    // overlap window replay-safe.
    ws_client.on_reconnect([&rest, &session, &engine_mtx, last_fill_time,
                            &app_config, &log]() {
      constexpr std::chrono::seconds kBackfillOverlap{60};
      const std::lock_guard<std::mutex> lock{engine_mtx};
      const long long min_ts =
          std::chrono::duration_cast<std::chrono::seconds>(
              (*last_fill_time - kBackfillOverlap).time_since_epoch())
              .count();
      try {
        const auto missed = rest.get_fills(min_ts);
        int replayed = 0;
        for (const auto &fill : missed) {
          const auto &tickers = app_config.target_tickers;
          if (std::ranges::find(tickers, fill.market_ticker) == tickers.end()) {
            continue;
          }
          *last_fill_time = std::max(*last_fill_time, fill.timestamp);
          session.on_fill(fill);
          ++replayed;
        }
        log->info("reconnect fill backfill: fetched={} replayed={}",
                  missed.size(), replayed);
      } catch (const std::exception &ex) {
        log->error("reconnect fill backfill failed: {}", ex.what());
      }
    });

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::thread ws_thread([&ws_client]() { ws_client.run(); });

    // Guarantees that on ANY exit from this scope — normal shutdown, a thrown
    // exception, or stack unwind — we stop the feed, join the thread, and
    // cancel every resting quote. cancel_all_quotes is best-effort and never
    // throws, so it is safe to run from a destructor; it runs after join()
    // (single-threaded by then) so no lock is needed.
    ScopeGuard shutdown_guard{
        [&ws_client, &ws_thread, &session, &rest, &cli, &log]() {
          log->info("shutting down — stopping feed, cancelling all quotes");
          ws_client.stop();
          if (ws_thread.joinable()) {
            ws_thread.join();
          }
          session.cancel_all_quotes();
          if (!cli.paper_mode) {
            // Verify against exchange truth and retry: run 8's shutdown died
            // mid-flatten with the network down and left live orders behind
            // (item 53). Never assume a fired cancel landed.
            constexpr int kShutdownAttempts = 3;
            constexpr auto kShutdownRetryDelay = std::chrono::seconds{2};
            for (int attempt = 1; attempt <= kShutdownAttempts; ++attempt) {
              try {
                const auto still_open = rest.get_open_orders();
                if (still_open.empty()) {
                  break;
                }
                log->warn("shutdown: {} order(s) still resting (attempt "
                          "{}/{}) — cancelling account-wide",
                          still_open.size(), attempt, kShutdownAttempts);
                session.cancel_preexisting_orders(still_open);
              } catch (const std::exception &ex) {
                log->error("shutdown cancel sweep failed (attempt {}/{}): {}",
                           attempt, kShutdownAttempts, ex.what());
                if (attempt < kShutdownAttempts) {
                  std::this_thread::sleep_for(kShutdownRetryDelay);
                }
              }
            }
            try {
              const int closed = flatten_all_positions(rest, log, &session);
              if (closed > 0) {
                log->info("shutdown — flattened {} position(s) to end flat",
                          closed);
              }
            } catch (const std::exception &ex) {
              log->error("shutdown flatten failed: {}", ex.what());
            }
          }
          kalshi::set_panic_handler(nullptr);
          log->info("shutdown complete");
        }};

    constexpr auto kPollInterval = std::chrono::milliseconds{100};
    constexpr int kStalenessCheckInterval = 300; // 300 × 100ms = 30s
    constexpr int kReconcileInterval = 1200;     // 1200 × 100ms = 120s
    constexpr int kPositionLogInterval = 600;    // 600 × 100ms = 60s
    constexpr int kPortfolioRiskInterval = 10;   // 10 × 100ms = 1s

    constexpr int kTicksPerMinute = 600;
    const int rotation_ticks =
        app_config.scanner.rotation_minutes * kTicksPerMinute;
    int poll_count = 0;
    bool stale_logged = false;

    while (!g_shutdown.load()) {
      std::this_thread::sleep_for(kPollInterval);
      ++poll_count;

      // Rotation runs OUTSIDE the engine lock: its scan is seconds of REST
      // and it takes the lock itself only for the brief session mutations.
      if (paper_ptr == nullptr && rotation_ticks > 0 &&
          poll_count % rotation_ticks == 0) {
        rotate_markets(rest, session, ws_client, app_config, engine_mtx, log);
      }

      // Hold the engine lock only for the work, never across the sleep.
      const std::lock_guard<std::mutex> lock{engine_mtx};
      if (poll_count % kStalenessCheckInterval == 0) {
        check_ws_staleness(ws_client, risk_mgr, log, stale_logged);
        session.requote_idle_markets();
      }
      // Portfolio kill-switch runs more often than the 60s status log so the
      // global halt reacts quickly to aggregate exposure / drawdown.
      if (poll_count % kPortfolioRiskInterval == 0) {
        session.run_portfolio_risk();
      }
      // Flatten on every iteration if any constraint is set (stale, PnL,
      // exposure, drawdown, manual) — never leave quotes resting while halted.
      session.enforce_quote_safety();
      if (poll_count % kPositionLogInterval == 0) {
        session.log_status();
      }
      // Reconcile against the exchange's authoritative positions. Skipped in
      // paper mode (no real exchange positions to compare against).
      if (paper_ptr == nullptr && poll_count % kReconcileInterval == 0) {
        reconcile_against_exchange(rest, order_mgr, app_config.target_tickers,
                                   &risk_mgr, log);
      }
    }

    if (paper_ptr != nullptr) {
      log->info("paper mode: simulated fills={}", paper_ptr->fills().size());
    }
    return 0; // ShutdownGuard stops the feed, joins, and cancels all quotes.

  } catch (const std::exception &exception) {
    std::cerr << "fatal: " << exception.what() << '\n';
    return 1;
  }
}
