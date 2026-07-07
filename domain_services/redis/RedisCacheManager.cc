//
// Created by Emmanuel Addo-Odame on 12/06/2026.
//

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include "dto/BaseApiResponse.h"
#include "RedisCacheManager.h"


namespace wssd_api::domain_services {


    drogon::Task<std::string> RedisCacheManager::getValue(std::string key, bool shouldRemove)
    {
        auto redisClientPtr_ = drogon::app().getRedisClient();
        if (!redisClientPtr_) {
            co_return "";
        }

        try {
            auto result = co_await redisClientPtr_->execCommandCoro(
                "GET %s", key.c_str());

            if (result.type() == drogon::nosql::RedisResultType::kNil) {
                co_return "";
            }

            if (result.type() == drogon::nosql::RedisResultType::kString) {
                std::string value = result.asString();

                if (shouldRemove) {
                    // Fire-and-forget — we already have the value, no need to block
                    redisClientPtr_->execCommandAsync(
                        [](const drogon::nosql::RedisResult&) {},
                        [](const std::exception& e) {
                            LOG_ERROR << "Error removing key from Redis: " << e.what();
                        },
                        "DEL %s", key.c_str());
                }
                co_return value;
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "Redis GET error: " << e.what();
        }

        co_return "";
    }

    drogon::Task<dto::BaseApiResponse> RedisCacheManager::setValue(std::string key, std::string value)
    {
        dto::BaseApiResponse response;
        auto redisClientPtr_ = drogon::app().getRedisClient();
        if (!redisClientPtr_) {
            response.success = false;
            response.message = "Redis client is not available";
            co_return response;
        }

        try {
            auto result = co_await redisClientPtr_->execCommandCoro(
                "SET %s %s", key.c_str(), value.c_str());

            // SET returns the status string "OK"
            if (result.type() == drogon::nosql::RedisResultType::kString
                    && result.asString() == "OK") {
                response.success = true;
                response.message = "Value set successfully";
            } else {
                response.success = false;
                response.message = "Failed to set value in Redis";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "Redis SET error: " << e.what();
            response.success = false;
            response.message = std::string("Redis SET error: ") + e.what();
        }
        co_return response;
    }

    drogon::Task<dto::BaseApiResponse> RedisCacheManager::setValueWithTtl(std::string key, std::string value, int ttlSeconds)
    {
        dto::BaseApiResponse response;
        auto redisClientPtr_ = drogon::app().getRedisClient();
        if (!redisClientPtr_) {
            response.success = false;
            response.message = "Redis client is not available";
            co_return response;
        }

        try {
            // SETEX key seconds value
            auto result = co_await redisClientPtr_->execCommandCoro(
                "SETEX %s %d %s", key.c_str(), ttlSeconds, value.c_str());

            if (result.type() == drogon::nosql::RedisResultType::kString
                    && result.asString() == "OK") {
                response.success = true;
                response.message = "Value set successfully";
            } else {
                response.success = false;
                response.message = "Failed to set value in Redis";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "Redis SETEX error: " << e.what();
            response.success = false;
            response.message = std::string("Redis SETEX error: ") + e.what();
        }
        co_return response;
    }

    drogon::Task<dto::BaseApiResponse> RedisCacheManager::removeValue(std::string key)
    {
        dto::BaseApiResponse response;
        auto redisClientPtr_ = drogon::app().getRedisClient();
        if (!redisClientPtr_) {
            response.success = false;
            response.message = "Redis client is not available";
            co_return response;
        }

        try {
            auto result = co_await redisClientPtr_->execCommandCoro(
                "DEL %s", key.c_str());

            // DEL → integer: number of keys deleted
            if (result.type() == drogon::nosql::RedisResultType::kInteger
                    && result.asInteger() > 0) {
                response.success = true;
                response.message = "Value removed successfully";
            } else {
                response.success = false;
                response.message = "Key not found or already removed";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "Redis DEL error: " << e.what();
            response.success = false;
            response.message = std::string("Redis DEL error: ") + e.what();
        }
        co_return response;
    }


}