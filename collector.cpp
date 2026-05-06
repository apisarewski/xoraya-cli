/**
 * collector.cpp — implémentation de la commande collect
 *
 * Scanne le réseau, télécharge les mesures de tous les loggers trouvés
 * (ou d'un logger ciblé par IP), optionnellement en boucle.
 *
 * Réutilise directement :
 *   - scan_network()   (scanner.hpp)
 *   - cmd_download()   (downloader.hpp)
 */

#include "collector.hpp"
#include "scanner.hpp"
#include "downloader.hpp"

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
// Signal Ctrl+C
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int)
{
    g_stop = 1;
    abort_downloads(); // interrompt WaitForEndOfDownload() si un download est en cours
}

// ---------------------------------------------------------------------------
// Création du dossier destination
// ---------------------------------------------------------------------------

/**
 * Crée récursivement le répertoire dest (équivalent mkdir -p).
 * Retourne 0 si succès (ou si déjà existant), -1 si erreur.
 */
static int mkdir_p(const std::string& path)
{
    if (path.empty()) return -1;

    // Essai direct
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
        return 0;

    // Sinon : créer les parents d'abord
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
// Redirection stdout/stderr pour le mode simple
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
// Horodatage pour l'en-tête de chaque scan
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
// Passe unique collect
// ---------------------------------------------------------------------------

/**
 * Effectue une passe de collect : scan + filtre + download.
 * Retourne le nombre de loggers ayant échoué.
 */
static int run_pass(const CollectOptions& opts)
{
    // --- Scan ---
    print_timestamp();
    printf("Scan du réseau (2 s)...\n");
    fflush(stdout);

    auto loggers = scan_network(2000);

    // --- Filtre --device (IP exacte) ---
    if (!opts.device_filter.empty()) {
        std::vector<LoggerInfo> filtered;
        for (const auto& lg : loggers)
            if (lg.ip == opts.device_filter)
                filtered.push_back(lg);
        loggers = std::move(filtered);
    }

    if (loggers.empty()) {
        printf("  Aucun logger trouvé%s.\n\n",
               opts.device_filter.empty() ? "" : " pour ce filtre");
        return 0;
    }

    // --- Dry-run ---
    if (opts.dry_run) {
        printf("  [DRY-RUN] %zu logger(s) qui seraient traités :\n", loggers.size());
        for (const auto& lg : loggers) {
            printf("    %-25s %-16s → download vers %s",
                   lg.name.c_str(), lg.ip.c_str(), opts.dest_dir.c_str());
            if (opts.delete_after)
                printf("  (+ suppression après download)");
            printf("\n");
        }
        printf("  Aucune action effectuée.\n\n");
        return 0;
    }

    // --- Création du dossier destination ---
    if (mkdir_p(opts.dest_dir) != 0) {
        fprintf(stderr, "Erreur : impossible de créer le dossier '%s' : %s\n",
                opts.dest_dir.c_str(), strerror(errno));
        return static_cast<int>(loggers.size()); // tous en échec
    }

    // --- Téléchargement ---
    int fail = 0;

    for (const auto& lg : loggers) {
        if (g_stop) break;

        if (opts.verbose) {
            // Sortie complète de cmd_download
            printf("\n--- %s (%s) ---\n", lg.name.c_str(), lg.ip.c_str());
            fflush(stdout);
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging);
            if (rc != 0) {
                fprintf(stderr, "  [collect] Erreur sur '%s'.\n", lg.name.c_str());
                ++fail;
            }
        } else {
            // Mode simple : une ligne par logger
            printf("  %-25s → ", lg.name.c_str());
            fflush(stdout);

            FdGuard guard;
            guard.suppress();
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging);
            guard.restore();

            if (rc == 0) {
                printf("OK\n");
            } else {
                printf("ERREUR\n");
                ++fail;
            }
            fflush(stdout);
        }
    }

    // --- Résumé ---
    int total = static_cast<int>(loggers.size());
    int ok    = total - fail;
    printf("\n  %d logger(s) : %d réussi(s)", total, ok);
    if (fail > 0)
        printf(", %d échoué(s)", fail);
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
            printf("Collect interrompu (Ctrl+C).\n");
            break;
        }

        if (opts.interval_s <= 0)
            break;

        printf("Prochain scan dans %d seconde(s). Ctrl+C pour arrêter.\n\n",
               opts.interval_s);
        fflush(stdout);

        // sleep interruptible par SIGINT
        sleep(static_cast<unsigned>(opts.interval_s));

        if (g_stop) {
            printf("Collect interrompu (Ctrl+C).\n");
            break;
        }
    }

    return global_fail;
}
