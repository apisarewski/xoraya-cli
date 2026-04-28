#pragma once

#include <string>

/**
 * Options pour la commande collect.
 */
struct CollectOptions {
    std::string dest_dir     = "./downloads";
    bool        delete_after = false;
    int         interval_s   = -1;    // -1 = passe unique ; >0 = boucle
    std::string device_filter;        // vide = tous ; sinon IP exacte
    bool        dry_run      = false;
    bool        verbose      = false;
};

/**
 * Scanne le réseau, télécharge les mesures de chaque logger trouvé.
 *
 * - Sans --interval : une seule passe, retourne 0 si tout réussit.
 * - Avec --interval : boucle infinie, interruptible par Ctrl+C (SIGINT).
 * - En cas d'erreur sur un logger : continue avec les suivants, retourne 1 à la fin.
 * - Aucun logger trouvé : message d'info, retourne 0 (pas une erreur).
 * - --dry-run : affiche ce qui serait fait, aucune action réseau/disque.
 *
 * @param opts  Options de collect
 * @return      0 si tous les téléchargements ont réussi (ou dry-run), 1 sinon
 */
int cmd_collect(const CollectOptions& opts);
