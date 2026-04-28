/**
 * main.cpp — xoraya-cli
 *
 * CLI Linux autonome pour loggers Xoraya (ML-N4000 et compatibles).
 * Sans Qt — C++17 pur + SDK X2E Linux système.
 *
 * Usage : xoraya-cli <command> [options]
 *
 * Phases :
 *   Phase 1 — scan       
 *   Phase 2 — list       
 *   Phase 3 — download   
 *   Phase 4 — delete   
 */

#include "scanner.hpp"
#include "downloader.hpp"
#include "collector.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Aide
// ---------------------------------------------------------------------------

static void print_help(const char* prog)
{
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  scan\n");
    printf("      Scanne le réseau local et liste les loggers Xoraya disponibles.\n\n");
    printf("  list <device>\n");
    printf("      Liste les mesures présentes sur l'HDD du logger <device>.\n\n");
    printf("  download <device> <dest_dir> [N] [--delete-after-download]\n");
    printf("      Télécharge toutes les mesures (ou la mesure N) dans <dest_dir>.\n");
    printf("      Par défaut : non destructif. Aucune mesure n'est supprimée.\n");
    printf("      Avec --delete-after-download : suppression après download réussi uniquement.\n\n");
    printf("  delete <device> <N>\n");
    printf("      Supprime manuellement la mesure d'index N sur le logger.\n\n");
    printf("  collect [options]\n");
    printf("      Scanne le réseau et télécharge les mesures de tous les loggers.\n");
    printf("      Options :\n");
    printf("        --dest <dir>              Dossier de destination (défaut : ./downloads)\n");
    printf("        --delete-after-download   Supprime les mesures après download réussi\n");
    printf("        --interval <s>            Boucle infinie, re-scan toutes les N secondes (N > 0)\n");
    printf("        --device <ip>             Limite à un logger par IP exacte\n");
    printf("        --dry-run                 Affiche ce qui serait fait, sans télécharger\n");
    printf("        --verbose                 Affiche la progression détaillée par mesure\n\n");
    printf("Options:\n");
    printf("  --help, -h    Affiche cette aide.\n");
    printf("\nExemples:\n");
    printf("  %s scan\n", prog);
    printf("  %s list XORAYA-001\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs 2\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs --delete-after-download\n", prog);
    printf("  %s download XORAYA-001 /tmp/logs 2 --delete-after-download\n", prog);
    printf("  %s delete  XORAYA-001 2\n", prog);
    printf("  %s collect\n", prog);
    printf("  %s collect --dest /tmp/logs\n", prog);
    printf("  %s collect --dest /tmp/logs --interval 60\n", prog);
    printf("  %s collect --device 192.168.1.10 --delete-after-download\n", prog);
    printf("  %s collect --dry-run --verbose\n", prog);
}

// ---------------------------------------------------------------------------
// Commandes
// ---------------------------------------------------------------------------

static int cmd_scan()
{
    fprintf(stderr, "Scan du réseau (2 s)...\n\n");

    auto loggers = scan_network(2000);

    if (loggers.empty()) {
        printf("Aucun logger trouvé.\n");
        return 0;
    }

    // En-tête tableau
    printf("%-25s %-16s %-20s %-18s %s\n",
           "Nom", "IP", "Firmware", "État", "Type");
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

    printf("\n%zu logger(s) trouvé(s).\n", loggers.size());
    return 0;
}


static int cmd_list(const char* device)
{
    return ::cmd_list(std::string(device));
}

static int cmd_download(int argc, char** argv)
{
    // argv[0] = device, argv[1] = dest_dir
    // argv[2..] = optionnels : [N] [--delete-after-download] (ordre libre)
    const std::string device   = argv[0];
    const std::string dest_dir = argv[1];
    int  index        = -1;    // -1 = toutes les mesures
    bool delete_after = false; // non destructif par défaut

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--delete-after-download") == 0) {
            delete_after = true;
        } else {
            // Tenter de parser comme un index numérique
            char* end = nullptr;
            long n = strtol(argv[i], &end, 10);
            if (*end != '\0' || n < 0) {
                fprintf(stderr, "Erreur : argument '%s' non reconnu.\n", argv[i]);
                fprintf(stderr, "Usage: download <device> <dest_dir> [N] [--delete-after-download]\n");
                return 1;
            }
            index = static_cast<int>(n);
        }
    }

    return ::cmd_download(device, dest_dir, index, delete_after);
}

static int cmd_delete(const char* device, const char* index_str)
{
    char* end = nullptr;
    long n = strtol(index_str, &end, 10);
    if (*end != '\0' || n < 0) {
        fprintf(stderr, "Erreur : index '%s' invalide (entier >= 0 attendu)\n", index_str);
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
        if (argc < 4) {
            fprintf(stderr, "Usage: %s download <device> <dest_dir> [N]\n", argv[0]);
            return 1;
        }
        // argv[2] = device, argv[3] = dest_dir, argv[4] (optionnel) = N
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
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--delete-after-download") == 0) {
                opts.delete_after = true;
            } else if (strcmp(argv[i], "--dry-run") == 0) {
                opts.dry_run = true;
            } else if (strcmp(argv[i], "--verbose") == 0) {
                opts.verbose = true;
            } else if (strcmp(argv[i], "--dest") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Erreur : --dest requiert un argument.\n");
                    return 1;
                }
                opts.dest_dir = argv[++i];
            } else if (strcmp(argv[i], "--interval") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Erreur : --interval requiert un argument.\n");
                    return 1;
                }
                char* end = nullptr;
                long n = strtol(argv[++i], &end, 10);
                if (*end != '\0' || n <= 0) {
                    fprintf(stderr, "Erreur : --interval doit être un entier > 0.\n");
                    return 1;
                }
                opts.interval_s = static_cast<int>(n);
            } else if (strcmp(argv[i], "--device") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Erreur : --device requiert un argument.\n");
                    return 1;
                }
                opts.device_filter = argv[++i];
            } else {
                fprintf(stderr, "Erreur : argument '%s' non reconnu.\n", argv[i]);
                fprintf(stderr, "Usage: %s collect [--dest <dir>] [--delete-after-download] "
                                "[--interval <s>] [--device <ip>] [--dry-run] [--verbose]\n", argv[0]);
                return 1;
            }
        }
        return cmd_collect(opts);
    }

    fprintf(stderr, "Erreur : commande inconnue '%s'\n\n", cmd);
    print_help(argv[0]);
    return 1;
}
