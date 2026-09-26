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
        domain_services/sapo/UssdFlowBindingStore.h
        domain_services/sapo/UssdFlowBindingStore.cc
        domain_services/sapo/ProviderAdapters.h
        domain_services/sapo/ProviderAdapters.cc
        domain_services/sapo/UssdSessionOrchestrator.h
        domain_services/sapo/UssdSessionOrchestrator.cc
        domain_services/sapo/UssdInteraction.h
        domain_services/sapo/DrogonLogSink.h
        domain_services/sapo/BlockingRunner.h
        utils/JsonBridge.h
)

find_package(Drogon CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ##############################################################################
# Sapo Engine (USSD workflow runtime) — Prebuilt Install Tree Only
#
# Resolution order for prebuilt SapoEngine:
#   1. -DWSSD_SAPO_PREFIX=<dir> or $ENV{SAPO_PREFIX}
#   2. <repo>/vendor/sapo
#
# The pinned version comes from sapo-engine.lock and is enforced with
# find_package(SapoEngine <version> EXACT).
# ##############################################################################
set(WSSD_SAPO_LOCK_FILE "${CMAKE_CURRENT_SOURCE_DIR}/sapo-engine.lock")
set(WSSD_SAPO_VERSION "")
if(EXISTS "${WSSD_SAPO_LOCK_FILE}")
    file(STRINGS "${WSSD_SAPO_LOCK_FILE}" _sapo_lock_lines
            REGEX "^[ \t]*SAPO_ENGINE_VERSION=")
    if(_sapo_lock_lines)
        list(GET _sapo_lock_lines -1 _sapo_lock_line)
        string(REGEX REPLACE "^[ \t]*SAPO_ENGINE_VERSION=[ \t]*" ""
                WSSD_SAPO_VERSION "${_sapo_lock_line}")
        string(REGEX REPLACE "[ \t\r\"]+$" "" WSSD_SAPO_VERSION "${WSSD_SAPO_VERSION}")
    endif()
    unset(_sapo_lock_lines)
    unset(_sapo_lock_line)
endif()

set(WSSD_SAPO_PREFIX "" CACHE PATH
        "Install prefix of a prebuilt SapoEngine tree (overrides SAPO_PREFIX and vendor/sapo)")

set(WSSD_SAPO_TARGET "")
set(WSSD_SAPO_INCLUDE_DIRS "")

# ---- Locate prebuilt install tree --------------------------------------------
set(_sapo_candidates "")
if(NOT "${WSSD_SAPO_PREFIX}" STREQUAL "")
    list(APPEND _sapo_candidates "${WSSD_SAPO_PREFIX}")
endif()
if(NOT "$ENV{SAPO_PREFIX}" STREQUAL "")
    list(APPEND _sapo_candidates "$ENV{SAPO_PREFIX}")
endif()
list(APPEND _sapo_candidates "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo")
list(REMOVE_DUPLICATES _sapo_candidates)

set(_sapo_find_args CONFIG QUIET)
if(NOT "${WSSD_SAPO_VERSION}" STREQUAL "")
    list(PREPEND _sapo_find_args "${WSSD_SAPO_VERSION}" EXACT)
endif()

foreach(_sapo_cand IN LISTS _sapo_candidates)
    if(WSSD_SAPO_TARGET)
        break()
    endif()
    if(NOT EXISTS "${_sapo_cand}/lib/cmake/SapoEngine/SapoEngineConfig.cmake")
        continue()
    endif()
    if(NOT EXISTS "${_sapo_cand}/lib/libsapo_core.a")
        message(STATUS "WssdApi: skipping incomplete SapoEngine prefix ${_sapo_cand} "
                "(has CMake config, no lib/libsapo_core.a)")
        continue()
    endif()
    unset(SapoEngine_DIR)
    unset(SapoEngine_DIR CACHE)
    find_package(SapoEngine ${_sapo_find_args} PATHS "${_sapo_cand}" NO_DEFAULT_PATH)
    if(SapoEngine_FOUND AND TARGET Sapo::sapo_core)
        set(WSSD_SAPO_TARGET Sapo::sapo_core)
        message(STATUS "WssdApi: using prebuilt SapoEngine "
                "${WSSD_SAPO_VERSION} from ${_sapo_cand}")
    else()
        message(STATUS "WssdApi: SapoEngine prefix ${_sapo_cand} does not provide "
                "version ${WSSD_SAPO_VERSION} (see sapo-engine.lock)")
    endif()
endforeach()
unset(_sapo_cand)

if(WSSD_SAPO_TARGET)
    get_target_property(_sapo_incs Sapo::sapo_core INTERFACE_INCLUDE_DIRECTORIES)
    if(_sapo_incs)
        list(APPEND WSSD_SAPO_INCLUDE_DIRS ${_sapo_incs})
    endif()
    if(TARGET Sapo::sapo_vendored)
        get_target_property(_sapo_thirdparty Sapo::sapo_vendored INTERFACE_INCLUDE_DIRECTORIES)
        if(_sapo_thirdparty)
            list(APPEND WSSD_SAPO_INCLUDE_DIRS ${_sapo_thirdparty})
        endif()
        unset(_sapo_thirdparty)
    endif()
    unset(_sapo_incs)
else()
    message(FATAL_ERROR
            "No prebuilt SapoEngine ${WSSD_SAPO_VERSION} found.\n"
            "Fetch the published artifact and pass its prefix:\n"
            "  ./scripts/fetch-sapo-engine.sh --dest build/sapo-prefix\n"
            "  cmake -S . -B build -DWSSD_SAPO_PREFIX=$PWD/build/sapo-prefix\n"
            "Prefixes examined: ${_sapo_candidates}\n"
            "A prefix needs lib/libsapo_core.a, lib/cmake/SapoEngine/ and bin/sapoc.")
endif()

if(NOT WSSD_SAPO_INCLUDE_DIRS)
    set(WSSD_SAPO_INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo/third_party")
endif()

unset(_sapo_candidates)
unset(_sapo_find_args)

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

add_subdirectory(test)