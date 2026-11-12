#pragma once
// Kalshi request signing (REST and the WebSocket handshake): KALSHI-ACCESS-KEY, -TIMESTAMP (ms)
// and -SIGNATURE = base64(sign(timestamp + METHOD + path)), path without the query string.
// Ed25519 and RSA-PSS keys both work; the type is read from the PEM.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

typedef struct evp_pkey_st EVP_PKEY;

namespace mt {

class KalshiSigner {
public:
    // Returns nullopt and sets `error` if a file is missing or the key is not Ed25519/RSA.
    static std::optional<KalshiSigner> load(const std::string& key_id_file, const std::string& private_key_file,
                                            std::string* error);
    // For tests: key id and PEM from memory.
    static std::optional<KalshiSigner> from_pem(std::string key_id, const std::string& pem, std::string* error);

    // base64 signature of `message`.
    std::string sign(std::string_view message) const;

    // The three KALSHI-ACCESS-* headers for one request.
    std::map<std::string, std::string> headers(int64_t timestamp_ms, std::string_view method,
                                               std::string_view path) const;

    const std::string& key_id() const { return key_id_; }
    bool is_ed25519() const { return is_ed25519_; }

private:
    struct KeyDeleter {
        void operator()(EVP_PKEY* key) const;
    };

    KalshiSigner(std::string key_id, EVP_PKEY* key, bool is_ed25519);

    std::string key_id_;
    std::shared_ptr<EVP_PKEY> key_;  // shared so copies are cheap
    bool is_ed25519_ = true;
};

std::string base64_encode(const unsigned char* data, size_t length);

// Expands a leading "~/" using $HOME.
std::string expand_home(const std::string& path);

}  // namespace mt
