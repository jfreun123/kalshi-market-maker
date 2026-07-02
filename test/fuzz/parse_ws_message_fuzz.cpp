// Fuzz target for WebSocketClient message parsing.
//
// Build with: cmake --preset=fuzz && cmake --build --preset=fuzz
// Run with:   ./build-fuzz/test/fuzz/parse_ws_message_fuzz [corpus_dir]
//
// The target feeds arbitrary byte sequences through the full message-parsing
// path (JSON decode → type dispatch → callback). Any crash or ASAN/UBSAN
// finding is a bug.

#include "auth.hpp"
#include "fake_websocket.hpp"
#include "websocket_client.hpp"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace {

constexpr int kRsaKeyBits = 2048;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — fuzz
// init
std::string g_priv_pem;

std::string generate_private_pem() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  EVP_PKEY *pkey = EVP_RSA_gen(static_cast<unsigned int>(kRsaKeyBits));
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  BUF_MEM *buf_mem = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  BIO_get_mem_ptr(bio, &buf_mem);
  std::string pem(buf_mem->data, buf_mem->length);
  BIO_free(bio);
  EVP_PKEY_free(pkey);
  return pem;
}

} // namespace

// Called once before fuzzing starts — generate the test key pair.
// NOLINTNEXTLINE(readability-identifier-naming) — libFuzzer C API
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
  g_priv_pem = generate_private_pem();
  return 0;
}

// Called for each fuzz input.
// NOLINTNEXTLINE(readability-identifier-naming) — libFuzzer C API
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — libFuzzer API
  const std::string message{reinterpret_cast<const char *>(data), size};

  auto fake_ws = std::make_unique<kalshi::FakeWebSocket>();
  fake_ws->enqueue_message(message);

  const kalshi::Auth auth{"fuzz-test-key", g_priv_pem};
  kalshi::WebSocketClient client{
      auth, std::move(fake_ws), "wss://test.example.com/ws",
      /*max_reconnects=*/0,
      /*reconnect_delay=*/std::chrono::milliseconds{0}};

  client.on_orderbook_snapshot([](const kalshi::Orderbook &) {});
  client.on_orderbook_delta(
      [](const std::string &, kalshi::Side, int, kalshi::Quantity) {});
  client.on_fill([](const kalshi::Fill &) {});

  client.run();
  return 0;
}
