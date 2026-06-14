#include "auth.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <string>
#include <vector>

// ---- Test helpers ----

namespace {

constexpr int kRsaKeyBits = 2048;
constexpr long long kFixedTimestampMs =
    1000000000LL; // 2001-09-09, stable test anchor

// Generates a fresh RSA key pair. Returns {priv_pem, pub_pem}.
std::pair<std::string, std::string> generate_test_rsa_keypair() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C API
  EVP_PKEY *pkey = EVP_RSA_gen(static_cast<unsigned int>(kRsaKeyBits));

  auto bio_to_string = [](BIO *bio) {
    BUF_MEM *buf_mem = nullptr;
    BIO_get_mem_ptr(bio,
                    &buf_mem); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
    std::string result(buf_mem->data, buf_mem->length);
    BIO_free(bio);
    return result;
  };

  BIO *priv_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr,
                           nullptr);
  std::string priv_pem = bio_to_string(priv_bio);

  BIO *pub_bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(pub_bio, pkey);
  std::string pub_pem = bio_to_string(pub_bio);

  EVP_PKEY_free(pkey);
  return {priv_pem, pub_pem};
}

// Base64-decodes a string_view (no newlines).
std::vector<unsigned char> base64_decode(std::string_view encoded) {
  BIO *b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO *mem_bio =
      BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
  BIO *chain = BIO_push(b64, mem_bio);

  std::vector<unsigned char> decoded(encoded.size()); // upper bound
  int decoded_len =
      BIO_read(chain, decoded.data(), static_cast<int>(decoded.size()));
  BIO_free_all(chain);

  decoded.resize(static_cast<size_t>(std::max(0, decoded_len)));
  return decoded;
}

struct VerifyArgs {
  std::string_view pub_pem;
  std::string_view message;
  std::string_view base64_sig;
};

// Verifies an RSA-SHA256 signature against a public key PEM.
bool verify_rsa_sha256(
    VerifyArgs args) { // NOLINT(performance-unnecessary-value-param)
                       // — small struct
  BIO *bio = BIO_new_mem_buf(args.pub_pem.data(),
                             static_cast<int>(args.pub_pem.size()));
  EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (pkey == nullptr) {
    return false;
  }

  auto sig_bytes = base64_decode(args.base64_sig);

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool verified =
      EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
      EVP_DigestVerifyUpdate(ctx, args.message.data(), args.message.size()) ==
          1 &&
      EVP_DigestVerifyFinal(ctx, sig_bytes.data(), sig_bytes.size()) == 1;

  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return verified;
}

} // namespace

// ---- Test fixture ----

class AuthTest : public ::testing::Test {
  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes):
  // GTest requires protected members to be accessible in TEST_F bodies.
protected:
  void SetUp() override {
    auto [priv, pub] = generate_test_rsa_keypair();
    priv_pem_ = std::move(priv);
    pub_pem_ = std::move(pub);
  }

  std::string
      priv_pem_; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
                 // — GTest fixture
  std::string
      pub_pem_; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
                // — GTest fixture
};

// ---- Tests ----

TEST_F(AuthTest, HeadersContainRequiredKeys) {
  kalshi::Auth auth{"test-api-key", priv_pem_};
  auto headers = auth.sign("GET", "/trade-api/v2/markets");
  EXPECT_GT(headers.count("Kalshi-Access-Key"), 0U);
  EXPECT_GT(headers.count("Kalshi-Access-Timestamp"), 0U);
  EXPECT_GT(headers.count("Kalshi-Access-Signature"), 0U);
}

TEST_F(AuthTest, ApiKeyMatchesInput) {
  kalshi::Auth auth{"my-api-key-123", priv_pem_};
  auto headers = auth.sign("GET", "/trade-api/v2/markets");
  EXPECT_EQ(headers.at("Kalshi-Access-Key"), "my-api-key-123");
}

TEST_F(AuthTest, TimestampIsRecentUnixMilliseconds) {
  kalshi::Auth auth{"key", priv_pem_};
  auto before_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  auto headers = auth.sign("GET", "/");
  auto after_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  long long timestamp_val = std::stoll(headers.at("Kalshi-Access-Timestamp"));
  EXPECT_GE(timestamp_val, before_ms);
  EXPECT_LE(timestamp_val, after_ms);
}

TEST_F(AuthTest, SignatureVerifiesWithPublicKey) {
  kalshi::Auth auth{"key", priv_pem_};
  const std::string method = "POST";
  const std::string path = "/trade-api/v2/orders";
  auto headers = auth.sign(method, path);

  std::string timestamp = headers.at("Kalshi-Access-Timestamp");
  std::string message = timestamp + method + path;
  std::string signature = headers.at("Kalshi-Access-Signature");

  EXPECT_TRUE(verify_rsa_sha256({pub_pem_, message, signature}));
}

TEST_F(AuthTest, DifferentMethodsProduceDifferentSignatures) {
  kalshi::Auth auth{"key", priv_pem_};
  const std::string path = "/trade-api/v2/markets";
  auto headers_get = auth.sign("GET", path, kFixedTimestampMs);
  auto headers_post = auth.sign("POST", path, kFixedTimestampMs);
  EXPECT_NE(headers_get.at("Kalshi-Access-Signature"),
            headers_post.at("Kalshi-Access-Signature"));
}

TEST_F(AuthTest, DifferentPathsProduceDifferentSignatures) {
  kalshi::Auth auth{"key", priv_pem_};
  auto headers_markets =
      auth.sign("GET", "/trade-api/v2/markets", kFixedTimestampMs);
  auto headers_orders =
      auth.sign("GET", "/trade-api/v2/orders", kFixedTimestampMs);
  EXPECT_NE(headers_markets.at("Kalshi-Access-Signature"),
            headers_orders.at("Kalshi-Access-Signature"));
}

TEST_F(AuthTest, InvalidKeyThrows) {
  kalshi::Auth auth{"key", "not-a-valid-pem"};
  // [[nodiscard]] result is intentionally discarded — we're testing the throw.
  EXPECT_THROW(
      {
        [[maybe_unused]] auto result = auth.sign("GET", "/");
      }, // NOLINT(bugprone-unused-return-value)
      std::runtime_error);
}
