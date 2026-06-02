/**
 * collector.cpp — collect command implementation
 *
 * Scans the network, downloads measurements from all discovered loggers
 * (or a specific logger filtered by IP), optionally in a loop.
 *
 * Reuses directly:
 *   - scan_network()   (scanner.hpp)
 *   - cmd_download()   (downloader.hpp)
 */

#include "collector.hpp"
#include "scanner.hpp"
#include "downloader.hpp"
#include "StatusWriter.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Ctrl+C signal
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int)
{
    g_stop = 1;
    abort_downloads(); // interrupts WaitForEndOfDownload() if a download is in progress
}

// ---------------------------------------------------------------------------
// Destination directory creation
// ---------------------------------------------------------------------------

/**
 * Recursively creates directory dest (equivalent to mkdir -p).
 * Returns 0 on success (or if already exists), -1 on error.
 */
static int mkdir_p(const std::string& path)
{
    if (path.empty()) return -1;

    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
        return 0;

    std::string parent = path;
    auto pos = parent.rfind('/');
    if (pos == std::string::npos || pos == 0)
        return -1;

    parent = parent.substr(0, pos);
    if (mkdir_p(parent) != 0)
        return -1;

    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
        return 0;

    return -1;
}

// ---------------------------------------------------------------------------
// stdout/stderr redirection for simple mode
// ---------------------------------------------------------------------------

struct FdGuard {
    int saved_out = -1;
    int saved_err = -1;
    int devnull   = -1;

    void suppress() {
        saved_out = dup(STDOUT_FILENO);
        saved_err = dup(STDERR_FILENO);
        devnull   = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
            devnull = -1;
        }
    }

    void restore() {
        fflush(stdout);
        fflush(stderr);
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); saved_out = -1; }
        if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); saved_err = -1; }
    }
};

// ---------------------------------------------------------------------------
// Timestamp header for each scan
// ---------------------------------------------------------------------------

static void print_timestamp()
{
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    printf("[%04d-%02d-%02d %02d:%02d:%02d UTC] ",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
}

// ---------------------------------------------------------------------------
// Single collect pass
// ---------------------------------------------------------------------------

/**
 * Runs one collect pass: scan + filter + download.
 * Returns the number of loggers that failed.
 */
static int run_pass(const CollectOptions& opts)
{
    // --- Scan ---
    print_timestamp();
    printf("Scanning network (2 s)...\n");
    fflush(stdout);

    StatusWriter::setScanning(opts.dest_dir);
    auto loggers = scan_network(2000);

    // --- Filter --device (exact IP) ---
    if (!opts.device_filter.empty()) {
        std::vector<LoggerInfo> filtered;
        for (const auto& lg : loggers)
            if (lg.ip == opts.device_filter)
                filtered.push_back(lg);
        loggers = std::move(filtered);
    }

    if (loggers.empty()) {
        printf("  No logger found%s.\n\n",
               opts.device_filter.empty() ? "" : " for this filter");
        return 0;
    }

    // --- Dry-run ---
    if (opts.dry_run) {
        printf("  [DRY-RUN] %zu logger(s) that would be processed:\n", loggers.size());
        for (const auto& lg : loggers) {
            printf("    %-25s %-16s → download to %s",
                   lg.name.c_str(), lg.ip.c_str(), opts.dest_dir.c_str());
            if (opts.delete_after)
                printf("  (+ delete after download)");
            printf("\n");
        }
        printf("  No action taken.\n\n");
        return 0;
    }

    // --- Create destination directory ---
    if (mkdir_p(opts.dest_dir) != 0) {
        fprintf(stderr, "Error: cannot create directory '%s': %s\n",
                opts.dest_dir.c_str(), strerror(errno));
        return static_cast<int>(loggers.size());
    }

    // --- Download ---
    int fail = 0;

    for (const auto& lg : loggers) {
        if (g_stop) break;

        if (opts.verbose) {
            printf("\n--- %s (%s) ---\n", lg.name.c_str(), lg.ip.c_str());
            fflush(stdout);
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging, opts.last_n);
            if (rc != 0) {
                fprintf(stderr, "  [collect] Error on '%s'.\n", lg.name.c_str());
                ++fail;
            }
        } else {
            // Simple mode: one line per logger
            printf("  %-25s → ", lg.name.c_str());
            fflush(stdout);

            FdGuard guard;
            guard.suppress();
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging, opts.last_n);
            guard.restore();

            if (rc == 0) {
                printf("OK\n");
            } else {
                printf("ERROR\n");
                ++fail;
            }
            fflush(stdout);
        }
    }

    // --- Summary ---
    int total = static_cast<int>(loggers.size());
    int ok    = total - fail;
    printf("\n  %d logger(s): %d succeeded", total, ok);
    if (fail > 0)
        printf(", %d failed", fail);
    printf(".\n\n");

    return fail;
}

// ---------------------------------------------------------------------------
// cmd_collect
// ---------------------------------------------------------------------------

int cmd_collect(const CollectOptions& opts)
{
    signal(SIGINT, on_sigint);

    int global_fail = 0;

    while (true) {
        int fail = run_pass(opts);
        if (fail > 0)
            global_fail = 1;

        if (g_stop) {
            printf("Collect interrupted (Ctrl+C).\n");
            break;
        }

        if (opts.interval_s <= 0)
            break;

        printf("Next scan in %d second(s). Ctrl+C to stop.\n\n",
               opts.interval_s);
        fflush(stdout);

        // sleep interruptible by SIGINT
        sleep(static_cast<unsigned>(opts.interval_s));

        if (g_stop) {
            printf("Collect interrupted (Ctrl+C).\n");
            break;
        }
    }

    StatusWriter::clear();
    return global_fail;
}
