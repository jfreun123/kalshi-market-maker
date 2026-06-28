#include "auth.hpp"
#include "config.hpp"
#include "http_transport.hpp"
#include "logger.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "paper_transport.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "ticker_scanner.hpp"
#include "websocket_client.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
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

static void setup_logger(const std::filesystem::path &log_dir) {
  std::filesystem::create_directories(log_dir);
  const auto log_path = log_dir / "app.log";

  constexpr std::size_t kMaxLogBytes = 20UL * 1024UL * 1024UL;
  constexpr std::size_t kMaxLogFiles = 14U;

  auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      log_path.string(), kMaxLogBytes, kMaxLogFiles);
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  auto logger = std::make_shared<spdlog::logger>(
      "kalshi", spdlog::sinks_init_list{console_sink, file_sink});
  logger->set_level(spdlog::level::info);
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
  logger->flush_on(spdlog::level::warn);

  kalshi::set_logger(logger);
}

// ---- Helpers ----

static constexpr double kCentsPerDollar = 100.0;

static const char *side_name(kalshi::Side side) {
  return side == kalshi::Side::Yes ? "yes" : "no";
}

static double
prior_pnl_for(const std::unordered_map<std::string, double> &prior_pnl,
              const std::string &ticker) {
  return prior_pnl.contains(ticker) ? prior_pnl.at(ticker) : 0.0;
}

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

// ---- WS event handlers ----

using ObMap = std::unordered_map<std::string, kalshi::LocalOrderbook>;
using PnlMap = std::unordered_map<std::string, double>;

static void handle_snapshot(ObMap &ob_map, const kalshi::Orderbook &snap,
                            std::shared_ptr<spdlog::logger> &log) {
  ob_map[snap.ticker].apply_snapshot(snap);
  log->info("snapshot ticker={} yes_levels={} no_levels={}", snap.ticker,
            snap.yes.size(), snap.no.size());
}

static void handle_delta(ObMap &ob_map, kalshi::Quoter &quoter,
                         const std::string &ticker, kalshi::Side side,
                         int price, int qty,
                         std::shared_ptr<spdlog::logger> &log) {
  ob_map[ticker].apply_delta(side, price, qty);
  log->debug("delta ticker={} side={} price={} qty={} mid={:.1f} spread={}",
             ticker, side_name(side), price, qty,
             ob_map[ticker].mid_price_cents(), ob_map[ticker].spread_cents());
  try {
    quoter.update(ticker, ob_map[ticker]);
  } catch (const std::exception &ex) {
    log->error("quoter error ticker={}: {}", ticker, ex.what());
  }
}

static void handle_fill(kalshi::IOrderManager &order_mgr,
                        kalshi::RiskManager &risk_mgr,
                        const kalshi::AppConfig &app_config, PnlMap &prior_pnl,
                        const std::filesystem::path &pnl_path,
                        std::shared_ptr<spdlog::logger> &log,
                        const kalshi::Fill &fill) {
  order_mgr.record_fill(fill);
  risk_mgr.update(order_mgr, app_config.target_tickers);

  const double session_pnl = order_mgr.realized_pnl(fill.market_ticker);
  const double total_pnl =
      prior_pnl_for(prior_pnl, fill.market_ticker) + session_pnl;

  log->info("fill ticker={} side={} price={} qty={} is_taker={} "
            "session_pnl=${:.2f} total_pnl=${:.2f}",
            fill.market_ticker, side_name(fill.side), fill.price_cents,
            fill.quantity, fill.is_taker, session_pnl / kCentsPerDollar,
            total_pnl / kCentsPerDollar);

  if (risk_mgr.is_halted()) {
    log->critical("risk halted constraints={}", risk_mgr.active_constraints());
  }

  prior_pnl[fill.market_ticker] = total_pnl;

  try {
    nlohmann::json json_pnl = prior_pnl;
    std::ofstream file{pnl_path};
    file << json_pnl.dump(2);
  } catch (...) {
    log->warn("pnl_state: failed to write {}", pnl_path.string());
  }
}

// ---- Status logging ----

