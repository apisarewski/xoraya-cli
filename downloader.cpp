/**
 * downloader.cpp — Listing et téléchargement de mesures Xoraya
 *
 * Références :
 *   XorayaUtils/XorayaConnection.cpp  → logique list / stop / restart
 *   samples Xoraya/download_all/      → engine::Download et engine::Copy
 *
 * Portage Qt → C++ pur :
 *   QString         → std::string
 *   QVector         → std::vector
 *   qDebug()        → fprintf(stderr, ...)
 *   QThread::msleep → std::this_thread::sleep_for  (non utilisé ici)
 *
 * Note : ne pas définir X2E_XorayaWin32_DYN_LINKING_AUTO.
 *        On lie via -lxorayasdk dans le Makefile.
 */

#include "downloader.hpp"

#include "x2e/loggerclient.h"
#include "x2e/loggerctrl_if.h"
#include "x2e/LoggerCmd.h"
#include "x2e/LoggerDataConsumer.h"
#include "x2e/HiResDateTime.h"
#include "x2e/LgHddMeasurements.h"
#include "x2e/LgHddFinalMeasurements.h"
#include "x2e/Engine_Copy.h"
#include "x2e/MsgFilterFactory.h"
#include "x2e/Connection.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <csignal>
#include <chrono>
#include <unistd.h>
#include <string>
#include <vector>
#include <filesystem>

using namespace x2e;
using namespace x2e::logger;

// ---------------------------------------------------------------------------
// Annulation du download en cours (Ctrl+C depuis collect)
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_abort = 0;

void abort_downloads()
{
    g_abort = 1;
    // Le polling loop dans download_gen2() détecte g_abort et envoie HddStop
}

// ---------------------------------------------------------------------------
// Helpers locaux
// ---------------------------------------------------------------------------

static std::string format_size(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    }
    return buf;
}

/**
 * Formate un timestamp HiRes SDK en "YYYYMMDD_HHmmss" (UTC).
 * HiResDateTime::getTime() → intervalles de 100 ns depuis l'epoch Unix.
 */
static std::string format_ts(timestampHiResLG_t raw)
{
    time_t secs = static_cast<time_t>(raw / 10'000'000ULL);
    struct tm t;
    gmtime_r(&secs, &t);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
    return buf;
}

/**
 * Corrige le champ DLC (byte 4 de CAN_DataFrame) dans chaque enregistrement
 * fixe d'un fichier MF4 produit par libxorayasdk 1.00.0046.
 *
 * Bug SDK : X2eToMdfConverter écrit 0x00 au byte 4 (DLC_field) mais inscrit
 * correctement la valeur au byte 5 (flag2). Le patch copie byte5 → byte4.
 *
 * Opère en mémoire (lecture complète + réécriture) ; pas de fichier temporaire.
 * Retourne le nombre de corrections appliquées, ou -1 en cas d'erreur.
 */
