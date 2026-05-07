#pragma once

#include <string>

/**
 * Options for the collect command.
 */
struct CollectOptions {
    std::string dest_dir     = "/home/Dexterlogs";
    bool        delete_after  = false;
    int         interval_s    = -1;    // -1 = single pass; >0 = loop
    std::string device_filter;         // empty = all; otherwise exact IP
    bool        dry_run       = false;
    bool        verbose       = false;
    bool        stop_logging  = false; // stop logging during download (opt-in)
    int         last_n        = -1;    // -1 = all; >0 = only the N most recent
};

/**
 * Scans the network and downloads measurements from each discovered logger.
 *
 * - Without --interval: single pass, returns 0 if everything succeeds.
 * - With --interval: infinite loop, interruptible via Ctrl+C (SIGINT).
 * - On logger error: continues with remaining loggers, returns 1 at the end.
 * - No logger found: informational message, returns 0 (not an error).
 * - --dry-run: prints what would happen, no network/disk action.
 *
 * @param opts  Collect options
 * @return      0 if all downloads succeeded (or dry-run), 1 otherwise
 */
int cmd_collect(const CollectOptions& opts);
