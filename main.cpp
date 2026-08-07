/**
 * main.cpp — xoraya-cli
 *
 * Autonomous Linux CLI for Xoraya loggers (ML-N4000 and compatible).
 * No Qt — pure C++17 + X2E Linux system SDK.
 *
 * Usage: xoraya-cli <command> [options]
 *
 * Phases:
 *   Phase 1 — scan
 *   Phase 2 — list
 *   Phase 3 — download
 *   Phase 4 — delete
 */

#include "scanner.hpp"
#include "downloader.hpp"
#include "collector.hpp"
#include "storage.hpp"
#include "StatusWriter.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

static void print_help(const char* prog)
{
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  scan\n");
    printf("      Scans the local network and lists available Xoraya loggers.\n\n");
    printf("  list <device>\n");
    printf("      Lists measurements stored on the HDD of logger <device>.\n\n");
    printf("  download <device> [dest_dir] [N] [--delete-after-download] [--stop-logging] [--last N]\n");
    printf("      Downloads all measurements (or measurement N) to dest_dir.\n");
    printf("      dest_dir defaults to auto-detected external drive when omitted.\n");
    printf("      By default: non-destructive. No measurement is deleted.\n");
    printf("      With --delete-after-download: delete each measurement immediately after download.\n");
    printf("      With --stop-logging: stop logging during download, restart it afterwards.\n");
    printf("      With --last N: download only the N most recent measurements.\n\n");
    printf("  delete <device> <N>\n");
    printf("      Manually deletes measurement at index N from the logger.\n\n");
    printf("  collect [options]\n");
    printf("      Scans the network and downloads measurements from all loggers.\n");
    printf("      Options:\n");
    printf("        --dest <dir>              Destination folder (default: auto-detect external drive)\n");
    printf("        --delete-after-download   Delete measurements after successful download\n");
    printf("        --interval <s>            Loop indefinitely, re-scanning every N seconds (N > 0)\n");
    printf("        --device <ip>             Restrict to one logger by exact IP\n");
    printf("        --dry-run                 Show what would happen without downloading\n");
    printf("        --verbose                 Show detailed per-measurement progress\n");
    printf("        --stop-logging            Stop logging during download, restart it afterwards\n");
    printf("        --last <N>                Download only the N most recent measurements (N >= 1)\n\n");
    printf("  detect-dest\n");
    printf("      Prints the auto-detected external-drive destination path to stdout,\n");
    printf("      or an error to stderr and exits 1 if no single external drive is found.\n");
    printf("      Used by the OLED screen daemon to show storage-detected status.\n\n");
    printf("Options:\n");
    printf("  --help, -h    Display this help.\n");
    printf("\nExamples:\n");
    printf("  %s scan\n", prog);
    printf("  %s list XORAYA-001\n", prog);
    printf("  %s download XORAYA-001\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs 2\n", prog);
    printf("  %s download XORAYA-001 --last 5\n", prog);
    printf("  %s download XORAYA-001 --delete-after-download\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs --delete-after-download\n", prog);
    printf("  %s delete  XORAYA-001 2\n", prog);
    printf("  %s collect\n", prog);
    printf("  %s collect --dest /tmp/logs\n", prog);
    printf("  %s collect --dest /tmp/logs --interval 60\n", prog);
    printf("  %s collect --device 192.168.1.10 --delete-after-download\n", prog);
    printf("  %s collect --dry-run --verbose\n", prog);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static int cmd_scan()
{
    fprintf(stderr, "Scanning network (2 s)...\n\n");

    auto loggers = scan_network(2000);

    if (loggers.empty()) {
        printf("No logger found.\n");
        return 0;
    }

    printf("%-25s %-16s %-20s %-18s %s\n",
           "Name", "IP", "Firmware", "State", "Type");
    printf("%-25s %-16s %-20s %-18s %s\n",
           "-------------------------",
           "----------------",
           "--------------------",
           "------------------",
           "----------");

    for (const auto& lg : loggers) {
        printf("%-25s %-16s %-20s %-18s %s\n",
               lg.name.c_str(),
               lg.ip.c_str(),
               lg.firmware.c_str(),
               lg.state.c_str(),
               lg.type.c_str());
    }

    printf("\n%zu logger(s) found.\n", loggers.size());
    return 0;
}


static int cmd_list(const char* device)
{
    return ::cmd_list(std::string(device));
}

static int cmd_download(int argc, char** argv)
{
    // argv[0] = device
    // argv[1] (optional): dest_dir if it is not a flag and not a bare integer,
    //                     otherwise auto-detects external drive under /media/<user>/
    // remaining: [N] [--delete-after-download] [--stop-logging] [--last N] (any order)
    const std::string device = argv[0];
    std::string dest_dir;
    bool dest_explicit = false;
    int  start        = 1;
    int  index        = -1;
    bool delete_after = false;
    bool stop_logging = false;
    int  last_n       = -1;

    if (argc > 1 && argv[1][0] != '-') {
        char* end = nullptr;
        strtol(argv[1], &end, 10);
        if (*end != '\0') {
            // Not a pure integer → treat as destination directory
            dest_dir = argv[1];
            dest_explicit = true;
            start = 2;
        }
    }

    for (int i = start; i < argc; ++i) {
        if (strcmp(argv[i], "--delete-after-download") == 0) {
            delete_after = true;
        } else if (strcmp(argv[i], "--stop-logging") == 0) {
            stop_logging = true;
        } else if (strcmp(argv[i], "--last") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --last requires an argument.\n");
                return 1;
            }
            char* end = nullptr;
            long n = strtol(argv[++i], &end, 10);
            if (*end != '\0' || n < 1) {
                fprintf(stderr, "Error: --last must be an integer >= 1.\n");
                return 1;
            }
            last_n = static_cast<int>(n);
        } else {
            char* end = nullptr;
            long n = strtol(argv[i], &end, 10);
            if (*end != '\0' || n < 0) {
                fprintf(stderr, "Error: unrecognized argument '%s'.\n", argv[i]);
                fprintf(stderr, "Usage: download <device> [dest_dir] [N] [--delete-after-download] [--stop-logging] [--last N]\n");
                return 1;
            }
            index = static_cast<int>(n);
        }
    }

    if (index >= 0 && last_n >= 1) {
        fprintf(stderr, "Error: a specific index and --last are mutually exclusive.\n");
        return 1;
    }

    if (!dest_explicit) {
        std::string err;
        dest_dir = detect_dest_dir(err);
        if (dest_dir.empty()) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            StatusWriter::setError(err);
            return 1;
        }
    }

    return ::cmd_download(device, dest_dir, index, delete_after, stop_logging, last_n);
}

