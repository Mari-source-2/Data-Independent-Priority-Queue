#pragma once

// Cryptographic building blocks, all based on OpenSSL (AES-NI or ARMv8 AES where available):
//  - Prf:       a pseudorandom function on 64-bit inputs, AES-128 on one block. Routes keys through the tree.
//  - SecureRng: a cryptographically secure generator, AES-128 in counter mode. Draws Path Heap's leaves.
//  - Aead:      AES-128-GCM with associated data, used to encrypt every outsourced object.
// All integers that enter a cipher are encoded little-endian, so results do not depend on the host.

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "bytes.hpp"

namespace opq {

using Key = std::array<unsigned char, 16>;

inline void openssl_check(int ok, const char* what) {
    if (ok != 1) throw std::runtime_error(std::string("OpenSSL failure: ") + what);
}

inline Key random_key() {
    Key key{};
    openssl_check(RAND_bytes(key.data(), static_cast<int>(key.size())), "RAND_bytes");
    return key;
}

// Loads a 16-byte key from `path`, or creates the file (mode 0600, never overwriting an existing
// file) with a fresh random key if it does not exist.
inline Key load_or_create_key(const std::string& path) {
    Key key{};
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            ssize_t n = ::read(fd, key.data(), key.size());
            ::close(fd);
            if (n != static_cast<ssize_t>(key.size())) throw std::runtime_error("key file " + path + " is too short");
            return key;
        }
        if (errno != ENOENT) throw std::runtime_error("cannot read key file " + path + ": " + std::strerror(errno));
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;  // created concurrently: read it
            throw std::runtime_error("cannot create key file " + path + ": " + std::strerror(errno));
        }
        key = random_key();
        bool ok = ::write(fd, key.data(), key.size()) == static_cast<ssize_t>(key.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!ok) throw std::runtime_error("cannot write key file " + path);
        return key;
    }
    throw std::runtime_error("cannot open key file " + path);
}

// One AES-128 block encryption under `key` of the little-endian encoding of (a, b).
inline Key aes_block(const Key& key, uint64_t a, uint64_t b) {
    unsigned char in[16];
    store_le64(in, a);
    store_le64(in + 8, b);
    Key out{};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    int len = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key.data(), nullptr) == 1 &&
              EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && EVP_EncryptUpdate(ctx, out.data(), &len, in, 16) == 1;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(in, sizeof(in));
    if (!ok) throw std::runtime_error("OpenSSL failure: AES block");
    return out;
}

// Derives an independent 128-bit key from a master key: AES_master(label || 0^64).
inline Key derive_key(const Key& master, uint64_t label) { return aes_block(master, label, 0); }

class Prf {
   public:
    explicit Prf(const Key& key) {
        ctx_ = EVP_CIPHER_CTX_new();
        if (ctx_ == nullptr) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
        openssl_check(EVP_EncryptInit_ex(ctx_, EVP_aes_128_ecb(), nullptr, key.data(), nullptr), "PRF init");
        openssl_check(EVP_CIPHER_CTX_set_padding(ctx_, 0), "PRF padding");
    }
    Prf(const Prf&) = delete;
    Prf& operator=(const Prf&) = delete;
    ~Prf() { EVP_CIPHER_CTX_free(ctx_); }

    // Returns the first 64 bits (little-endian) of AES_K(x || 0^64).
    uint64_t eval(uint64_t x) const {
        uint64_t y;
        eval_many(&x, &y, 1);
        return y;
    }

    // out[i] = eval(in[i]) for i < count, with one cipher call per chunk of inputs.
    void eval_many(const uint64_t* in, uint64_t* out, size_t count) const {
        constexpr size_t kChunk = 256;
        unsigned char buf[16 * kChunk];
        for (size_t done = 0; done < count; done += kChunk) {
            size_t m = std::min(kChunk, count - done);
            std::memset(buf, 0, 16 * m);
            for (size_t i = 0; i < m; i++) store_le64(buf + 16 * i, in[done + i]);
            int len = 0;
            openssl_check(EVP_EncryptUpdate(ctx_, buf, &len, buf, static_cast<int>(16 * m)), "PRF eval");
            for (size_t i = 0; i < m; i++) out[done + i] = load_le64(buf + 16 * i);
        }
        evals_ += count;
    }

