cmake_minimum_required(VERSION 3.28)
project(WssdApi_test CXX)

# The Sapo mapping tests compile ProviderAdapters.cc directly: it only touches
# header-declared Sapo shapes (ExecutionOutcome fields, nlohmann/json), so the
# test binary links no Sapo library. The vendored headers + nlohmann in
# vendor/sapo-dev/include are always present in the repo, independent of how the
# main target resolves the engine (prebuilt vs FetchContent).
add_executable(${PROJECT_NAME}
        test_main.cc
        test_sapo_adapters.cc
        ../domain_services/sapo/ProviderAdapters.cc
        ../domain_services/sapo/UssdFlowBindingStore.cc)

target_include_directories(${PROJECT_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/sapo/include/sapo
        ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/sapo/include/sapo/third_party)

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

ParseAndAddDrogonTests(${PROJECT_NAME})
