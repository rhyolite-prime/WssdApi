cmake_minimum_required(VERSION 3.28)
project(WssdApi CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options("-include" "${CMAKE_CURRENT_SOURCE_DIR}/utils/codecvt_helper.h" "-Wno-deprecated-declarations")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-fcoroutines)
endif()

include(CheckIncludeFileCXX)

check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)
if (NOT "${CMAKE_CXX_STANDARD}" STREQUAL "")
    # Do nothing
elseif (HAS_ANY AND HAS_STRING_VIEW AND HAS_COROUTINE)
    set(CMAKE_CXX_STANDARD 20)
elseif (HAS_ANY AND HAS_STRING_VIEW)
    set(CMAKE_CXX_STANDARD 17)
else ()
    set(CMAKE_CXX_STANDARD 14)
endif ()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(${PROJECT_NAME} main.cc
        constants/ErrorCodes.h
        domain_services/redis/RedisCacheManager.h
        domain_services/redis/RedisCacheManager.cc
        dto/BaseApiResponse.h
        domain_services/ussd_directory/UssdDirectoryService.h
        domain_services/subscription/SubscriptionService.h
        domain_services/subscription/SubscriptionService.cc
        domain_services/ussd_directory/UssdDirectoryService.cc
        domain_services/ussd_code_inventory/UssdCodeInventoryService.h
        domain_services/ussd_code_inventory/UssdCodeInventoryService.cc
        domain_services/ussd_plugins/UssdPluginService.h
        domain_services/ussd_plugins/UssdPluginService.cc
        domain_services/ussd_templates/UssdTemplateService.h
        domain_services/ussd_templates/UssdTemplateService.cc
        domain_services/billing_ledger/BillingLedgerService.h
        domain_services/billing_ledger/BillingLedgerService.cc
        utils/codecvt_helper.h
        domain_services/wssd_registry/WssdRegistryService.h
        domain_services/wssd_registry/WssdRegistryService.cc
        dto/WssdRegistryDto.h
        dto/UssdDirectoryDto.h
        utils/IdGeneratorUtils.h
        utils/IdGeneratorUtils.cc
        dto/PluginDto.h
        domain_services/turbo_ledger/TurboLedgerService.h
        domain_services/turbo_ledger/TurboLedgerService.cc
        dto/UssdSessionResponse.h
        dto/UssdSessionRequestDto.h
        domain_services/ussd_sessions/UssdSessionService.h
        domain_services/ussd_sessions/UssdSessionService.cc
        domain_services/adapters/DrogonHttpTransport.h
        domain_services/sapo/SapoSettings.h
        domain_services/sapo/SapoSettings.cc
        domain_services/sapo/SapoEngineService.h
        domain_services/sapo/SapoEngineService.cc
        domain_services/sapo/UssdWorkflowResolver.h
        domain_services/sapo/UssdWorkflowResolver.cc
        domain_services/sapo/ProviderAdapters.h
        domain_services/sapo/ProviderAdapters.cc
        domain_services/sapo/UssdSessionOrchestrator.h
        domain_services/sapo/UssdSessionOrchestrator.cc
        domain_services/sapo/UssdInteraction.h
        domain_services/sapo/DrogonLogSink.h
        domain_services/sapo/BlockingRunner.h
        utils/JsonBridge.h
)

# ##############################################################################
# If you include the drogon source code locally in your project, use this method
# to add drogon 
# add_subdirectory(drogon) 
# target_link_libraries(${PROJECT_NAME} PRIVATE drogon)
#
# and comment out the following lines
find_package(Drogon CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ##############################################################################
# Sapo Engine (USSD workflow runtime).
#
# Resolution order:
#   1. Prebuilt install tree under vendor/sapo (include/ + lib/libsapo_core.a
#      as produced by `cmake --install` of SapoEngine).
#   2. Otherwise FetchContent builds SapoEngine from source (needs network at
#      configure time). Pin WSSD_SAPO_GIT_TAG to a release tag in CI.
#
# Either path provides the include directories (engine headers +
# third_party/nlohmann) through the imported target, plus the public
# SAPO_ENABLE_REDIS compile definition the Redis state store is guarded on.
# ##############################################################################
option(WSSD_USE_VENDORED_SAPO "Prefer the prebuilt engine under vendor/sapo when present" ON)
set(WSSD_SAPO_GIT_TAG "master" CACHE STRING "SapoEngine git tag/branch when fetching from source")
set(WSSD_SAPO_TARGET "")

if(WSSD_USE_VENDORED_SAPO AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/lib/libsapo_core.a")
    message(STATUS "WssdApi: using vendored SapoEngine from vendor/sapo")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo")
    find_package(SapoEngine CONFIG REQUIRED)
    set(WSSD_SAPO_TARGET Sapo::sapo_core)
    target_include_directories(${PROJECT_NAME}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo/third_party)
else()
    message(STATUS "WssdApi: fetching SapoEngine (${WSSD_SAPO_GIT_TAG}) via FetchContent")
    include(FetchContent)
    set(SAPO_ENABLE_REDIS ON CACHE BOOL "" FORCE)
    set(SAPO_ENABLE_CPR OFF CACHE BOOL "" FORCE)
    set(SAPO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sapo_engine
            GIT_REPOSITORY https://github.com/rhyolite-prime/SapoEngine.git
            GIT_TAG ${WSSD_SAPO_GIT_TAG}
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(sapo_engine)
    if(TARGET Sapo::sapo_core)
        set(WSSD_SAPO_TARGET Sapo::sapo_core)
    elseif(TARGET sapo_core)
        set(WSSD_SAPO_TARGET sapo_core)
    else()
        message(FATAL_ERROR "SapoEngine provided no usable library target")
    endif()
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
        Drogon::Drogon
        ${WSSD_SAPO_TARGET})

# ##############################################################################

if (CMAKE_CXX_STANDARD LESS 17)
    message(FATAL_ERROR "c++17 or higher is required")
elseif (CMAKE_CXX_STANDARD LESS 20)
    message(STATUS "use c++17")
else ()
    message(STATUS "use c++20")
endif ()

aux_source_directory(controllers CTL_SRC)
aux_source_directory(filters FILTER_SRC)
aux_source_directory(plugins PLUGIN_SRC)
aux_source_directory(models MODEL_SRC)

drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
        ${CMAKE_CURRENT_BINARY_DIR})
# use the following line to create views with namespaces.
# drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
#                     ${CMAKE_CURRENT_BINARY_DIR} TRUE)
# use the following line to create views with namespace CHANGE_ME prefixed
# and path namespaces.
# drogon_create_views(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/views
#                     ${CMAKE_CURRENT_BINARY_DIR} TRUE CHANGE_ME)


target_include_directories(${PROJECT_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/models)


target_sources(${PROJECT_NAME}
        PRIVATE
        ${SRC_DIR}
        ${CTL_SRC}
        ${FILTER_SRC}
        ${PLUGIN_SRC}
        ${MODEL_SRC})
# ##############################################################################
# uncomment the following line for dynamically loading views 
# set_property(TARGET ${PROJECT_NAME} PROPERTY ENABLE_EXPORTS ON)

# ##############################################################################

add_subdirectory(test)
