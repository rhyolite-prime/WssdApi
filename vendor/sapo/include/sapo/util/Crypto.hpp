//
//  Sapo Engine — self-contained digest / MAC / random helpers.
//
//  Rationale (implementation_plan_2.md T3.3): the `crypto.*` and `auth.*`
//  command families must work without pulling a hard TLS-library dependency
//  into `sapo_core`. SHA-1/SHA-256, HMAC, MD5 and CSPRNG-backed random ids are
//  implemented here (verified against published test vectors in tests/). When
//  OpenSSL is available (`SAPO_WITH_OPENSSL`) the AES-GCM helpers delegate to
//  it; otherwise they report `NOT_IMPLEMENTED` instead of pretending.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sapo::util {

    enum class DigestKind { Sha256, Sha1, Md5, Sha512 };

    /// Returns raw digest bytes.
    [[nodiscard]] std::string digest(DigestKind kind, std::string_view data);
    [[nodiscard]] std::string digestHex(DigestKind kind, std::string_view data);
    /// `algorithm` accepts: sha256, sha1, md5 (case-insensitive).
    [[nodiscard]] std::optional<DigestKind> kindFor(const std::string &algorithm);
    [[nodiscard]] std::optional<std::string> digestHexByName(const std::string &algorithm, std::string_view data);

    /// HMAC over `algorithm` (sha256|sha1|md5|sha512); raw bytes.
    [[nodiscard]] std::optional<std::string> hmac(DigestKind kind, std::string_view key, std::string_view data);
    [[nodiscard]] std::optional<std::string> hmacHex(const std::string &algorithm, std::string_view key,
                                                      std::string_view data);

    /// PBKDF2-HMAC-SHA256 (RFC 6070-style) for password/KDF use in plugins.
    [[nodiscard]] std::string pbkdf2Sha256(std::string_view password, std::string_view salt, uint32_t iterations,
                                           size_t derived_len);

    /// Constant-time compare (avoids timing leaks on token comparison).
    [[nodiscard]] bool timingSafeEquals(std::string_view a, std::string_view b);

    [[nodiscard]] std::string randomBytes(size_t count);
    [[nodiscard]] std::string randomHex(size_t byte_count);
    /// RFC 4122 v4 UUID.
    [[nodiscard]] std::string uuidV4();
    /// Digits-only PIN of the requested length.
    [[nodiscard]] std::string randomPin(size_t digits);

    /// AES-256-GCM when built with OpenSSL; `nullopt` when unavailable.
    [[nodiscard]] std::optional<std::string> aesGcmEncrypt(std::string_view key32, std::string_view plaintext,
                                                           std::string &nonce_out);
    [[nodiscard]] std::optional<std::string> aesGcmDecrypt(std::string_view key32, std::string_view nonce,
                                                           std::string_view ciphertext);

} // namespace sapo::util