static int cmd_delete(const char* device, const char* index_str)
{
    char* end = nullptr;
    long n = strtol(index_str, &end, 10);
    if (*end != '\0' || n < 0) {
        fprintf(stderr, "Error: invalid index '%s' (integer >= 0 expected)\n", index_str);
        return 1;
    }
    return ::cmd_delete(std::string(device), static_cast<int>(n));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    if (argc < 2
        || strcmp(argv[1], "--help") == 0
        || strcmp(argv[1], "-h") == 0)
    {
        print_help(argv[0]);
        return 0;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "scan") == 0) {
        return cmd_scan();
    }

    if (strcmp(cmd, "list") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s list <device>\n", argv[0]);
            return 1;
        }
        return cmd_list(argv[2]);
    }

    if (strcmp(cmd, "download") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s download <device> [dest_dir] [N] [--delete-after-download] [--stop-logging] [--last N]\n", argv[0]);
            return 1;
        }
        // argv[2] = device, argv[3..] = optional dest_dir, N, flags
        return cmd_download(argc - 2, argv + 2);
    }

    if (strcmp(cmd, "delete") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s delete <device> <N>\n", argv[0]);
            return 1;
        }
        return cmd_delete(argv[2], argv[3]);
    }

    if (strcmp(cmd, "collect") == 0) {
        CollectOptions opts;
        bool dest_explicit = false;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--delete-after-download") == 0) {
                opts.delete_after = true;
            } else if (strcmp(argv[i], "--dry-run") == 0) {
                opts.dry_run = true;
            } else if (strcmp(argv[i], "--verbose") == 0) {
                opts.verbose = true;
            } else if (strcmp(argv[i], "--stop-logging") == 0) {
                opts.stop_logging = true;
            } else if (strcmp(argv[i], "--dest") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --dest requires an argument.\n");
                    return 1;
                }
                opts.dest_dir = argv[++i];
                dest_explicit = true;
            } else if (strcmp(argv[i], "--interval") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --interval requires an argument.\n");
                    return 1;
                }
                char* end = nullptr;
                long n = strtol(argv[++i], &end, 10);
                if (*end != '\0' || n <= 0) {
                    fprintf(stderr, "Error: --interval must be an integer > 0.\n");
                    return 1;
                }
                opts.interval_s = static_cast<int>(n);
            } else if (strcmp(argv[i], "--device") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --device requires an argument.\n");
                    return 1;
                }
                opts.device_filter = argv[++i];
            } else if (strcmp(argv[i], "--last") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --last requires an argument.\n");
                    return 1;
                }
                char* end = nullptr;
                long n = strtol(argv[++i], &end, 10);
                if (*end != '\0' || n < 1) {
                    fprintf(stderr, "Error: --last must be an integer >= 1.\n");
                    return 1;
                }
                opts.last_n = static_cast<int>(n);
            } else {
                fprintf(stderr, "Error: unrecognized argument '%s'.\n", argv[i]);
                fprintf(stderr, "Usage: %s collect [--dest <dir>] [--delete-after-download] "
                                "[--interval <s>] [--device <ip>] [--dry-run] [--verbose] "
                                "[--stop-logging] [--last N]\n", argv[0]);
                return 1;
            }
        }
        if (!dest_explicit) {
            std::string err;
            opts.dest_dir = detect_dest_dir(err);
            if (opts.dest_dir.empty()) {
                fprintf(stderr, "Error: %s\n", err.c_str());
                StatusWriter::setError(err);
                return 1;
            }
        }
        return cmd_collect(opts);
    }

    if (strcmp(cmd, "detect-dest") == 0) {
        std::string err;
        std::string dest_dir = detect_dest_dir(err);
        if (dest_dir.empty()) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        printf("%s\n", dest_dir.c_str());
        return 0;
    }

    fprintf(stderr, "Error: unknown command '%s'\n\n", cmd);
    print_help(argv[0]);
    return 1;
}
