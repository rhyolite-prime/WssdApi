cmake_minimum_required(VERSION 3.28)
project(WssdApi_test CXX)

# The Sapo mapping tests compile ProviderAdapters.cc directly: it only touches
# header-declared Sapo shapes (ExecutionOutcome fields, nlohmann/json), so the
# test binary links no Sapo library. WSSD_SAPO_INCLUDE_DIRS is set by the root
# CMakeLists and follows however the engine was resolved (prebuilt prefix or
# FetchContent source tree), so the tests always compile against the same
# headers the binary links. It falls back to the headers committed under
# vendor/sapo/include, which are present in every clone.
add_executable(${PROJECT_NAME}
        test_main.cc
        test_sapo_adapters.cc
        ../domain_services/sapo/ProviderAdapters.cc
        ../domain_services/sapo/UssdFlowBindingStore.cc)

target_include_directories(${PROJECT_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${WSSD_SAPO_INCLUDE_DIRS})

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

ParseAndAddDrogonTests(${PROJECT_NAME})
