#include "auth.hpp"
#include "http_transport.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "websocket_client.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---- Shutdown flag ----

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<bool> g_shutdown{false};

// Signal handler: must be a plain C function (no C++ linkage).
extern "C" void handle_signal(int /*sig*/) { g_shutdown.store(true); }

// ---- Helpers ----

namespace {

// Reads a required environment variable; throws if absent.
std::string require_env(const char *name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe) — called before any threads start
  const char *value = std::getenv(name);
  if (value == nullptr) {
    throw std::runtime_error(std::string{"Required env var not set: "} + name);
  }
  return std::string{value};
}

// Splits a comma-separated string into a vector of trimmed tokens.
std::vector<std::string> split_csv(std::string_view input) {
  std::vector<std::string> tokens;
  std::string current_token;
  for (char character : input) {
    if (character == ',') {
      if (!current_token.empty()) {
        tokens.push_back(std::move(current_token));
        current_token.clear();
      }
    } else {
      current_token += character;
    }
  }
  if (!current_token.empty()) {
    tokens.push_back(std::move(current_token));
  }
  return tokens;
}

} // namespace

// ---- Entry point ----

int main() {
  try {
    // Load credentials and target markets from environment.
    const std::string api_key = require_env("KALSHI_API_KEY");
    const std::string private_key_pem = require_env("KALSHI_PRIVATE_KEY_PEM");
    const std::vector<std::string> tickers =
        split_csv(require_env("KALSHI_TICKERS"));

    if (tickers.empty()) {
      std::cerr << "KALSHI_TICKERS must contain at least one ticker\n";
      return 1;
    }

    // Build components.
    kalshi::Auth auth{api_key, private_key_pem};
    kalshi::RestClient rest{auth, std::make_unique<kalshi::HttpTransport>()};
    kalshi::WebSocketClient ws_client{auth,
                                      std::make_unique<kalshi::IxWebSocket>()};

    std::unordered_map<std::string, kalshi::LocalOrderbook> ob_map;
    kalshi::OrderManager order_mgr{rest};
    kalshi::RiskManager risk_mgr{kalshi::RiskLimits{}};
    kalshi::Quoter quoter{kalshi::QuoterConfig{}, order_mgr, risk_mgr};

    // Seed orderbooks from REST so quotes can fire on the first WS delta.
    for (const auto &ticker : tickers) {
      auto snap = rest.get_orderbook(ticker);
      ob_map[ticker].apply_snapshot(snap);
      ws_client.subscribe(ticker);
    }

    // Wire WebSocket callbacks.
    ws_client.on_orderbook_snapshot([&ob_map](const kalshi::Orderbook &snap) {
      ob_map[snap.ticker].apply_snapshot(snap);
    });

    ws_client.on_orderbook_delta([&ob_map, &quoter](const std::string &ticker,
                                                    kalshi::Side side,
                                                    int price, int qty) {
      ob_map[ticker].apply_delta(side, price, qty);
      quoter.update(ticker, ob_map[ticker]);
    });

    ws_client.on_fill(
        [&order_mgr, &risk_mgr, &tickers](const kalshi::Fill &fill) {
          order_mgr.record_fill(fill);
          risk_mgr.update(order_mgr, tickers);
        });

    // Register signal handlers then start WS in a background thread.
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::thread ws_thread([&ws_client]() { ws_client.run(); });

    // Main thread polls the shutdown flag.
    constexpr auto kShutdownPollInterval = std::chrono::milliseconds{100};
    while (!g_shutdown.load()) {
      std::this_thread::sleep_for(kShutdownPollInterval);
    }

    ws_client.stop();
    ws_thread.join();

    // Cancel all open orders before exiting.
    for (const auto &ticker : tickers) {
      order_mgr.cancel_all(ticker);
    }

    std::cout << "Shutdown complete.\n";
    return 0;

  } catch (const std::exception &exception) {
    std::cerr << "Fatal: " << exception.what() << '\n';
    return 1;
  }
}