static int patch_mf4_dlc(const std::string& path)
{
    // --- Chargement complet du fichier ---
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) {
        fprintf(stderr, "  [patch_dlc] Impossible d'ouvrir '%s'\n", path.c_str());
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 64) { fclose(f); return -1; }

    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    if (static_cast<long>(fread(buf.data(), 1, static_cast<size_t>(fsize), f)) != fsize) {
        fclose(f);
        fprintf(stderr, "  [patch_dlc] Lecture échouée : '%s'\n", path.c_str());
        return -1;
    }

    // MF4 ID block commence par "MDF     " (8 octets), pas "##ID"
    if (fsize < 8 || memcmp(buf.data(), "MDF     ", 8) != 0) {
        fclose(f);
        fprintf(stderr, "  [patch_dlc] Fichier MF4 invalide : '%s'\n", path.c_str());
        return -1;
    }

    const size_t fsz = static_cast<size_t>(fsize);

    // Helpers de lecture little-endian avec borne
    auto safe_r8 = [&](size_t o) -> uint8_t {
        return (o < fsz) ? buf[o] : 0;
    };
    auto safe_r16 = [&](size_t o) -> uint16_t {
        if (o + 2 > fsz) return 0;
        uint16_t v; memcpy(&v, buf.data() + o, 2); return v;
    };
    auto safe_r32 = [&](size_t o) -> uint32_t {
        if (o + 4 > fsz) return 0;
        uint32_t v; memcpy(&v, buf.data() + o, 4); return v;
    };
    auto safe_r64 = [&](size_t o) -> uint64_t {
        if (o + 8 > fsz) return 0;
        uint64_t v; memcpy(&v, buf.data() + o, 8); return v;
    };
    // Lit un offset de lien MF4 (int64, 0 = null)
    auto link_at = [&](size_t o) -> size_t {
        int64_t v; if (o + 8 > fsz) return 0;
        memcpy(&v, buf.data() + o, 8);
        return (v > 0) ? static_cast<size_t>(v) : 0;
    };

    // Chaque bloc MF4 : id(4) + res(4) + len(8) + lnk_cnt(8) = 24 octets d'en-tête
    // Liens  : lnk_cnt * 8 octets
    // Données: à partir de off + 24 + lnk_cnt * 8

    // ##HD toujours à l'offset 64 ; liens[0] = premier ##DG
    const size_t HD_OFF = 64;
    size_t dg_off = link_at(HD_OFF + 24);   // HD.links[0]

    int total_patches = 0;

    while (dg_off != 0 && dg_off + 32 < fsz) {
        if (memcmp(buf.data() + dg_off, "##DG", 4) != 0) break;

        uint64_t dg_lnk_cnt = safe_r64(dg_off + 16);
        size_t   dg_data     = dg_off + 24 + dg_lnk_cnt * 8;

        // DG.links : [0]=next_DG  [1]=first_CG  [2]=data_block
        size_t next_dg  = link_at(dg_off + 24);
        size_t cg_first = link_at(dg_off + 24 +  8);
        size_t data_blk = link_at(dg_off + 24 + 16);

        uint8_t rec_id_sz = safe_r8(dg_data);
        if (rec_id_sz != 2) { dg_off = next_dg; continue; }

        // --- Table des CG : rec_id → {is_vlsd, data_bytes} ---
        struct CgEntry { bool valid; bool is_vlsd; uint32_t data_bytes; };
        CgEntry cg_tbl[256] = {};   // rec_ids sont petits en pratique

        bool has_fixed = false;
        size_t cg_off = cg_first;

        while (cg_off != 0 && cg_off + 32 < fsz) {
            if (memcmp(buf.data() + cg_off, "##CG", 4) != 0) break;

            uint64_t cg_lnk_cnt = safe_r64(cg_off + 16);
            size_t   cg_data    = cg_off + 24 + cg_lnk_cnt * 8;
            size_t   next_cg    = link_at(cg_off + 24);  // CG.links[0]

            uint64_t rec_id64   = safe_r64(cg_data + 0);   // cg_record_id
            uint16_t cg_flags   = safe_r16(cg_data + 16);   // après rec_id(8)+cycle_cnt(8)
            uint32_t data_bytes = safe_r32(cg_data + 24);   // après +flags(2)+sep(2)+res(4)

            if (rec_id64 < 256) {
                bool is_vlsd = (cg_flags & 0x0001) != 0;
                uint8_t rid  = static_cast<uint8_t>(rec_id64);
                cg_tbl[rid]  = { true, is_vlsd, data_bytes };
                if (!is_vlsd && (rid == 1 || rid == 3 || rid == 5 || rid == 7))
                    has_fixed = true;
            }
            cg_off = next_cg;
        }

        if (!has_fixed) { dg_off = next_dg; continue; }

        // --- Collecte des blocs DT (direct ou via DL) ---
        struct DtSpan { size_t start; size_t len; };
        std::vector<DtSpan> dt_spans;

        if (data_blk != 0 && data_blk + 24 < fsz) {
            if (memcmp(buf.data() + data_blk, "##DT", 4) == 0) {
                uint64_t blk_len = safe_r64(data_blk + 8);
                if (blk_len > 24)
                    dt_spans.push_back({ data_blk + 24, static_cast<size_t>(blk_len - 24) });

            } else if (memcmp(buf.data() + data_blk, "##DL", 4) == 0) {
                size_t dl_off = data_blk;
                while (dl_off != 0 && dl_off + 32 < fsz) {
                    if (memcmp(buf.data() + dl_off, "##DL", 4) != 0) break;
                    uint64_t dl_lnk_cnt = safe_r64(dl_off + 16);
                    size_t   dl_next    = link_at(dl_off + 24);  // DL.links[0]
                    // DL.links[1..dl_lnk_cnt-1] = DT blocks
                    for (uint64_t k = 1; k < dl_lnk_cnt; ++k) {
                        size_t dt_off = link_at(dl_off + 24 + k * 8);
                        if (dt_off == 0 || dt_off + 24 >= fsz) continue;
                        if (memcmp(buf.data() + dt_off, "##DT", 4) != 0) continue;
                        uint64_t blk_len = safe_r64(dt_off + 8);
                        if (blk_len > 24)
                            dt_spans.push_back({ dt_off + 24, static_cast<size_t>(blk_len - 24) });
                    }
                    dl_off = dl_next;
                }
            }
        }

        // --- Scan des enregistrements et patch ---
        // Enregistrement fixe (30 octets) :
        //   rec_id(2) + timestamp(8) + Async(1) + CAN_DataFrame(19)
        //   CAN_DataFrame byte 4 (offset 15) = DLC_field (0x00 bug) ← cible
        //   CAN_DataFrame byte 5 (offset 16) = flag2 (DLC correct)  ← source

        for (const auto& span : dt_spans) {
            size_t pos = 0;
            while (pos + 2 <= span.len) {
                uint8_t rid = buf[span.start + pos] |
                              (static_cast<uint8_t>(buf[span.start + pos + 1]) & 0);
                // rec_id est little-endian uint16 ; dans ces fichiers toujours < 256
                uint16_t rec_id;
                memcpy(&rec_id, buf.data() + span.start + pos, 2);
                if (rec_id >= 256 || !cg_tbl[rec_id].valid) break;
                (void)rid;

                const CgEntry& cg = cg_tbl[rec_id];
                if (!cg.is_vlsd) {
                    // Enregistrement fixe
                    size_t rec_end = pos + 2 + cg.data_bytes;
                    if (rec_end > span.len) break;
                    size_t dlc_abs  = span.start + pos + 15;
                    size_t flag_abs = span.start + pos + 16;
                    if (buf[dlc_abs] == 0 && buf[flag_abs] != 0) {
                        buf[dlc_abs] = buf[flag_abs];
                        ++total_patches;
                    }
                    pos = rec_end;
                } else {
                    // Enregistrement VLSD : rec_id(2) + length(4) + data(length)
                    if (pos + 6 > span.len) break;
                    uint32_t vlen;
                    memcpy(&vlen, buf.data() + span.start + pos + 2, 4);
                    pos += 2 + 4 + vlen;
                }
            }
        }

        dg_off = next_dg;
    }

    // Réécriture si des corrections ont été appliquées
    if (total_patches > 0) {
        rewind(f);
        if (fwrite(buf.data(), 1, fsz, f) != fsz) {
            fclose(f);
            fprintf(stderr, "  [patch_dlc] Écriture échouée : '%s'\n", path.c_str());
            return -1;
        }
    }

    fclose(f);
    return total_patches;
}

