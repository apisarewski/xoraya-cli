#pragma once
#include <string>

/**
 * Scans /media/<username>/ for mounted external drives.
 *
 * Returns "<mount>/Dexterlogs" when exactly one drive is found.
 * Returns "" and sets err when 0 or 2+ drives are found.
 */
std::string detect_dest_dir(std::string& err);
