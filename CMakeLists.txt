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
#target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon sapo_core)
target_link_libraries(${PROJECT_NAME} PRIVATE
        Drogon::Drogon)

# Add sapo-engine project
#add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sapo-engine sapo-engine_build)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/models
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sapo/include/sapo/third_party)


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
