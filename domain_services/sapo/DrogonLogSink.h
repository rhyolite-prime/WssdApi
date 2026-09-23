//
// DrogonLogSink.h — routes Sapo engine logs into Drogon/Trantor logging.
//
// The engine never writes to stdout directly once this sink is installed:
// every record keeps its component/execution/node correlation and any
// secret values registered via the provider config stay redacted (the
// Sapo Logger redacts before the sink sees the text).
//

#pragma once

#include <string>

#include <trantor/utils/Logger.h>

#include "observability/Logger.hpp"

namespace wssd_api::sapo_host {

class DrogonLogSink final : public sapo::obs::ILogSink {
  public:
    void write(const sapo::obs::LogRecord &record) override {
        std::string line = "[sapo:" + record.component + "] " + record.message;
        if (!record.execution_id.empty()) {
            line += " exec=";
            line += record.execution_id;
        }
        if (!record.node_id.empty()) {
            line += " node=";
            line += record.node_id;
        }
        if (record.fields.is_object() && !record.fields.empty()) {
            line += " fields=";
            line += record.fields.dump();
        }

        switch (record.level) {
            case sapo::obs::LogLevel::Trace:
            case sapo::obs::LogLevel::Debug:
                LOG_DEBUG << line;
                break;
            case sapo::obs::LogLevel::Info:
                LOG_INFO << line;
                break;
            case sapo::obs::LogLevel::Warn:
                LOG_WARN << line;
                break;
            case sapo::obs::LogLevel::Error:
            case sapo::obs::LogLevel::Off:
                LOG_ERROR << line;
                break;
        }
    }
};

}  // namespace wssd_api::sapo_host