    uint64_t evals() const { return evals_; }

   private:
    EVP_CIPHER_CTX* ctx_ = nullptr;
    mutable uint64_t evals_ = 0;
};

// AES-128-CTR keystream as a source of uniform 64-bit words. With a random key (the default) the
// output is unpredictable to anyone who sees earlier outputs. Seeding derives the key from the seed,
// which makes runs reproducible but INSECURE (the seed determines everything); tests only.
class SecureRng {
   public:
    SecureRng() : SecureRng(random_key()) {}
    static SecureRng insecure_from_seed(uint64_t seed) { return SecureRng(aes_block(Key{}, seed, 0x5eed)); }

    SecureRng(SecureRng&& o) noexcept : ctx_(o.ctx_), buf_(o.buf_), pos_(o.pos_) { o.ctx_ = nullptr; }
    SecureRng(const SecureRng&) = delete;
    SecureRng& operator=(const SecureRng&) = delete;
    ~SecureRng() { EVP_CIPHER_CTX_free(ctx_); }

    uint64_t next() {
        if (pos_ == buf_.size()) refill();
        uint64_t v = load_le64(buf_.data() + pos_);
        pos_ += 8;
        return v;
    }

    // Uniform in [0, bound), bound >= 1, by rejection sampling (no rejection for powers of two).
    uint64_t below(uint64_t bound) {
        if (bound == 0) throw std::invalid_argument("SecureRng::below(0)");
        if ((bound & (bound - 1)) == 0) return next() & (bound - 1);
        uint64_t limit = ~0ULL - (~0ULL % bound);  // accept values < limit, a multiple of bound
        uint64_t v;
        do v = next();
        while (v >= limit);
        return v % bound;
    }

   private:
    explicit SecureRng(const Key& key) {
        ctx_ = EVP_CIPHER_CTX_new();
        if (ctx_ == nullptr) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
        unsigned char iv[16] = {0};
        openssl_check(EVP_EncryptInit_ex(ctx_, EVP_aes_128_ctr(), nullptr, key.data(), iv), "RNG init");
        pos_ = buf_.size();
    }
    void refill() {
        std::array<unsigned char, 4096> zero{};
        int len = 0;
        openssl_check(EVP_EncryptUpdate(ctx_, buf_.data(), &len, zero.data(), static_cast<int>(zero.size())),
                      "RNG refill");
        pos_ = 0;
    }
    EVP_CIPHER_CTX* ctx_ = nullptr;
    std::array<unsigned char, 4096> buf_{};
    size_t pos_ = 0;
};

// AES-128-GCM. A sealed object is IV (12 bytes) || tag (16 bytes) || encrypted payload, encrypted
// and decrypted in place. Every IV is drawn uniformly at random (from a buffer filled by RAND_bytes),
// so IVs collide only with negligible probability, also across sessions under a persistent key.
//
// The associated data binds an object to its context: a random 16-byte instance identifier chosen
// when the tables are created (so objects of another session, another queue or another scheme are
// rejected), the table, the object index and the version.
class Aead {
   public:
    static constexpr size_t kIvSize = 12;
    static constexpr size_t kTagSize = 16;
    static constexpr size_t kOverhead = kIvSize + kTagSize;
    using Ad = std::array<unsigned char, 16 + 3 * 8>;

    explicit Aead(const Key& key) {
        enc_ = EVP_CIPHER_CTX_new();
        dec_ = EVP_CIPHER_CTX_new();
        if (enc_ == nullptr || dec_ == nullptr) {
            EVP_CIPHER_CTX_free(enc_);
            EVP_CIPHER_CTX_free(dec_);
            throw std::runtime_error("EVP_CIPHER_CTX_new failed");
        }
        openssl_check(EVP_EncryptInit_ex(enc_, EVP_aes_128_gcm(), nullptr, nullptr, nullptr), "GCM enc init");
        openssl_check(EVP_CIPHER_CTX_ctrl(enc_, EVP_CTRL_AEAD_SET_IVLEN, kIvSize, nullptr), "GCM ivlen");
        openssl_check(EVP_EncryptInit_ex(enc_, nullptr, nullptr, key.data(), nullptr), "GCM enc key");
        openssl_check(EVP_DecryptInit_ex(dec_, EVP_aes_128_gcm(), nullptr, nullptr, nullptr), "GCM dec init");
        openssl_check(EVP_CIPHER_CTX_ctrl(dec_, EVP_CTRL_AEAD_SET_IVLEN, kIvSize, nullptr), "GCM ivlen");
        openssl_check(EVP_DecryptInit_ex(dec_, nullptr, nullptr, key.data(), nullptr), "GCM dec key");
    }
    Aead(const Aead&) = delete;
    Aead& operator=(const Aead&) = delete;
    ~Aead() {
        EVP_CIPHER_CTX_free(enc_);
        EVP_CIPHER_CTX_free(dec_);
    }