/**
 * Arrête le logging si le logger est en train d'enregistrer.
 *
 * @param ctrl          Contrôleur connecté
 * @param had_to_stop   [out] true si on a effectivement arrêté le logging
 * @return              true si succès (ou déjà arrêté), false en cas d'erreur SDK
 */
static bool stop_logging_if_needed(LoggerCtrl& ctrl, bool& had_to_stop)
{
    had_to_stop = false;

    cmd::LogState logStateCmd;
    auto err = ctrl->DoCmd(logStateCmd);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : impossible de lire l'état du logger : %s\n",
                err.AsString().c_str());
        return false;
    }

    // Déjà arrêté → rien à faire
    if (logStateCmd.isStopped() == XBool::True) {
        return true;
    }

    fprintf(stderr, "Arrêt du logging en cours...\n");

    // Arrêter le logging sur le stockage HDD uniquement
    // (miroir de XorayaConnection::getHddMeasurementList)
    cmd::LogStop cmdStop(cmd::LogStop::Force::Skip_Pending_Data);
    cmdStop.Set(cmd::types::DataTarget::TypeId::LoggerStorage);
    err = ctrl->DoCmd(cmdStop);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : arrêt du logging échoué : %s\n",
                err.AsString().c_str());
        return false;
    }

    had_to_stop = true;
    return true;
}

/**
 * Redémarre le logging avec les mêmes cibles qu'avant l'arrêt.
 * Miroir de XorayaConnection::startLogging().
 *
 * @param ctrl  Contrôleur connecté
 */
static void restart_logging(LoggerCtrl& ctrl)
{
    // Relire l'état courant pour connaître les data sinks actifs
    cmd::LogState stateCmd;
    auto err = ctrl->DoCmd(stateCmd);
    if (!err.IsNone()) {
        fprintf(stderr, "Avertissement : impossible de relire l'état pour redémarrer : %s\n",
                err.AsString().c_str());
        return;
    }

    if (stateCmd.isStopped() != XBool::True) {
        return; // déjà en train de logger, rien à faire
    }

    // Accumuler les targets depuis le DataSink (uint32_t bitmask)
    uint32_t targets = 0;
    for (const auto& sink : stateCmd.DataSink()) {
        targets += sink.Type();
    }

    fprintf(stderr, "Redémarrage du logging...\n");
    // Variable intermédiaire pour éviter le "most vexing parse"
    cmd::types::DataTarget target_mask(targets);
    cmd::LogStart cmdStart(target_mask);
    err = ctrl->DoCmd(cmdStart);
    if (!err.IsNone()) {
        fprintf(stderr, "Avertissement : redémarrage du logging échoué : %s\n",
                err.AsString().c_str());
    }
}

// ---------------------------------------------------------------------------
// cmd_list
// ---------------------------------------------------------------------------

