#pragma once

#include <string>
#include <cstddef>

/**
 * Affiche la liste des mesures présentes sur l'HDD d'un logger.
 *
 * Séquence :
 *   Connect → LogState → (LogStop si nécessaire) → HddDirMeasurement
 *   → affichage → (LogStart si on avait arrêté) → Disconnect
 *
 * @param device  Nom ou IP du logger Xoraya (ex: "XORAYA-001" ou "192.168.1.10")
 * @return        0 si succès, 1 si erreur
 */
int cmd_list(const std::string& device);

/**
 * Télécharge des mesures depuis un logger Xoraya vers un répertoire local.
 *
 * Par défaut : non destructif. La suppression post-download est explicitement
 * opt-in via delete_after=true (flag CLI : --delete-after-download).
 *
 * Règles de suppression si delete_after=true :
 *   1. La suppression n'a lieu qu'après un download réussi.
 *   2. Elle est annoncée clairement dans la console.
 *   3. Un échec de suppression retourne une erreur (même si le download a réussi).
 *
 * @param device        Nom ou IP du logger
 * @param dest_dir      Répertoire de destination (créé si absent)
 * @param index         Index de la mesure à télécharger, ou -1 pour toutes
 * @param delete_after  false par défaut — passer true uniquement si --delete-after-download
 * @return              0 si succès, 1 si erreur
 */
int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index = -1,
                 bool delete_after = false,
                 bool stop_logging = false);

/**
 * Supprime une mesure ciblée par son index sur le logger.
 *
 * Commande séparée, explicite, indépendante du download.
 * Dispatche sur HddDeleteMeasurement (Gen2) ou HddDeleteFinalMeasurement (Gen3).
 *
 * @param device  Nom ou IP du logger
 * @param index   Index de la mesure à supprimer (doit exister sur l'HDD)
 * @return        0 si succès, 1 si erreur
 */
int cmd_delete(const std::string& device, int index);

/**
 * Demande l'annulation du download en cours (appelable depuis un handler SIGINT).
 *
 * Positionne un flag interne et appelle ForceCancel() sur l'engine::Download actif,
 * ce qui fait sortir WaitForEndOfDownload() avec l'état Abort.
 * Sans effet si aucun download n'est en cours.
 */
void abort_downloads();
