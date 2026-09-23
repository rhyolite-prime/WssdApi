//
//  Sapo Engine — encoding helpers (base64/base64url/hex/base32).
//
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sapo::util {

    [[nodiscard]] std::string base64Encode(std::string_view input);
    [[nodiscard]] std::optional<std::string> base64Decode(std::string_view input);

    /// URL-safe base64 without padding (JWT segments).
    [[nodiscard]] std::string base64UrlEncode(std::string_view input);
    [[nodiscard]] std::optional<std::string> base64UrlDecode(std::string_view input);

    [[nodiscard]] std::string toHex(std::string_view bytes);
    [[nodiscard]] std::optional<std::string> fromHex(std::string_view hex);

    /// RFC 4648 base32 (uppercase, no padding) — used by TOTP secrets.
    [[nodiscard]] std::string base32Encode(std::string_view input);
    [[nodiscard]] std::optional<std::string> base32Decode(std::string_view input);

    [[nodiscard]] std::string percentEncode(std::string_view input);

} // namespace sapo::util
