#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <sys/stat.h>

#include "App.h"

#include "rinha/classifier.hpp"
#include "rinha/request.hpp"
#include "rinha/types.hpp"

namespace fs = std::filesystem;

namespace {

struct ListenerConfig {
    bool use_unix_socket = false;
    std::string host = "0.0.0.0";
    int port = 3000;
    std::string unix_socket_path;
};

struct RequestContext {
    bool aborted = false;
    std::string body;
};

struct AppState {
    explicit AppState(rinha::Classifier classifier_in) : classifier(std::move(classifier_in)) {}

    rinha::Classifier classifier;
};

std::string env_or_default(const char* key, std::string default_value) {
    if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    return default_value;
}

std::optional<std::string> optional_env(const char* key) {
    if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    return std::nullopt;
}

bool parse_bind_addr(const std::string& bind_addr, ListenerConfig& config, std::string& error) {
    const std::size_t separator = bind_addr.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator == bind_addr.size() - 1) {
        error = "bind inválido " + bind_addr;
        return false;
    }

    config.host = bind_addr.substr(0, separator);
    try {
        config.port = std::stoi(bind_addr.substr(separator + 1));
    } catch (...) {
        error = "bind inválido " + bind_addr;
        return false;
    }

    if (config.port <= 0 || config.port > 65535) {
        error = "bind inválido " + bind_addr;
        return false;
    }

    return true;
}

bool load_references_from_env(rinha::ReferenceSet& refs, std::string& error) {
    const std::string references_path = env_or_default("REFERENCES_PATH", "resources/references.json.gz");
    const std::optional<std::string> references_bin_path = optional_env("REFERENCES_BIN_PATH");
    const std::optional<std::string> labels_bin_path = optional_env("LABELS_BIN_PATH");

    if (references_bin_path.has_value() != labels_bin_path.has_value()) {
        error = "REFERENCES_BIN_PATH e LABELS_BIN_PATH devem ser informados juntos";
        return false;
    }

    if (references_bin_path) {
        return rinha::ReferenceSet::load_binary(*references_bin_path, *labels_bin_path, refs, error);
    }

    return rinha::ReferenceSet::load_gzip_json(references_path, refs, error);
}

bool listener_config_from_env(ListenerConfig& config, std::string& error) {
    const std::optional<std::string> unix_socket_path = optional_env("UNIX_SOCKET_PATH");
    if (unix_socket_path) {
        config.use_unix_socket = true;
        config.unix_socket_path = *unix_socket_path;
        return true;
    }

    return parse_bind_addr(env_or_default("BIND_ADDR", "0.0.0.0:3000"), config, error);
}

std::string_view classification_json(const rinha::Classification& classification) {
    constexpr std::string_view json_0 = "{\"approved\":true,\"fraud_score\":0.0}";
    constexpr std::string_view json_1 = "{\"approved\":true,\"fraud_score\":0.2}";
    constexpr std::string_view json_2 = "{\"approved\":true,\"fraud_score\":0.4}";
    constexpr std::string_view json_3 = "{\"approved\":false,\"fraud_score\":0.6}";
    constexpr std::string_view json_4 = "{\"approved\":false,\"fraud_score\":0.8}";
    constexpr std::string_view json_5 = "{\"approved\":false,\"fraud_score\":1.0}";

    const int bucket =
        std::clamp(static_cast<int>(std::floor((classification.fraud_score * 5.0f) + 0.5f)), 0, 5);

    switch (bucket) {
        case 0:
            return json_0;
        case 1:
            return json_1;
        case 2:
            return json_2;
        case 3:
            return json_3;
        case 4:
            return json_4;
        case 5:
            return json_5;
        default:
            return json_0;
    }
}

bool prepare_unix_socket(const std::string& path, std::string& error) {
    const fs::path socket_path(path);
    if (socket_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(socket_path.parent_path(), ec);
        if (ec) {
            error = "falha ao criar diretório do socket " + path + ": " + ec.message();
            return false;
        }
    }

    std::error_code ec;
    if (fs::exists(socket_path, ec) && !ec) {
        fs::remove(socket_path, ec);
        if (ec) {
            error = "falha ao remover socket antigo " + path + ": " + ec.message();
            return false;
        }
    }

    return true;
}

int run_server(const ListenerConfig& config, const std::shared_ptr<AppState>& state) {
    bool listening = false;

    auto app = uWS::App();
    app.get("/ready", [](auto* res, auto*) {
        res->writeStatus("204 No Content");
        res->endWithoutBody();
    });

    app.post("/fraud-score", [state](auto* res, auto*) {
        auto context = std::make_shared<RequestContext>();
        res->onAborted([context]() {
            context->aborted = true;
        });
        res->onData([res, context, state](std::string_view chunk, bool is_last) {
            if (context->aborted) {
                return;
            }

            context->body.append(chunk.data(), chunk.size());
            if (!is_last) {
                return;
            }

            rinha::Payload payload;
            std::string error;
            if (!rinha::parse_payload(context->body, payload, error)) {
                res->writeStatus("400 Bad Request");
                res->endWithoutBody();
                return;
            }

            rinha::Classification classification{};
            if (!state->classifier.classify(payload, classification, error)) {
                classification = {};
            }

            const std::string_view body = classification_json(classification);
            res->cork([res, body]() {
                res->writeHeader("Content-Type", "application/json");
                res->end(body);
            });
        });
    });

    if (config.use_unix_socket) {
        app.listen(
            [&listening, &config](auto* listen_socket) {
                listening = listen_socket != nullptr;
                if (listening) {
                    chmod(config.unix_socket_path.c_str(), 0777);
                }
            },
            config.unix_socket_path
        );
    } else {
        app.listen(
            config.host,
            config.port,
            [&listening](auto* listen_socket) {
                listening = listen_socket != nullptr;
            }
        );
    }

    if (!listening) {
        return 1;
    }

    app.run();
    return 0;
}

}  // namespace

int main() {
    std::string error;
    ListenerConfig listener_config;
    if (!listener_config_from_env(listener_config, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    if (listener_config.use_unix_socket && !prepare_unix_socket(listener_config.unix_socket_path, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    rinha::ReferenceSet refs;
    if (!load_references_from_env(refs, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    auto state = std::make_shared<AppState>(rinha::Classifier(std::move(refs)));
    return run_server(listener_config, state);
}
