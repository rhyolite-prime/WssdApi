//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//
#include "IdGeneratorUtils.h"

#include <random>
#include <sstream>

namespace wssd_api::utils {

    std::string IdGeneratorUtils::generateGuid() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 15);

        auto hexDigit = [&]() {
            int v = dist(gen);
            std::stringstream ss;
            ss << std::hex << std::nouppercase << v;
            return ss.str();
        };

        std::stringstream guid;

        // 8-4-4-4-12 pattern
        int groups[] = {8, 4, 4, 4, 12};
        for (int i = 0; i < 5; ++i) {
            if (i > 0)
                guid << "-";
            for (int j = 0; j < groups[i]; ++j) {
                guid << hexDigit();
            }
        }
        return guid.str();
    }

    std::string IdGeneratorUtils::generateRandomSixDigit() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(100000, 999999);
        int num = dist(gen);
        return std::to_string(num);
    }

    std::string IdGeneratorUtils::generateAlphanumericId(size_t length) {
        static const char charset[] = "0123456789"
                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "abcdefghijklmnopqrstuvwxyz";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

        std::string id;
        id.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            id += charset[dist(gen)];
        }
        return id;
    }

}