    static Ad make_ad(const Key& instance, uint64_t table, uint64_t id, uint64_t version) {
        Ad ad{};
        std::memcpy(ad.data(), instance.data(), 16);
        store_le64(ad.data() + 16, table);
        store_le64(ad.data() + 24, id);
        store_le64(ad.data() + 32, version);
        return ad;
    }

    // `obj` holds kOverhead bytes of space followed by the plaintext; on return it holds the sealed object.
    void seal(std::string& obj, const Ad& ad) {
        if (obj.size() < kOverhead) throw std::logic_error("object too short to seal");
        auto* p = reinterpret_cast<unsigned char*>(obj.data());
        next_iv(p);
        openssl_check(EVP_EncryptInit_ex(enc_, nullptr, nullptr, nullptr, p), "GCM set iv");
        int len = 0;
        openssl_check(EVP_EncryptUpdate(enc_, nullptr, &len, ad.data(), static_cast<int>(ad.size())), "GCM ad");
        int body = static_cast<int>(obj.size() - kOverhead);
        openssl_check(EVP_EncryptUpdate(enc_, p + kOverhead, &len, p + kOverhead, body), "GCM encrypt");
        int fin = 0;
        openssl_check(EVP_EncryptFinal_ex(enc_, p + kOverhead + len, &fin), "GCM final");
        if (len + fin != body) throw std::runtime_error("GCM length mismatch");
        openssl_check(EVP_CIPHER_CTX_ctrl(enc_, EVP_CTRL_AEAD_GET_TAG, kTagSize, p + kIvSize), "GCM tag");
    }

    // Decrypts a sealed object in place; afterwards the plaintext starts at obj.data() + kOverhead.
    // Throws std::runtime_error if the object or its associated data was modified.
    void open(std::string& obj, const Ad& ad) {
        if (obj.size() < kOverhead) throw std::runtime_error("ciphertext too short");
        auto* p = reinterpret_cast<unsigned char*>(obj.data());
        openssl_check(EVP_DecryptInit_ex(dec_, nullptr, nullptr, nullptr, p), "GCM set iv");
        int len = 0;
        openssl_check(EVP_DecryptUpdate(dec_, nullptr, &len, ad.data(), static_cast<int>(ad.size())), "GCM ad");
        int body = static_cast<int>(obj.size() - kOverhead);
        openssl_check(EVP_DecryptUpdate(dec_, p + kOverhead, &len, p + kOverhead, body), "GCM decrypt");
        openssl_check(EVP_CIPHER_CTX_ctrl(dec_, EVP_CTRL_AEAD_SET_TAG, kTagSize, p + kIvSize), "GCM set tag");
        int fin = 0;
        if (EVP_DecryptFinal_ex(dec_, p + kOverhead + len, &fin) != 1) {
            OPENSSL_cleanse(p + kOverhead, static_cast<size_t>(body));
            throw std::runtime_error("authentication failed: ciphertext, location or version does not match");
        }
    }

   private:
    void next_iv(unsigned char* out) {
        if (iv_pos_ + kIvSize > iv_pool_.size()) {
            openssl_check(RAND_bytes(iv_pool_.data(), static_cast<int>(iv_pool_.size())), "RAND_bytes");
            iv_pos_ = 0;
        }
        std::memcpy(out, iv_pool_.data() + iv_pos_, kIvSize);
        iv_pos_ += kIvSize;
    }

    EVP_CIPHER_CTX* enc_ = nullptr;
    EVP_CIPHER_CTX* dec_ = nullptr;
    std::array<unsigned char, 4092> iv_pool_{};  // 341 IVs
    size_t iv_pos_ = iv_pool_.size();
};

}  // namespace opq
