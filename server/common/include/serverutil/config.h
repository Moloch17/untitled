#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace serverutil {

// Configuration comes from the environment: it's what docker compose passes
// naturally, and it keeps credentials out of the repo.

inline std::string envString(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : fallback;
}

inline uint16_t envPort(const char* name, uint16_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 && parsed <= 65535 ? static_cast<uint16_t>(parsed) : fallback;
}

inline uint32_t envUint(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
}

}  // namespace serverutil
