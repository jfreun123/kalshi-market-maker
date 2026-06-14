#include "auth.hpp"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace kalshi {

Auth::Auth(std::string api_key, std::string pem_private_key)
    : api_key_(std::move(api_key)),
      pem_private_key_(std::move(pem_private_key)) {}

namespace {

std::string base64_encode(const unsigned char *data, size_t len) {
  BIO *b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO *mem_bio = BIO_new(BIO_s_mem());
  BIO *chain = BIO_push(b64, mem_bio);

  BIO_write(chain, data, static_cast<int>(len));
  BIO_flush(chain);

  BUF_MEM *buf_mem = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast) — OpenSSL C macro
  BIO_get_mem_ptr(chain, &buf_mem);
  std::string result(buf_mem->data, buf_mem->length);

  BIO_free_all(chain);
  return result;
}

std::string rsa_sha256_sign(const std::string &pem_key,
                            const std::string &message) {
  BIO *bio = BIO_new_mem_buf(pem_key.data(), static_cast<int>(pem_key.size()));
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);

  if (pkey == nullptr) {
    throw std::runtime_error("auth: failed to load RSA private key from PEM");
  }

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();

  if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    throw std::runtime_error("auth: EVP_DigestSignInit failed");
  }
  if (EVP_DigestSignUpdate(ctx, message.data(), message.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    throw std::runtime_error("auth: EVP_DigestSignUpdate failed");
  }

  size_t sig_len = 0;
  EVP_DigestSignFinal(ctx, nullptr, &sig_len);
  std::vector<unsigned char> sig(sig_len);
  EVP_DigestSignFinal(ctx, sig.data(), &sig_len);

  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);

  return base64_encode(sig.data(), sig_len);
}

} // namespace

std::map<std::string, std::string>
Auth::sign(std::string_view method, std::string_view path,
           std::optional<long long> timestamp_ms) const {
  long long timestamp_val = timestamp_ms.value_or(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  std::string ts_str = std::to_string(timestamp_val);
  std::string message = ts_str + std::string(method) + std::string(path);
  std::string signature = rsa_sha256_sign(pem_private_key_, message);

  return {
      {"Kalshi-Access-Key", api_key_},
      {"Kalshi-Access-Timestamp", ts_str},
      {"Kalshi-Access-Signature", std::move(signature)},
  };
}

} // namespace kalshi
