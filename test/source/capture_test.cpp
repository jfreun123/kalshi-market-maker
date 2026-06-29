#include "capture.hpp"

#include "fake_transport.hpp"
#include "fake_websocket.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <sstream>
#include <string>

namespace {

constexpr int kHttpOk = 200;
constexpr std::size_t kTwo = 2;

const std::string kUrl = "https://demo-api.kalshi.co/x";
const std::string kWsUrl = "wss://demo-api.kalshi.co/ws";
const std::string kMsgA = R"({"type":"orderbook_snapshot"})";
const std::string kMsgB = R"({"type":"orderbook_delta"})";

} // namespace

TEST(CapturingWebSocketTest, TeesEachFrameToStreamAndForwardsToHandler) {
  auto fake = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *fake_ptr = fake.get();
  std::ostringstream out;
  kalshi::CapturingWebSocket capture{std::move(fake), out};

  std::size_t forwarded = 0;
  capture.on_message([&forwarded](const std::string &) { ++forwarded; });
  fake_ptr->enqueue_message(kMsgA);
  fake_ptr->enqueue_message(kMsgB);
  capture.run();

  EXPECT_EQ(forwarded, kTwo);
  EXPECT_EQ(capture.captured_count(), kTwo);
  // One raw frame per line, in order — directly replay-compatible.
  EXPECT_EQ(out.str(), kMsgA + "\n" + kMsgB + "\n");
}

TEST(CapturingWebSocketTest, ForwardsLifecycleCallsToInner) {
  auto fake = std::make_unique<kalshi::FakeWebSocket>();
  kalshi::FakeWebSocket *fake_ptr = fake.get();
  std::ostringstream out;
  kalshi::CapturingWebSocket capture{std::move(fake), out};

  capture.connect(kWsUrl, {});
  capture.send("subscribe");

  EXPECT_EQ(fake_ptr->connect_count(), 1);
  EXPECT_EQ(fake_ptr->connected_url(), kWsUrl);
  ASSERT_EQ(fake_ptr->sent_messages().size(), 1U);
  EXPECT_EQ(fake_ptr->sent_messages().front(), "subscribe");
}

TEST(CapturingHttpTransportTest, RecordsResponseAndReturnsItUnchanged) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport *fake_ptr = fake.get();
  fake_ptr->enqueue({kHttpOk, R"({"orderbook":{}})"});
  std::ostringstream out;
  kalshi::CapturingHttpTransport capture{std::move(fake), out};

  const auto response = capture.get(kUrl, {});

  EXPECT_EQ(response.status_code, kHttpOk);
  EXPECT_EQ(response.body, R"({"orderbook":{}})");
  EXPECT_EQ(capture.captured_count(), 1U);

  const auto record = nlohmann::json::parse(out.str());
  EXPECT_EQ(record.at("method").get<std::string>(), "GET");
  EXPECT_EQ(record.at("url").get<std::string>(), kUrl);
  EXPECT_EQ(record.at("status").get<int>(), kHttpOk);
  EXPECT_EQ(record.at("response_body").get<std::string>(),
            R"({"orderbook":{}})");
}

TEST(CapturingHttpTransportTest, RecordsRequestBodyForPost) {
  auto fake = std::make_unique<FakeTransport>();
  fake->enqueue({kHttpOk, "{}"});
  std::ostringstream out;
  kalshi::CapturingHttpTransport capture{std::move(fake), out};

  (void)capture.post(kUrl, {}, R"({"ticker":"T"})");

  const auto record = nlohmann::json::parse(out.str());
  EXPECT_EQ(record.at("method").get<std::string>(), "POST");
  EXPECT_EQ(record.at("request_body").get<std::string>(), R"({"ticker":"T"})");
}
