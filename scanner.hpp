#pragma once

#include <string>
#include <vector>

/**
 * Information about a Xoraya logger discovered on the network.
 */
struct LoggerInfo {
    std::string name;
    std::string ip;
    std::string firmware;
    std::string state;
    std::string type;
};

/**
 * Scans the local network for Xoraya loggers.
 *
 * @param timeout_ms  Scan duration in milliseconds (default: 2000 ms)
 * @return            List of discovered loggers
 */
std::vector<LoggerInfo> scan_network(int timeout_ms = 2000);
