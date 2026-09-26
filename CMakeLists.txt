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
# Sapo Engine (USSD workflow runtime) — the upstream repository is PRIVATE.
#
# No build path here may assume an anonymous clone works. Resolution order:
#
#   1. A prebuilt install tree (a SapoEngine `cmake --install` prefix), taken
#      from the first of these that holds a COMPLETE tree:
#        -DWSSD_SAPO_PREFIX=<dir> / $ENV{SAPO_PREFIX}  <- what CI passes
#        <repo>/vendor/sapo                            <- opt-in vendoring
#      scripts/fetch-sapo-engine.sh materialises such a prefix from the OCI
#      artifact the engine publishes to GitHub Packages, authenticated with the
#      workflow's own GITHUB_TOKEN — no PAT, no deploy key, no git clone.
#   2. Otherwise, when WSSD_SAPO_SOURCE is ON or AUTO, build from source with
#      FetchContent. This needs git credentials for the private repo; the
#      pre-flight below fails with instructions instead of letting FetchContent
#      die on an opaque 403 or hang on a credential prompt.
#
# WSSD_SAPO_SOURCE=OFF makes the prebuilt tree mandatory — CI uses that so it
# can never silently fall back to building an unpinned engine from source.
#
# The pinned version comes from sapo-engine.lock and is enforced with
# find_package(SapoEngine <version> EXACT), so a stale artifact fails loudly.
# Either path supplies the include directories (engine headers + vendored
# nlohmann) and the SAPO_ENABLE_REDIS definition through the imported target.
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
set(WSSD_SAPO_SOURCE "AUTO" CACHE STRING
        "Engine source policy: AUTO = prebuilt then source, ON = source only, OFF = prebuilt only")
set_property(CACHE WSSD_SAPO_SOURCE PROPERTY STRINGS AUTO ON OFF)
set(WSSD_SAPO_REPO "https://github.com/rhyolite-prime/SapoEngine.git" CACHE STRING
        "SapoEngine git URL, used only when building from source")
set(WSSD_SAPO_GIT_TAG "${WSSD_SAPO_VERSION}" CACHE STRING
        "SapoEngine git tag/branch when building from source (defaults to sapo-engine.lock)")

set(WSSD_SAPO_TARGET "")
set(WSSD_SAPO_INCLUDE_DIRS "")

# ---- 1. prebuilt install tree ------------------------------------------------
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

set(_sapo_stale "")
# SOURCE=ON means "build from source, ignore any prebuilt tree", so do not even
# search: reporting "using prebuilt ..." only to discard it is misleading.
foreach(_sapo_cand IN LISTS _sapo_candidates)
    if(WSSD_SAPO_TARGET OR WSSD_SAPO_SOURCE STREQUAL "ON")
        break()
    endif()
    if(NOT EXISTS "${_sapo_cand}/lib/cmake/SapoEngine/SapoEngineConfig.cmake")
        continue()
    endif()
    # Headers-only trees (vendor/sapo as committed: .gitignore excludes *.a) are
    # not linkable. Skip with a reason rather than letting the generated import
    # check abort on "this file does not exist".
    if(NOT EXISTS "${_sapo_cand}/lib/libsapo_core.a")
        message(STATUS "WssdApi: skipping incomplete SapoEngine prefix ${_sapo_cand} "
                "(has CMake config, no lib/libsapo_core.a)")
        continue()
    endif()
    unset(SapoEngine_DIR)
    unset(SapoEngine_DIR CACHE)
    find_package(SapoEngine ${_sapo_find_args} PATHS "${_sapo_cand}" NO_DEFAULT_PATH)
    # The installed package exports the engine as Sapo::core (SapoEngineTargets
    # .cmake); older/in-tree builds called it Sapo::sapo_core or sapo_core.
    # Accept all of them — checking only one name made a perfectly good prefix
    # look like a version mismatch.
    set(_sapo_found_target "")
    foreach(_sapo_name IN ITEMS Sapo::core Sapo::sapo_core sapo_core)
        if(TARGET ${_sapo_name})
            set(_sapo_found_target ${_sapo_name})
            break()
        endif()
    endforeach()
    unset(_sapo_name)
    if(SapoEngine_FOUND AND _sapo_found_target)
        set(WSSD_SAPO_TARGET ${_sapo_found_target})
        message(STATUS "WssdApi: using prebuilt SapoEngine "
                "${WSSD_SAPO_VERSION} from ${_sapo_cand} (target ${WSSD_SAPO_TARGET})")
    elseif(SapoEngine_FOUND)
        set(_sapo_stale "${_sapo_cand}")
        message(STATUS "WssdApi: SapoEngine prefix ${_sapo_cand} was found but exports "
                "no known engine target (expected one of Sapo::core, Sapo::sapo_core, sapo_core)")
    else()
        # Config present but rejected: almost always a version mismatch against
        # the lock file, which is exactly what EXACT is here to catch.
        set(_sapo_stale "${_sapo_cand}")
        set(_sapo_cand_version "unknown")
        if(EXISTS "${_sapo_cand}/lib/cmake/SapoEngine/SapoEngineConfigVersion.cmake")
            file(STRINGS "${_sapo_cand}/lib/cmake/SapoEngine/SapoEngineConfigVersion.cmake"
                    _sapo_cand_version_line REGEX "^set\\(PACKAGE_VERSION ")
            if(_sapo_cand_version_line)
                list(GET _sapo_cand_version_line 0 _sapo_cand_version)
                string(REGEX REPLACE "^set\\(PACKAGE_VERSION[ \t]*\"([^\"]*)\".*$" "\\1"
                        _sapo_cand_version "${_sapo_cand_version}")
            endif()
            unset(_sapo_cand_version_line)
        endif()
        message(STATUS "WssdApi: SapoEngine prefix ${_sapo_cand} provides version "
                "${_sapo_cand_version}, not the pinned ${WSSD_SAPO_VERSION} (see sapo-engine.lock)")
        unset(_sapo_cand_version)
    endif()
    unset(_sapo_found_target)
