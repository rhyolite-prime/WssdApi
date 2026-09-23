//
// ProviderAdapters.cc
//

#include "ProviderAdapters.h"

#include <cctype>

#include <nlohmann/json.hpp>

#include "runtime/VirtualMachine.hpp"

namespace wssd_api::sapo_host::adapters {
namespace {

std::string firstNonEmpty(std::initializer_list<std::string> values) {
    for (const auto &value : values) {
        if (!value.empty()) {
            return value;
        }
    }
    return "";
}

std::string stringField(const nlohmann::json &object, const char *key) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return "";
}

/// Message carried by a suspend prompt ({message, interaction_type, ...}).
std::string promptMessage(const nlohmann::json &prompt) {
    if (prompt.is_string()) {
        return prompt.get<std::string>();
    }
    return stringField(prompt, "message");
}

/// Message carried by a terminate/completed payload. Blueprints disagree on
/// the key, so every conventional spelling is accepted.
std::string outputMessage(const nlohmann::json &output) {
    if (output.is_string()) {
        return output.get<std::string>();
    }
    if (!output.is_object()) {
        return "";
    }
    return firstNonEmpty({stringField(output, "message"), stringField(output, "msg"),
                          stringField(output, "text"), stringField(output, "display")});
}

/// Appends a numbered menu when the prompt carries structured options but
/// the message text does not already list them.
std::string withOptions(const std::string &message, const nlohmann::json &prompt) {
    if (!prompt.is_object() || !prompt.contains("options")) {
        return message;
    }
    const auto &options = prompt["options"];
    if (!options.is_array() || options.empty()) {
        return message;
    }
    if (message.find("1.") != std::string::npos || message.find("1)") != std::string::npos) {
        return message;
    }
    std::string out = message;
    if (!out.empty() && out.back() != '\n') {
        out += '\n';
    }
    int index = 1;
    for (const auto &option : options) {
        std::string label;
        if (option.is_string()) {
            label = option.get<std::string>();
        } else if (option.is_object()) {
            label = firstNonEmpty({stringField(option, "label"), stringField(option, "display"),
                                   stringField(option, "text"), stringField(option, "value"),
                                   stringField(option, "name")});
        }
        if (label.empty()) {
            continue;
        }
        out += std::to_string(index++) + ". " + label + "\n";
    }
    return out;
}

std::string firstLine(const std::string &message, std::size_t maxLength) {
    std::string line = message.substr(0, message.find_first_of("\r\n"));
    line = trimCopy(line);
    if (line.size() > maxLength) {
        line = trimCopy(line.substr(0, maxLength));
    }
    return line;
}

}  // namespace

UssdInteraction normalizeNalo(const dto::NaloUssdSessionRequestDto &dto, const Json::Value &raw) {
    UssdInteraction interaction;
    interaction.provider = UssdProvider::Nalo;
    interaction.networkSessionId = dto.getSession();
    interaction.msisdn = dto.getMsisdn();
    interaction.userInput = dto.getUserData();
    interaction.serviceKey = dto.getUserId();
    interaction.dialCode = extractDialCode(dto.getUserData());
    interaction.network = dto.getNetwork();
    interaction.raw = raw;
    return interaction;
}

UssdInteraction normalizeHubtel(const dto::HubtelUssdSessionRequestDto &dto, const Json::Value &raw) {
    UssdInteraction interaction;
    interaction.provider = UssdProvider::Hubtel;
    interaction.networkSessionId = dto.getSessionId();
    interaction.msisdn = dto.getMobile();
    interaction.userInput = dto.getMessage();
    interaction.serviceKey = dto.getServiceCode();
    interaction.network = dto.getOperator();
    interaction.sequence = dto.getSequence();
    interaction.clientState = dto.getClientState();
    interaction.isStart = equalsIgnoreCase(dto.getType(), "initiation");
    interaction.isRelease = equalsIgnoreCase(dto.getType(), "release");
    if (interaction.isStart) {
        interaction.dialCode = extractDialCode(dto.getMessage());
    }
    interaction.raw = raw;
    return interaction;
}

