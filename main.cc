#include <drogon/drogon.h>

// The Sapo engine lifecycle is owned by SapoEnginePlugin (plugins/,
// registered in config.json): it configures the VirtualMachine, wires the
// Drogon HTTP transport and log sink, selects the state store, loads
// blueprints, and shuts everything down with the app.
int main() {
    // Load config file
    //drogon::app().loadConfigFile("config.json");
    drogon::app().loadConfigFile("../config.json");

    // Run HTTP framework, the method will block in the internal event loop
    drogon::app().run();
    return 0;
}