endforeach()
unset(_sapo_cand)

if(WSSD_SAPO_TARGET)
    get_target_property(_sapo_incs ${WSSD_SAPO_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
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
elseif(NOT WSSD_SAPO_SOURCE STREQUAL "OFF")
    # ---- 2. build from source (private repo: needs credentials) --------------
    message(STATUS "WssdApi: no prebuilt SapoEngine ${WSSD_SAPO_VERSION}; "
            "building from source (${WSSD_SAPO_REPO} @ ${WSSD_SAPO_GIT_TAG})")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "WSSD_SAPO_SOURCE=${WSSD_SAPO_SOURCE} needs git to clone "
                "SapoEngine, but git was not found. Install git, or fetch a prebuilt "
                "engine with scripts/fetch-sapo-engine.sh and pass "
                "-DWSSD_SAPO_PREFIX=<dir>.")
    endif()
    # Reachability/auth pre-flight. Without it a private-repo clone surfaces as a
    # cryptic FetchContent failure — or worse, blocks on an interactive
    # username prompt. GIT_TERMINAL_PROMPT=0 makes that an immediate error; the
    # variable stays set so the clone FetchContent spawns inherits it too.
    set(ENV{GIT_TERMINAL_PROMPT} "0")
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" ls-remote --heads "${WSSD_SAPO_REPO}"
            RESULT_VARIABLE _sapo_probe
            OUTPUT_QUIET ERROR_QUIET
            TIMEOUT 60)
    if(NOT _sapo_probe EQUAL 0)
        message(FATAL_ERROR
                "Cannot reach ${WSSD_SAPO_REPO} — the repository is private (or the URL is "
                "wrong) and this process holds no credentials for it.\n"
                "Pick one:\n"
                "  a) Use the published prebuilt engine (recommended, no credentials):\n"
                "       ./scripts/fetch-sapo-engine.sh --dest build/sapo-prefix\n"
                "       cmake -S . -B build -DWSSD_SAPO_PREFIX=build/sapo-prefix\n"
                "  b) Grant git read access to the private repo, then re-configure:\n"
                "       git config --global url.\"https://x-access-token:<TOKEN>"
                "@github.com/rhyolite-prime/\".insteadOf \"https://github.com/rhyolite-prime/\"\n"
                "  c) Point at an engine you already built yourself:\n"
                "       cmake -S . -B build -DWSSD_SAPO_PREFIX=/path/to/sapo/install\n"
                "  d) Forbid source builds entirely (CI does this) with "
                "-DWSSD_SAPO_SOURCE=OFF.")
    endif()
    unset(_sapo_probe)
    include(FetchContent)
    set(SAPO_ENABLE_REDIS ON CACHE BOOL "" FORCE)
    set(SAPO_ENABLE_CPR OFF CACHE BOOL "" FORCE)
    set(SAPO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sapo_engine
            GIT_REPOSITORY "${WSSD_SAPO_REPO}"
            GIT_TAG "${WSSD_SAPO_GIT_TAG}"
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(sapo_engine)
    if(TARGET Sapo::core)
        set(WSSD_SAPO_TARGET Sapo::core)
    elseif(TARGET Sapo::sapo_core)
        set(WSSD_SAPO_TARGET Sapo::sapo_core)
    elseif(TARGET sapo_core)
        set(WSSD_SAPO_TARGET sapo_core)
    endif()
    if(WSSD_SAPO_TARGET)
        get_target_property(_sapo_incs ${WSSD_SAPO_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
        if(_sapo_incs)
            list(APPEND WSSD_SAPO_INCLUDE_DIRS ${_sapo_incs})
        endif()
        unset(_sapo_incs)
        if(TARGET sapo_vendored)
            get_target_property(_sapo_thirdparty sapo_vendored INTERFACE_INCLUDE_DIRECTORIES)
            if(_sapo_thirdparty)
                list(APPEND WSSD_SAPO_INCLUDE_DIRS ${_sapo_thirdparty})
            endif()
            unset(_sapo_thirdparty)
        endif()
        message(STATUS "WssdApi: built SapoEngine from source (${WSSD_SAPO_GIT_TAG})")
    endif()
endif()

if(NOT WSSD_SAPO_TARGET)
    message(FATAL_ERROR
            "No usable SapoEngine ${WSSD_SAPO_VERSION} was found and "
            "WSSD_SAPO_SOURCE=${WSSD_SAPO_SOURCE} forbids building it from source.\n"
            "Fetch the published artifact and pass its prefix:\n"
            "  ./scripts/fetch-sapo-engine.sh --dest build/sapo-prefix\n"
            "  cmake -S . -B build -DWSSD_SAPO_PREFIX=$PWD/build/sapo-prefix\n"
            "Prefixes examined: ${_sapo_candidates}\n"
            "A prefix needs lib/libsapo_core.a, lib/cmake/SapoEngine/ and bin/sapoc.\n"
            "Allow a source build instead with -DWSSD_SAPO_SOURCE=AUTO "
            "(requires git credentials for the private repo).")
endif()

# Header search path for targets that use engine headers WITHOUT linking the
# engine (test/WssdApi_test); the linked target propagates these on its own.
if(NOT WSSD_SAPO_INCLUDE_DIRS)
    set(WSSD_SAPO_INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo/third_party")
endif()

unset(_sapo_candidates)
unset(_sapo_find_args)
unset(_sapo_stale)

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
