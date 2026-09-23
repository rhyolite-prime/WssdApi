#include <drogon/drogon.h>

#include "domain_services/adapters/DrogonHttpTransport.h"
#include <runtime/VirtualMachine.hpp>

int main() {

    sapo::runtime::TaskServices services = sapo::runtime::TaskServices::defaults();

    // Logger
    auto logger = std::make_shared<sapo::obs::Logger>();
    logger->setLevel(sapo::obs::LogLevel::Info);
    services.logger = logger;

    // State Store (Choose single-node FileStateStore or multi-node RedisStateStore)
    const char* redis_host = std::getenv("SAPO_REDIS_HOST");
    if (redis_host) {
#if defined(SAPO_ENABLE_REDIS)
        auto redis_client = std::make_shared<sapo::redis::SocketRedisClient>(redis_host, 6379);
        sapo::redis::RedisStateStoreOptions redis_opts;
        redis_opts.ttl_seconds = 900; // 15-minute expiration for abandoned sessions
        services.state_store = std::make_shared<sapo::redis::RedisStateStore>(redis_client, redis_opts);
#endif
    } else {
        services.state_store = std::make_shared<sapo::runtime::FileStateStore>("sapo/state-store");
    }

    // 2. Inject your Drogon transport!
    services.transport = std::make_shared<HostDrogonTransport>();

    // 3. Hand services to the VirtualMachine
    auto vm = std::make_unique<sapo::runtime::VirtualMachine>(std::move(services));
    vm->start();

    //Load config file
    //drogon::app().loadConfigFile("../config.json");
    drogon::app().loadConfigFile("config.json");

    //Run HTTP framework,the method will block in the internal event loop
    drogon::app().run();
    return 0;
}
