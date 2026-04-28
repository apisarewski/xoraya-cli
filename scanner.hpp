#pragma once

#include <string>
#include <vector>

/**
 * Informations sur un logger Xoraya découvert sur le réseau.
 */
struct LoggerInfo {
    std::string name;
    std::string ip;
    std::string firmware;
    std::string state;
    std::string type;
};

/**
 * Scanne le réseau local à la recherche de loggers Xoraya.
 *
 * @param timeout_ms  Durée du scan en millisecondes (défaut : 2000 ms)
 * @return            Liste des loggers trouvés
 */
std::vector<LoggerInfo> scan_network(int timeout_ms = 2000);