static void log_status_snapshot(const std::vector<std::string> &tickers,
                                const kalshi::IOrderManager &order_mgr,
                                const kalshi::RiskManager &risk_mgr,
                                const PnlMap &prior_pnl,
                                std::shared_ptr<spdlog::logger> &log) {
  for (const auto &ticker : tickers) {
    const int pos = order_mgr.net_position(ticker);
    const double session_pnl = order_mgr.realized_pnl(ticker);
    const double prior = prior_pnl_for(prior_pnl, ticker);
    log->info("status ticker={} net_pos={} session_pnl_dollars={:.2f} "
              "total_pnl_dollars={:.2f} halted={} constraints={}",
              ticker, pos, session_pnl / kCentsPerDollar,
              (prior + session_pnl) / kCentsPerDollar, risk_mgr.is_halted(),
              risk_mgr.active_constraints());
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

// Generates the known Kalshi economic event series for the next six months.
// Format: KXCPI-YYMMx (monthly CPI) and KXFED-YYMMx (FOMC meeting months).
static std::vector<std::string>
generate_upcoming_series(std::chrono::system_clock::time_point now) {
  constexpr int kMonthsAhead = 6;

  // FOMC typically meets in Jan, Mar, May, Jun, Jul, Sep, Nov, Dec.
  // Approximate by including all months — the scanner filter handles empty
  // results from series that don't exist yet.
  const std::time_t now_tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_now{};
  gmtime_r(&now_tt, &tm_now);

  std::vector<std::string> series;
  series.reserve(static_cast<std::size_t>(kMonthsAhead) * 2U);

  for (int offset = 0; offset < kMonthsAhead; ++offset) {
    const int month = ((tm_now.tm_mon + offset) % 12) + 1;
    const int year = (tm_now.tm_year + 1900) +
                     (tm_now.tm_mon + offset) / 12; // NOLINT(*-magic-numbers)
    constexpr int kYearModulo = 100;
    const int year_2digit = year % kYearModulo;

    std::array<char, 16> buf{}; // NOLINT(*-magic-numbers)
    // Month letter: A=Jan, B=Feb, ..., L=Dec
    const char month_letter = static_cast<char>('A' + month - 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::snprintf(buf.data(), buf.size(), "%02d%c", year_2digit, month_letter);
    const std::string suffix{buf.data()};

    series.push_back("KXCPI-" + suffix);
    series.push_back("KXFED-" + suffix);
  }
  return series;
}

static int run_scan_mode(kalshi::RestClient &rest,
                         std::shared_ptr<spdlog::logger> &log) {
  constexpr int kScanTopN = 20;
  log->info("scanner mode — scanning active markets");

  const auto now = std::chrono::system_clock::now();
  kalshi::ScannerConfig config;
  config.event_series = generate_upcoming_series(now);
  for (const auto &series : config.event_series) {
    log->info("scanner searching series={}", series);
  }

  kalshi::TickerScanner scanner{rest, config};
  const auto results = scanner.scan(kScanTopN, now);
  if (results.empty()) {
    log->warn("no markets passed scanner filters");
    return 0;
  }
  log->info("scanner results (top {}):", results.size());
  for (std::size_t rank = 0U; rank < results.size(); ++rank) {
    const auto &market = results[rank];
    log->info("  {:>2}. ticker={} mid={}c spread={}c vol=${:.0f} days={:.1f} "
              "score={:.3f} cat={} \"{}\"",
              rank + 1U, market.ticker, market.mid_price_cents,
              market.spread_cents, market.volume_usd, market.days_to_close,
              market.score, market.category, market.title);
  }
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

// ---- Arg parsing ----

struct CliArgs {
  bool paper_mode{false};
  bool scan_mode{false};
  std::filesystem::path config_path{"config.json"};
};

static CliArgs parse_args(std::span<char *> args) {
  CliArgs result;
  for (std::size_t idx = 1U; idx < args.size(); ++idx) {
    const std::string_view arg{args[idx]};
    if (arg == "--paper") {
      result.paper_mode = true;
    } else if (arg == "--scan") {
      result.scan_mode = true;
    } else {
      result.config_path = std::filesystem::path{args[idx]};
    }
  }
  return result;
}

// ---- Entry point ----

int main(int argc, char *argv[]) {
  try {
    const auto cli =
        parse_args(std::span<char *>(argv, static_cast<std::size_t>(argc)));
    const kalshi::AppConfig app_config = kalshi::load_config(cli.config_path);

    setup_logger(std::filesystem::path{app_config.log_dir});
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

    auto [http_transport, paper_ptr] = make_http_transport(cli.paper_mode, log);
    kalshi::RestClient rest{auth, std::move(http_transport),
                            app_config.base_url};

    if (cli.scan_mode) {
      return run_scan_mode(rest, log);
    }

    if (app_config.target_tickers.empty()) {
      log->critical(
          "target_tickers is empty — add tickers to config or use --scan");
      return 1;
    }

    kalshi::WebSocketClient ws_client{
        auth, std::make_unique<kalshi::IxWebSocket>(), app_config.ws_url};

    ObMap ob_map;
    kalshi::OrderManager order_mgr{rest};
    kalshi::RiskManager risk_mgr{app_config.risk};
    kalshi::Quoter quoter{app_config.quoter, order_mgr, risk_mgr};

    const std::filesystem::path pnl_path{"pnl_state.json"};
    auto prior_pnl = load_pnl(pnl_path);
    for (const auto &[ticker, cents] : prior_pnl) {
      log->info("pnl_state loaded ticker={} prior_pnl_dollars={:.2f}", ticker,
                cents / kCentsPerDollar);
    }

    for (const auto &ticker : app_config.target_tickers) {
      log->info("seeding orderbook ticker={}", ticker);
      auto snap = rest.get_orderbook(ticker);
      ob_map[ticker].apply_snapshot(snap);
      ws_client.subscribe(ticker);
      quoter.update(ticker, ob_map[ticker]);
    }

    ws_client.on_orderbook_snapshot(
        [&ob_map, &log](const kalshi::Orderbook &snap) {
          handle_snapshot(ob_map, snap, log);
        });

    ws_client.on_orderbook_delta(
        [&ob_map, &quoter, &log](const std::string &ticker, kalshi::Side side,
                                 int price, int qty) {
          handle_delta(ob_map, quoter, ticker, side, price, qty, log);
        });

    ws_client.on_fill([&order_mgr, &risk_mgr, &app_config, &prior_pnl,
                       &pnl_path, &log](const kalshi::Fill &fill) {
      handle_fill(order_mgr, risk_mgr, app_config, prior_pnl, pnl_path, log,
                  fill);
    });

    ws_client.on_disconnect([&order_mgr, &app_config, &log]() {
      log->warn("ws disconnected — cancelling all open orders");
      for (const auto &ticker : app_config.target_tickers) {
        order_mgr.cancel_all(ticker);
      }
    });

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::thread ws_thread([&ws_client]() { ws_client.run(); });

    constexpr auto kPollInterval = std::chrono::milliseconds{100};
    constexpr int kPositionLogInterval = 600;    // 600 × 100ms = 60s
    constexpr int kStalenessCheckInterval = 300; // 300 × 100ms = 30s

    int poll_count = 0;
    bool stale_logged = false;

    while (!g_shutdown.load()) {
      std::this_thread::sleep_for(kPollInterval);
      ++poll_count;

      if (poll_count % kStalenessCheckInterval == 0) {
        check_ws_staleness(ws_client, risk_mgr, log, stale_logged);
      }
      if (poll_count % kPositionLogInterval == 0) {
        log_status_snapshot(app_config.target_tickers, order_mgr, risk_mgr,
                            prior_pnl, log);
      }
    }

    log->info("shutdown signal received — stopping");
    ws_client.stop();
    ws_thread.join();

    for (const auto &ticker : app_config.target_tickers) {
      order_mgr.cancel_all(ticker);
    }

    if (paper_ptr != nullptr) {
      log->info("paper mode: simulated fills={}", paper_ptr->fills().size());
    }

    log->info("shutdown complete");
    return 0;

  } catch (const std::exception &exception) {
    std::cerr << "fatal: " << exception.what() << '\n';
    return 1;
  }
}