int cmd_list(const std::string& device)
{
    // 1. Créer le contrôleur (LCT_Universal → supporte Gen2 et Gen3)
    auto ctrl = LoggerClient::CreateCtrl(LoggerClient::LCT_Universal);

    // 2. Connexion
    fprintf(stderr, "Connexion à '%s'...\n", device.c_str());
    auto err = ctrl->Connect(device);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : connexion à '%s' échouée : %s\n",
                device.c_str(), err.AsString().c_str());
        return 1;
    }
    fprintf(stderr, "Connecté.\n");

    // 3. Arrêter le logging si nécessaire
    bool had_to_stop = false;
    if (!stop_logging_if_needed(ctrl, had_to_stop)) {
        ctrl->Disconnect();
        return 1;
    }

    // 4. Lire le répertoire HDD
    cmd::HddDirMeasurement cmdDir;
    err = ctrl->DoCmd(cmdDir);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : lecture du répertoire HDD échouée : %s\n",
                err.AsString().c_str());
        if (had_to_stop) restart_logging(ctrl);
        ctrl->Disconnect();
        return 1;
    }

    const hdd::MeasurementList& list = cmdDir.Entries();
    size_t count = list.entries();

    if (count == 0) {
        printf("Logger '%s' : aucune mesure sur l'HDD.\n", device.c_str());
        if (had_to_stop) restart_logging(ctrl);
        ctrl->Disconnect();
        return 0;
    }

    // 5. Afficher le tableau
    printf("\nLogger : %s  (%zu mesure(s))\n\n", device.c_str(), count);
    printf(" %3s  %-19s  %-19s  %10s  %9s  %s\n",
           "#", "Début (UTC)", "Fin (UTC)", "Messages", "Taille", "Type");
    printf(" %s\n", std::string(82, '-').c_str());

    HiResDateTime ts;
    for (size_t i = 0; i < count; ++i) {
        const hdd::Measurement& m = list.get(i);

        ts.setTime(m.GetTimeStartHiRes());
        std::string t_start = ts.toString(HiResDateTime::TF_DateTime);

        ts.setTime(m.GetTimeEndHiRes());
        std::string t_end = ts.toString(HiResDateTime::TF_DateTime);

        const char* type_str = m.IsMainMeasurement() ? "Main" : "Snapshot";

        printf(" %3zu  %-19s  %-19s  %10llu  %9s  %s\n",
               i,
               t_start.c_str(),
               t_end.c_str(),
               (unsigned long long)m.GetMsgCnt(),
               format_size(m.GetDataCnt()).c_str(),
               type_str);
    }
    printf("\n");

    // 6. Redémarrer le logging si on l'a arrêté
    if (had_to_stop) restart_logging(ctrl);

    // 7. Déconnecter
    ctrl->Disconnect();
    return 0;
}

// ===========================================================================
// Helpers de suppression — Phase 4
// ===========================================================================

/**
 * Supprime une liste de mesures Gen2 via HddDeleteMeasurement.
 * Appelé après un download réussi, ou directement par cmd_delete.
 *
 * @param ctrl         Contrôleur connecté (logging déjà arrêté)
 * @param to_delete    Vecteur des mesures à supprimer
 * @return             0 si succès, 1 si erreur SDK
 */
static int delete_gen2(LoggerCtrl& ctrl,
                       const std::vector<hdd::Measurement>& to_delete)
{
    // Construire la MeasurementList attendue par HddDeleteMeasurement
    hdd::MeasurementList del_list;
    for (const auto& m : to_delete)
        del_list.add(m);

    cmd::HddDeleteMeasurement cmdDel;
    cmdDel.Measurements(del_list);

    auto err = ctrl->DoCmd(cmdDel);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : suppression échouée : %s\n",
                err.AsString().c_str());
        return 1;
    }
    return 0;
}

/**
 * Supprime une mesure Gen3 via HddDeleteFinalMeasurement (plage temporelle).
 * Gen3 n'expose pas de suppression par liste — on supprime par Begin/End.
 * On appelle DoCmd une fois par mesure.
 *
 * @param ctrl         Contrôleur connecté
 * @param to_delete    Vecteur des mesures à supprimer
 * @return             0 si succès, 1 si au moins une suppression échoue
 */
static int delete_gen3(LoggerCtrl& ctrl,
                       const std::vector<hdd::FinalMeasurement>& to_delete)
{
    int rc = 0;
    HiResDateTime dt_start, dt_end;

    for (const auto& m : to_delete) {
        dt_start.setTime(m.GetTimeStartHiRes());
        dt_end.setTime(m.GetTimeEndHiRes());

        cmd::HddDeleteFinalMeasurement cmdDel;
        cmdDel.Begin(dt_start);
        cmdDel.End(dt_end);

        auto err = ctrl->DoCmd(cmdDel);
        if (!err.IsNone()) {
            fprintf(stderr, "Erreur : suppression mesure [%s → %s] échouée : %s\n",
                    dt_start.toString(HiResDateTime::TF_DateTime).c_str(),
                    dt_end.toString(HiResDateTime::TF_DateTime).c_str(),
                    err.AsString().c_str());
            rc = 1;
        }
    }
    return rc;
}

