#include "io/auth.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mt {

namespace {

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

// RAII owner for an EVP_MD_CTX.
struct DigestContext {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    ~DigestContext() { EVP_MD_CTX_free(context); }
};

}  // namespace

void KalshiSigner::KeyDeleter::operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }

KalshiSigner::KalshiSigner(std::string key_id, EVP_PKEY* key, bool is_ed25519)
    : key_id_(std::move(key_id)), key_(key, KeyDeleter{}), is_ed25519_(is_ed25519) {}

std::optional<KalshiSigner> KalshiSigner::from_pem(std::string key_id, const std::string& pem, std::string* error) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key == nullptr) {
        if (error) *error = "could not parse the PEM private key";
        return std::nullopt;
    }

    const int type = EVP_PKEY_get_base_id(key);
    if (type != EVP_PKEY_ED25519 && type != EVP_PKEY_RSA) {
        EVP_PKEY_free(key);
        if (error) *error = "unsupported key type (expected Ed25519 or RSA)";
        return std::nullopt;
    }
    return KalshiSigner(std::move(key_id), key, type == EVP_PKEY_ED25519);
}

std::optional<KalshiSigner> KalshiSigner::load(const std::string& key_id_file, const std::string& private_key_file,
                                               std::string* error) {
    const auto key_id = read_file(expand_home(key_id_file));
    if (!key_id) {
        if (error) *error = "missing key id file " + key_id_file;
        return std::nullopt;
    }
    const auto pem = read_file(expand_home(private_key_file));
    if (!pem) {
        if (error) *error = "missing private key file " + private_key_file;
        return std::nullopt;
    }
    return from_pem(trim(*key_id), *pem, error);
}

std::string KalshiSigner::sign(std::string_view message) const {
    DigestContext digest;
    EVP_PKEY_CTX* key_context = nullptr;  // owned by `digest`

    // Ed25519 is pure EdDSA: no digest, one-shot sign. RSA uses PSS with SHA-256,
    // MGF1(SHA-256) and salt length = digest length, as Kalshi expects.
    const EVP_MD* hash = is_ed25519_ ? nullptr : EVP_sha256();
    if (EVP_DigestSignInit(digest.context, &key_context, hash, nullptr, key_.get()) != 1) {
        throw std::runtime_error("EVP_DigestSignInit failed");
    }
    if (!is_ed25519_) {
        EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST);
        EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, EVP_sha256());
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(message.data());
    size_t signature_length = 0;
    if (EVP_DigestSign(digest.context, nullptr, &signature_length, bytes, message.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign (size) failed");
    }
    std::vector<unsigned char> signature(signature_length);
    if (EVP_DigestSign(digest.context, signature.data(), &signature_length, bytes, message.size()) != 1) {
        throw std::runtime_error("EVP_DigestSign failed");
    }
    return base64_encode(signature.data(), signature_length);
}

std::map<std::string, std::string> KalshiSigner::headers(int64_t timestamp_ms, std::string_view method,
                                                         std::string_view path) const {
    const std::string timestamp = std::to_string(timestamp_ms);
    std::string message = timestamp;
    message += method;
    message += path;
    return {
        {"KALSHI-ACCESS-KEY", key_id_},
        {"KALSHI-ACCESS-TIMESTAMP", timestamp},
        {"KALSHI-ACCESS-SIGNATURE", sign(message)},
    };
}

std::string base64_encode(const unsigned char* data, size_t length) {
    std::string encoded(4 * ((length + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), data,
                                        static_cast<int>(length));
    encoded.resize(static_cast<size_t>(written));
    return encoded;
}

std::string expand_home(const std::string& path) {
    if (path.rfind("~/", 0) != 0) return path;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + path.substr(1);
}

}  // namespace mt
