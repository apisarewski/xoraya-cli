/**
 * scanner.cpp — Network scan for Xoraya loggers
 *
 * Based on: samples Xoraya/scan/main.cpp
 * SDK API : CScansCommand (x2e/scans_Command.h)
 *           CLoggerBCAnswerMsg (x2e/scans_message.h)
 *           ip::v4::Address (x2e/Connection.h)
 *
 * Note: do not define X2E_XorayaWin32_DYN_LINKING_AUTO —
 *       link directly with -lxorayasdk via the Makefile.
 */

#include "scanner.hpp"

#include "x2e/scans_Command.h"
#include "x2e/Connection.h"
#include "x2e/X2ETypes.h"

#include <cstdio>

using namespace x2e;
using namespace x2e::logger::connection;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static const char* ct_to_string(ConnectionType::TypeId::type_t id)
{
    switch (id)
    {
        case ConnectionType::TypeId::Generation_1:    return "Gen1";
        case ConnectionType::TypeId::Generation_2:    return "Gen2 (N4000)";
        case ConnectionType::TypeId::Generation_3:    return "Gen3";
        case ConnectionType::TypeId::DataCube:        return "DataCube";
        case ConnectionType::TypeId::DataCubeNSeries: return "DataCubeNSeries";
        case ConnectionType::TypeId::DLNcluster:      return "DLNcluster";
        default:                                      return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<LoggerInfo> scan_network(int timeout_ms)
{
    std::vector<LoggerInfo> result;

    CScansCommand scanner;
    // Start() blocks for timeout_ms then returns
    scanner.Start(static_cast<size_t>(timeout_ms));

    for (const auto& msg : scanner.Result())
    {
        LoggerInfo info;
        info.name     = msg.getName()     ? msg.getName()     : "";
        info.firmware = msg.getVersionFW() ? msg.getVersionFW() : "";
        info.state    = LoggerState::toFriendly(msg.getState());
        info.type     = ct_to_string(msg.getConnectionType());

        // getIP() returns a uint32_t (network byte order → Address handles the conversion)
        info.ip = ip::v4::Address(msg.getIP()).to_string();

        result.push_back(std::move(info));
    }

    return result;
}