// ===========================================================================
// cmd_download — Phase 3
// ===========================================================================

// ---------------------------------------------------------------------------
// Callback de progression pour engine::Copy (Gen3)
// ---------------------------------------------------------------------------

namespace {

class CopyProgress final : public engine::Copy::ICallBackHandler
{
public:
    void HandleReportCopy(float pct_total,
                          timespanLowRes::type_t eta_sec,
                          uint32_t mbit_s) override
    {
        printf("\r  %5.1f%%  %4u Mbit/s  ETA : %ds   ",
               pct_total, mbit_s, (int)eta_sec);
        fflush(stdout);
    }

    void HandleFileStarting(engine::FileItem& file) override
    {
        printf("\n  → %s  ", file.dst_path.c_str());
        fflush(stdout);
    }

    void HandleFileEnded(engine::FileItem& /*file*/,
                         uint32_t avg_mbit,
                         engine::Copy::CopyState state) override
    {
        if (state == engine::Copy::CopyState::Done) {
            printf("\r  ✓  %u Mbit/s moy.%30s\n", avg_mbit, "");
        } else {
            printf("\r  ✗  Erreur de copie (état %d)%20s\n",
                   static_cast<int>(state), "");
        }
        fflush(stdout);
    }

    void HandleDoneCopy(engine::Copy::CopyState /*state*/) override
    {
        printf("  Copie terminée.\n");
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Gen2 : HddGetMeasurement + WriterMf4
// Miroir de XorayaConnection::onDownloadEntries() — même filtre réutilisé,
// même boucle callAgain()/finalizeShadow(), même détection de fin via
// IsReceiving() + IsDataConsuming().
// ---------------------------------------------------------------------------

static int download_gen2(LoggerCtrl& ctrl,
                          const std::string& device_name,
                          const std::string& dest_dir,
                          int index,
                          bool delete_after)
{
    cmd::HddDirMeasurement hdd_dir;
    hdd_dir.enableShadow(); // merge stream_queue + default_queue into one measurement (prevents extra AT blocks in MF4)
    auto err = ctrl->DoCmd(hdd_dir);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : HddDirMeasurement échoué : %s\n",
                err.AsString().c_str());
        return 1;
    }

    const hdd::MeasurementList& all = hdd_dir.Entries();
    size_t count = all.entries();

    if (count == 0) {
        printf("Aucune mesure disponible.\n");
        return 0;
    }

    // 2. Sélection des mesures
    std::vector<hdd::Measurement> targets;
    if (index < 0) {
        targets.reserve(count);
        for (size_t i = 0; i < count; ++i)
            targets.push_back(all.get(i));
        printf("Téléchargement de %zu mesure(s)...\n", count);
    } else {
        if (static_cast<size_t>(index) >= count) {
            fprintf(stderr, "Erreur : index %d hors bornes (0..%zu)\n",
                    index, count - 1);
            return 1;
        }
        targets.push_back(all.get(static_cast<size_t>(index)));
        printf("Téléchargement de la mesure %d...\n", index);
    }

    // 3. Créer le filtre UNE FOIS avant la boucle, AddFilter avant SetProperty.
    //    XorayaConnection : lrx->ClearFilter() + AddFilter() hors boucle,
    //    puis SetProperty("Filename") mis à jour pour chaque mesure dans la boucle.
    LoggerDataRecv lrx = ctrl->GetRecv();
    lrx->ClearFilter();
    LoggerDataRecvFilter filter = MsgFilterFactory::Create(MsgFilterFactory::WriterMf4);
    lrx->AddFilter(filter);

    uint64_t total_rx_all  = 0;  // pour la vitesse moyenne globale
    auto     t_global_start = std::chrono::steady_clock::now();

    std::vector<hdd::Measurement> downloaded;

