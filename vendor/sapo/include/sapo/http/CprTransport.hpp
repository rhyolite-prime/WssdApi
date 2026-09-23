//
//  libcurl/cpr-backed HTTP transport (optional build).
//
//  Enabled with -DSAPO_ENABLE_CPR=ON; when disabled `NullTransport` answers
//  instead and every `http.*` command fails with a clear NOT_IMPLEMENTED
//  message — the engine never silently "succeeds" an HTTP call it did not make.
//
#pragma once

#include "http/IHttpTransport.hpp"

namespace sapo::http {

#if defined(SAPO_ENABLE_CPR)
    /// Real transport built on libcurl via the cpr wrapper.
    class CprTransport final : public IHttpTransport {
    public:
        CprTransport() = default;
        ~CprTransport() override = default;

        [[nodiscard]] Response send(const Request &request) override;
        [[nodiscard]] std::string name() const override { return "cpr"; }
    };

    [[nodiscard]] TransportPtr makeCprTransport();
#endif

} // namespace sapo::http
