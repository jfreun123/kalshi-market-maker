#include "auth.hpp"
#include "config.hpp"
#include "http_transport.hpp"
#include "order_manager.hpp"
#include "orderbook.hpp"
#include "paper_transport.hpp"
#include "quoter.hpp"
#include "rest_client.hpp"
#include "risk_manager.hpp"
#include "websocket_client.hpp"

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

// Signal handler: must be a plain C function (no C++ linkage).
extern "C" void handle_signal(int /*sig*/) { g_shutdown.store(true); }

// ---- Entry point ----

int main(int argc, char *argv[]) {
  try {
    const auto args = std::span<char *>(argv, static_cast<std::size_t>(argc));

    // Parse CLI flags: [--paper] [config_path]
    bool paper_mode = false;
    std::filesystem::path config_path{"config.json"};
    for (std::size_t arg_index = 1U; arg_index < static_cast<std::size_t>(argc);
         ++arg_index) {
      const std::string_view arg{args[arg_index]};
      if (arg == "--paper") {
        paper_mode = true;
      } else {
        config_path = std::filesystem::path{args[arg_index]};
      }
    }

    const kalshi::AppConfig app_config = kalshi::load_config(config_path);

    // Read private key PEM content from the path in config.
    std::ifstream key_file{app_config.private_key_path};
    if (!key_file) {
      std::cerr << "Cannot open private key: " << app_config.private_key_path
                << '\n';
      return 1;
    }
    const std::string private_key_pem{std::istreambuf_iterator<char>{key_file},
                                      std::istreambuf_iterator<char>{}};

    // Build components — paper mode swaps HttpTransport for PaperTransport.
    kalshi::Auth auth{app_config.api_key, private_key_pem};

    std::unique_ptr<kalshi::IHttpTransport> http_transport;
    kalshi::PaperTransport *paper_transport_ptr = nullptr;
    if (paper_mode) {
      auto paper = std::make_unique<kalshi::PaperTransport>();
      paper_transport_ptr = paper.get();
      http_transport = std::move(paper);
      std::cout << "[paper] Running in paper-trading mode — no live orders.\n";
    } else {
      http_transport = std::make_unique<kalshi::HttpTransport>();
    }

    kalshi::RestClient rest{auth, std::move(http_transport),
                            app_config.base_url};
    kalshi::WebSocketClient ws_client{
        auth, std::make_unique<kalshi::IxWebSocket>(), app_config.ws_url};

    std::unordered_map<std::string, kalshi::LocalOrderbook> ob_map;
    kalshi::OrderManager order_mgr{rest};
    kalshi::RiskManager risk_mgr{app_config.risk};
    kalshi::Quoter quoter{app_config.quoter, order_mgr, risk_mgr};

    // Seed orderbooks from REST so quotes can fire on the first WS delta.
    for (const auto &ticker : app_config.target_tickers) {
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
        [&order_mgr, &risk_mgr, &app_config](const kalshi::Fill &fill) {
          order_mgr.record_fill(fill);
          risk_mgr.update(order_mgr, app_config.target_tickers);
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
    for (const auto &ticker : app_config.target_tickers) {
      order_mgr.cancel_all(ticker);
    }

    if (paper_mode && paper_transport_ptr != nullptr) {
      std::cout << "[paper] Simulated fills: "
                << paper_transport_ptr->fills().size() << '\n';
    }

    std::cout << "Shutdown complete.\n";
    return 0;

  } catch (const std::exception &exception) {
    std::cerr << "Fatal: " << exception.what() << '\n';
    return 1;
  }
}