    for (size_t i = 0; i < targets.size(); ++i) {
        if (g_abort) break;

        const hdd::Measurement& m = targets[i];
        bool is_snapshot = !m.IsMainMeasurement();

        // Nom de fichier construit manuellement (YYYYMMDD_HHmmss)
        std::string base_path = dest_dir + "/" + device_name
                                + "_" + format_ts(m.GetTimeStartHiRes())
                                + "-" + format_ts(m.GetTimeEndHiRes())
                                + (is_snapshot ? "_snapshot" : "");

        printf("\n  → Mesure %zu\n", (index < 0 ? i : static_cast<size_t>(index)));

        // SetProperty APRÈS AddFilter — identique à XorayaConnection
        filter->SetProperty("Filename",       base_path);
        filter->SetProperty("MaxFileSize",    "52428800");
        filter->SetProperty("SplitOnMaxSize", "true");
        filter->SetProperty("Extension",      "mf4");

        // Boucle callAgain() — miroir exact de XorayaConnection::onDownloadEntries()
        cmd::HddGetMeasurement cmdGet;
        cmdGet.Measurement(m);

        bool call_again = true;
        while (call_again && !g_abort) {
            Util::ErrorHdl errH = ctrl->DoCmd(cmdGet);
            if (!errH.IsNone()) {
                fprintf(stderr, "Erreur : HddGetMeasurement échoué : %s\n",
                        errH.AsString().c_str());
                return 1;
            }

            // Attente de fin : IsReceiving() || IsDataConsuming() → false
            // XorayaConnection utilise msleep(1000) ; on utilise 200 ms
            // pour être plus réactif au Ctrl+C tout en restant fonctionnel.
            uint64_t last_rx = 0;
            while (!g_abort) {
                usleep(200000);

                bool receiving = ctrl->IsReceiving();
                bool consuming = ctrl->GetCnt(ILoggerCtrl::CId_IsDataConsuming) != 0;

                uint64_t bytes_rx  = ctrl->GetCnt(ILoggerCtrl::CId_BytesRx);
                uint64_t bytes_max = ctrl->GetCnt(ILoggerCtrl::CId_BytesRx_Max);

                if (bytes_max > 0) {
                    double pct   = (double)bytes_rx / (double)bytes_max * 100.0;
                    double mbit  = (double)(bytes_rx - last_rx) * 8.0 / 1e6 / 0.2;
                    printf("\r  %5.1f%%  %4.0f Mbit/s   ", pct, mbit);
                    fflush(stdout);
                }
                last_rx = bytes_rx;

                if (!receiving && !consuming) break;
            }

            if (g_abort) {
                // Arrêt propre côté logger
                cmd::HddStop cmdStop;
                ctrl->DoCmd(cmdStop);
                break;
            }

            uint64_t final_rx  = ctrl->GetCnt(ILoggerCtrl::CId_BytesRx);
            uint64_t final_max = ctrl->GetCnt(ILoggerCtrl::CId_BytesRx_Max);
            total_rx_all += final_rx;
            printf("\r  ✓  %llu / %llu octets%30s\n",
                   (unsigned long long)final_rx,
                   (unsigned long long)final_max, "");

            // callAgain() / finalizeShadow() — identique à XorayaConnection
            call_again = cmdGet.callAgain();
            if (!call_again && m.HasShadow()) {
                cmdGet.finalizeShadow();
            }
        }

        if (g_abort) break;

        downloaded.push_back(m);

        // Patch DLC : libxorayasdk 1.00.0046 écrit 0x00 au byte 4 de CAN_DataFrame
        // (DLC_field) dans tous les enregistrements fixes. Byte 5 (flag2) contient
        // la valeur correcte — on copie byte5 → byte4 pour chaque fichier split.
        {
            namespace fs = std::filesystem;
            const std::string base_fname = fs::path(base_path).filename().string();
            int patched_files = 0, total_dlc = 0;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dest_dir, ec)) {
                if (!e.is_regular_file()) continue;
                const std::string fn = e.path().filename().string();
                if (fn.size() <= base_fname.size() + 1) continue;
                if (fn.compare(0, base_fname.size(), base_fname) != 0) continue;
                if (fn[base_fname.size()] != '_') continue;
                if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".mf4") != 0) continue;
                int n = patch_mf4_dlc(e.path().string());
                if (n >= 0) { total_dlc += n; ++patched_files; }
            }
            if (patched_files > 0)
                printf("  [patch_dlc] %d fichier(s) corrigé(s), %d champ(s) DLC\n",
                       patched_files, total_dlc);
        }
    }

    // Vitesse moyenne globale
    if (!downloaded.empty() && total_rx_all > 0) {
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_global_start).count();
        double avg_mbit = (elapsed > 0.0)
                          ? (double)total_rx_all * 8.0 / 1e6 / elapsed
                          : 0.0;
        printf("  Vitesse moyenne globale : %.0f Mbit/s\n", avg_mbit);
    }

    if (g_abort) return 1;

    // Suppression post-download
    if (delete_after && !downloaded.empty()) {
        printf("\n⚠  Suppression de %zu mesure(s) sur le logger...\n", downloaded.size());
        int rc_del = delete_gen2(ctrl, downloaded);
        if (rc_del != 0) {
            fprintf(stderr, "Erreur : suppression échouée après download réussi.\n");
            fprintf(stderr, "         Les fichiers téléchargés sont intacts dans '%s'.\n",
                    dest_dir.c_str());
            return 1;
        }
        printf("   Suppression terminée.\n");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Gen3 : engine::Copy (SelfOwned) + HddDirFinalMeasurement
// Référence : samples Xoraya/download_all/download_all.cpp → copy_gen3()
// ---------------------------------------------------------------------------

static int copy_gen3(LoggerCtrl& ctrl,
                      const std::string& dest_dir,
                      int index,
                      bool delete_after)
{
    // 1. Lire le répertoire HDD (Gen3 : FinalMeasurement)
    cmd::HddDirFinalMeasurement hdd_dir;
    auto err = ctrl->DoCmd(hdd_dir);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : HddDirFinalMeasurement échoué : %s\n",
                err.AsString().c_str());
        return 1;
    }

    const hdd::FinalMeasurementList& all = hdd_dir.Entries();
    size_t count = all.entries();

    if (count == 0) {
        printf("Aucune mesure disponible.\n");
        return 0;
    }

    // 2. Sélectionner les mesures à copier
    std::vector<hdd::FinalMeasurement> targets;
    if (index < 0) {
        targets.reserve(count);
        for (size_t i = 0; i < count; ++i)
            targets.push_back(all.get(i));
        printf("Copie de %zu mesure(s)...\n", count);
    } else {
        if (static_cast<size_t>(index) >= count) {
            fprintf(stderr, "Erreur : index %d hors bornes (0..%zu)\n",
                    index, count - 1);
            return 1;
        }
        targets.push_back(all.get(static_cast<size_t>(index)));
        printf("Copie de la mesure %d...\n", index);
    }

    // 3. Configurer l'engine de copie
    engine::Copy cp;
    cp.setLCtrl(ctrl);
    cp.setMeasurements(targets);
    cp.SetCopyMode(engine::Copy::CopyMode::SelfOwned);
    cp.SetTargetPath(dest_dir);
    cp.setCallBackIntervallReport(500);

    CopyProgress cb;
    cp.SetCallBackHandler(&cb);

    // 4. Lancer la copie
    if (!cp.Run()) {
        fprintf(stderr, "Erreur : engine::Copy::Run() échoué\n");
        return 1;
    }

    auto state = cp.WaitForEndOfCopy();
    if (state != engine::Copy::CopyState::Done) {
        fprintf(stderr, "Erreur : copie terminée avec état %d\n",
                static_cast<int>(state));
        return 1;
    }

    // Suppression post-copie — uniquement si demandée et copie réussie
    if (delete_after) {
        printf("\n⚠  Suppression de %zu mesure(s) sur le logger...\n", targets.size());
        int rc_del = delete_gen3(ctrl, targets);
        if (rc_del != 0) {
            fprintf(stderr, "Erreur : suppression échouée après copie réussie.\n");
            fprintf(stderr, "         Les fichiers copiés sont intacts dans '%s'.\n",
                    dest_dir.c_str());
            return 1;
        }
        printf("   Suppression terminée.\n");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Point d'entrée public
// ---------------------------------------------------------------------------

int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index,
                 bool delete_after,
                 bool stop_logging)
{
    // 1. Créer le répertoire de destination si nécessaire
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        fprintf(stderr, "Erreur : impossible de créer '%s' : %s\n",
                dest_dir.c_str(), ec.message().c_str());
        return 1;
    }

    // 2. Connexion
    auto ctrl = LoggerClient::CreateCtrl(LoggerClient::LCT_Universal);
    fprintf(stderr, "Connexion à '%s'...\n", device.c_str());
    auto err = ctrl->Connect(device);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : connexion à '%s' échouée : %s\n",
                device.c_str(), err.AsString().c_str());
        return 1;
    }

    // Récupérer le nom réel du logger (ex: "DX11242-1") pour le nommage des fichiers.
    // GetName() retourne le nom alias exposé par le SDK, indépendamment de l'identifiant
    // passé à Connect() (IP ou nom réseau). Fallback sur device si l'appel échoue.
    std::string logger_name = device;
    {
        std::string tmp;
        auto err_name = ctrl->GetName(tmp);
        if (err_name == x2e::X2Error::NoError() && !tmp.empty())
            logger_name = tmp;
        else
            fprintf(stderr, "Avertissement : GetName() échoué (code %d), utilisation de '%s'\n",
                    static_cast<int32_t>(err_name), device.c_str());
    }

    fprintf(stderr, "Connecté. Type : ");

    // 3. Arrêt optionnel du logging (--stop-logging)
    bool had_to_stop = false;
    if (stop_logging) {
        if (!stop_logging_if_needed(ctrl, had_to_stop)) {
            ctrl->Disconnect();
            return 1;
        }
    }

    // 4. Dispatch Gen2 / Gen3 selon le type détecté après connexion
    int rc = 0;
    using TypeId = connection::ConnectionType::TypeId;

    switch (ctrl->ConnectionType()) {
        case TypeId::Generation_2:
        case TypeId::DataCube:
        case TypeId::DLNcluster:
            fprintf(stderr, "Gen2 (engine::Download + MF4)\n");
            if (delete_after)
                fprintf(stderr, "Mode : téléchargement + suppression après succès\n");
            rc = download_gen2(ctrl, logger_name, dest_dir, index, delete_after);
            break;

        case TypeId::Generation_3:
        case TypeId::DataCubeNSeries:
            fprintf(stderr, "Gen3 (engine::Copy)\n");
            if (delete_after)
                fprintf(stderr, "Mode : copie + suppression après succès\n");
            rc = copy_gen3(ctrl, dest_dir, index, delete_after);
            break;

        default:
            fprintf(stderr, "non supporté.\n");
            fprintf(stderr, "Erreur : type de device non supporté pour le téléchargement.\n");
            fprintf(stderr, "         Devices supportés : Gen2, DataCube, DLNcluster, Gen3, DataCubeNSeries.\n");
            rc = 1;
            break;
    }

    if (stop_logging && had_to_stop)
        restart_logging(ctrl);

    ctrl->Disconnect();
    if (rc == 0)
        printf("\nFichiers disponibles dans : %s\n", dest_dir.c_str());
    return rc;
}