Json::Value renderNalo(const dto::NaloUssdSessionRequestDto &dto, const UssdResult &result) {
    dto::NaloUssdSessionResponse response;
    response.USERID = dto.getUserId();
    response.MSISDN = dto.getMsisdn();
    response.SESSIONID = dto.getSession();
    response.USERDATA = dto.getUserData();
    response.MSGTYPE = result.cont;
    response.MSG = result.message;
    return response.toJson();
}

Json::Value renderHubtel(const dto::HubtelUssdSessionRequestDto &dto, const UssdResult &result) {
    dto::HubtelUssdSessionResponse response;
    response.SessionId = dto.getSessionId();
    response.Type = result.cont ? "Response" : "Release";
    response.Message = result.message;
    response.Label = result.label.empty() ? firstLine(result.message, 32) : result.label;
    if (result.cont) {
        response.ClientState = result.clientState.empty() ? dto.getClientState() : result.clientState;
    } else {
        response.ClientState = "End";
    }
    response.DataType = result.dataType.empty() ? "input" : result.dataType;
    response.FieldType = result.fieldType.empty() ? "text" : result.fieldType;
    return response.toJson();
}

UssdResult renderOutcome(const ::sapo::runtime::ExecutionOutcome &outcome) {
    UssdResult result;
    result.sapoSessionId = outcome.session_id;
    result.workflowId = outcome.workflow_id;
    result.status = outcome.status;
    result.elapsedMs = outcome.elapsed_ms;
    result.nodeVisits = outcome.node_visits;

    const bool cont = (outcome.status == "awaiting_input" || outcome.status == "suspended");
    result.cont = cont;
    result.clientState = outcome.cursor;

    if (outcome.prompt.is_object()) {
        const std::string dataType = stringField(outcome.prompt, "data_type");
        if (!dataType.empty()) {
            result.dataType = dataType;
        }
        const std::string fieldType = stringField(outcome.prompt, "field_type");
        if (!fieldType.empty()) {
            result.fieldType = fieldType;
        }
        if (result.dataType == "input" && result.fieldType == "text") {
            const std::string interaction = outcome.prompt.value("interaction_type", "input");
            if (interaction == "menu") {
                result.dataType = "menu";
            } else if (interaction == "pin" || interaction == "password") {
                result.fieldType = "password";
            } else if (interaction == "display") {
                result.dataType = "display";
            }
        }
    }

    if (outcome.status == "failed" || outcome.status == "cancelled") {
        result.cont = false;
        result.clientState = "End";
        result.message = (outcome.status == "cancelled")
                             ? "Session ended."
                             : "Sorry, the service is temporarily unavailable. Please try again "
                               "later.";
    } else if (cont) {
        result.message = firstNonEmpty(
            {promptMessage(outcome.prompt), outputMessage(outcome.output)});
        if (result.message.empty()) {
            result.message = "Please enter your response:";
        }
        result.message = withOptions(result.message, outcome.prompt);
    } else {
        result.clientState = "End";
        result.message =
            firstNonEmpty({outputMessage(outcome.output), promptMessage(outcome.prompt)});
        if (result.message.empty()) {
            result.message = "Thank you for using our service.";
        }
    }

    result.label = firstLine(result.message, 32);
    return result;
}

bool isDialCode(const std::string &text) {
    const std::string trimmed = trimCopy(text);
    return trimmed.size() >= 3 && trimmed.front() == '*' && trimmed.back() == '#';
}

std::string extractDialCode(const std::string &text) {
    return isDialCode(text) ? trimCopy(text) : std::string();
}

std::string trimCopy(const std::string &text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool equalsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string maskMsisdn(const std::string &msisdn) {
    if (msisdn.size() <= 3) {
        return "***";
    }
    return "***" + msisdn.substr(msisdn.size() - 3);
}

}  // namespace wssd_api::sapo_host::adapters
