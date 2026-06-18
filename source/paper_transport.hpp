#pragma once

#include "http_transport.hpp"
#include "types.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kalshi {

// Paper-trading implementation of IHttpTransport.
//
// Intercepts place/cancel/list order requests and returns synthetic JSON
// responses without hitting the exchange. Fills are simulated by calling
// simulate_fill(). Use this as a drop-in replacement for HttpTransport when
// running the market maker with --paper.
class PaperTransport : public IHttpTransport {
public:
  PaperTransport() = default;
  ~PaperTransport() override = default;

  PaperTransport(const PaperTransport &) = delete;
  PaperTransport &operator=(const PaperTransport &) = delete;
  PaperTransport(PaperTransport &&) = delete;
  PaperTransport &operator=(PaperTransport &&) = delete;

  // ---- IHttpTransport ----

  // GET /portfolio/orders → returns the current open order list.
  // All other GETs return an empty 200 (not used by the main loop).
  [[nodiscard]] HttpResponse
  get(std::string_view url,
      const std::map<std::string, std::string> &headers) override;

  // POST /portfolio/orders → logs the order and returns a synthetic 201.
  [[nodiscard]] HttpResponse
  post(std::string_view url, const std::map<std::string, std::string> &headers,
       std::string_view body) override;

  // DELETE /portfolio/orders/{id} → removes the order and returns 200.
  [[nodiscard]] HttpResponse
  delete_(std::string_view url,
          const std::map<std::string, std::string> &headers) override;

  // ---- Paper trading controls ----

  // Simulates a partial or full fill for the given order.
  // Returns false if the order is not found or already fully filled.
  bool simulate_fill(const std::string &order_id, int fill_quantity);

  // ---- Inspection ----

  [[nodiscard]] const std::vector<Order> &open_orders() const;
  [[nodiscard]] const std::vector<Fill> &fills() const;

private:
  std::string next_order_id();

  std::vector<Order> open_orders_;
  std::vector<Fill> fills_;
  int next_id_{1};
};

} // namespace kalshi