// ===========================================================================
// cmd_delete — Phase 4 : suppression manuelle ciblée
// ===========================================================================

int cmd_delete(const std::string& device, int index)
{
    if (index < 0) {
        fprintf(stderr, "Erreur : index invalide (%d). Un entier >= 0 est requis.\n", index);
        return 1;
    }

    // 1. Connexion
    auto ctrl = LoggerClient::CreateCtrl(LoggerClient::LCT_Universal);
    fprintf(stderr, "Connexion à '%s'...\n", device.c_str());
    auto err = ctrl->Connect(device);
    if (!err.IsNone()) {
        fprintf(stderr, "Erreur : connexion à '%s' échouée : %s\n",
                device.c_str(), err.AsString().c_str());
        return 1;
    }

    int rc = 0;
    using TypeId = connection::ConnectionType::TypeId;

    switch (ctrl->ConnectionType()) {

        // ---- Gen2 : HddDeleteMeasurement ----
        case TypeId::Generation_2:
        case TypeId::DataCube:
        case TypeId::DLNcluster: {
            // Arrêter le logging pour accéder à l'HDD
            bool had_to_stop = false;
            if (!stop_logging_if_needed(ctrl, had_to_stop)) {
                ctrl->Disconnect();
                return 1;
            }

            // Lire le répertoire
            cmd::HddDirMeasurement hdd_dir;
            hdd_dir.enableShadow();
            err = ctrl->DoCmd(hdd_dir);
            if (!err.IsNone()) {
                fprintf(stderr, "Erreur : HddDirMeasurement échoué : %s\n",
                        err.AsString().c_str());
                if (had_to_stop) restart_logging(ctrl);
                ctrl->Disconnect();
                return 1;
            }

            const hdd::MeasurementList& all = hdd_dir.Entries();
            size_t count = all.entries();

            if (static_cast<size_t>(index) >= count) {
                fprintf(stderr, "Erreur : index %d hors bornes (0..%zu)\n",
                        index, count - 1);
                if (had_to_stop) restart_logging(ctrl);
                ctrl->Disconnect();
                return 1;
            }

            printf("⚠  Suppression de la mesure %d sur '%s'...\n",
                   index, device.c_str());

            std::vector<hdd::Measurement> target = { all.get(static_cast<size_t>(index)) };
            rc = delete_gen2(ctrl, target);

            if (rc == 0)
                printf("   Mesure %d supprimée.\n", index);

            if (had_to_stop) restart_logging(ctrl);
            break;
        }

        // ---- Gen3 : HddDeleteFinalMeasurement (plage temporelle) ----
        case TypeId::Generation_3:
        case TypeId::DataCubeNSeries: {
            cmd::HddDirFinalMeasurement hdd_dir;
            err = ctrl->DoCmd(hdd_dir);
            if (!err.IsNone()) {
                fprintf(stderr, "Erreur : HddDirFinalMeasurement échoué : %s\n",
                        err.AsString().c_str());
                ctrl->Disconnect();
                return 1;
            }

            const hdd::FinalMeasurementList& all = hdd_dir.Entries();
            size_t count = all.entries();

            if (static_cast<size_t>(index) >= count) {
                fprintf(stderr, "Erreur : index %d hors bornes (0..%zu)\n",
                        index, count - 1);
                ctrl->Disconnect();
                return 1;
            }

            printf("⚠  Suppression de la mesure %d sur '%s'...\n",
                   index, device.c_str());

            std::vector<hdd::FinalMeasurement> target = { all.get(static_cast<size_t>(index)) };
            rc = delete_gen3(ctrl, target);

            if (rc == 0)
                printf("   Mesure %d supprimée.\n", index);
            break;
        }

        default:
            fprintf(stderr, "Erreur : type de device non supporté pour la suppression.\n");
            rc = 1;
            break;
    }

    ctrl->Disconnect();
    return rc;
}
