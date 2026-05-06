/**
 * downloader.cpp — Xoraya measurement listing and download
 *
 * References:
 *   XorayaUtils/XorayaConnection.cpp  → list / stop / restart logic
 *   samples Xoraya/download_all/      → engine::Download and engine::Copy
 *
 * Qt → pure C++ port:
 *   QString         → std::string
 *   QVector         → std::vector
 *   qDebug()        → fprintf(stderr, ...)
 *   QThread::msleep → std::this_thread::sleep_for  (unused here)
 *
 * Note: do not define X2E_XorayaWin32_DYN_LINKING_AUTO.
 *       Link via -lxorayasdk in the Makefile.
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
// Cancel ongoing download (Ctrl+C from collect)
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_abort = 0;

void abort_downloads()
{
    g_abort = 1;
    // The polling loop in download_gen2() detects g_abort and sends HddStop
}

// ---------------------------------------------------------------------------
// Local helpers
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
 * Formats an HiRes SDK timestamp as "YYYYMMDD_HHmmss" (UTC).
 * HiResDateTime::getTime() → 100 ns intervals since Unix epoch.
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
 * Fixes the DLC field (byte 4 of CAN_DataFrame) in every fixed record of an
 * MF4 file produced by libxorayasdk 1.00.0046.
 *
 * SDK bug: X2eToMdfConverter writes 0x00 at byte 4 (DLC_field) but correctly
 * writes the value at byte 5 (flag2). The patch copies byte5 → byte4.
 *
 * Operates in memory (full read + rewrite); no temporary file.
 * Returns the number of corrections applied, or -1 on error.
 */
static int patch_mf4_dlc(const std::string& path)
{
    // --- Full file load ---
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) {
        fprintf(stderr, "  [patch_dlc] Cannot open '%s'\n", path.c_str());
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 64) { fclose(f); return -1; }

    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    if (static_cast<long>(fread(buf.data(), 1, static_cast<size_t>(fsize), f)) != fsize) {
        fclose(f);
        fprintf(stderr, "  [patch_dlc] Read failed: '%s'\n", path.c_str());
        return -1;
    }

    // MF4 ID block starts with "MDF     " (8 bytes), not "##ID"
    if (fsize < 8 || memcmp(buf.data(), "MDF     ", 8) != 0) {
        fclose(f);
        fprintf(stderr, "  [patch_dlc] Invalid MF4 file: '%s'\n", path.c_str());
        return -1;
    }

    const size_t fsz = static_cast<size_t>(fsize);

    // Bounded little-endian read helpers
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
    // Reads an MF4 link offset (int64, 0 = null)
    auto link_at = [&](size_t o) -> size_t {
        int64_t v; if (o + 8 > fsz) return 0;
        memcpy(&v, buf.data() + o, 8);
        return (v > 0) ? static_cast<size_t>(v) : 0;
    };

    // Each MF4 block: id(4) + res(4) + len(8) + lnk_cnt(8) = 24-byte header
    // Links: lnk_cnt * 8 bytes
    // Data: starting at off + 24 + lnk_cnt * 8

    // ##HD always at offset 64; links[0] = first ##DG
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

        // --- CG table: rec_id → {is_vlsd, data_bytes} ---
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

        // --- Collect DT blocks (direct or via DL) ---
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

        // --- Scan records and patch ---
        // Fixed record (30 bytes):
        //   rec_id(2) + timestamp(8) + Async(1) + CAN_DataFrame(19)
        //   CAN_DataFrame byte 4 (offset 15) = DLC_field (0x00 bug) ← target
        //   CAN_DataFrame byte 5 (offset 16) = flag2 (correct DLC)  ← source

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

    // Rewrite if corrections were applied
    if (total_patches > 0) {
        rewind(f);
        if (fwrite(buf.data(), 1, fsz, f) != fsz) {
            fclose(f);
            fprintf(stderr, "  [patch_dlc] Write failed: '%s'\n", path.c_str());
            return -1;
        }
    }

    fclose(f);
    return total_patches;
}

/**
 * Stops logging if the logger is currently recording.
 * Saves the active DataSink targets before stopping so they can be
 * restored by restart_logging() — the targets list is empty after a stop.
 *
 * @param ctrl           Connected controller
 * @param had_to_stop    [out] true if logging was actually stopped
 * @param saved_targets  [out] bitmask of active DataSink types before the stop
 * @return               true on success (or already stopped), false on SDK error
 */
static bool stop_logging_if_needed(LoggerCtrl& ctrl,
                                   bool& had_to_stop,
                                   uint32_t& saved_targets)
{
    had_to_stop    = false;
    saved_targets  = 0;

    cmd::LogState logStateCmd;
    auto err = ctrl->DoCmd(logStateCmd);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: cannot read logger state: %s\n",
                err.AsString().c_str());
        return false;
    }

    // Already stopped → nothing to do
    if (logStateCmd.isStopped() == XBool::True) {
        return true;
    }

    // Save active targets BEFORE stopping — they are unavailable afterwards
    for (const auto& sink : logStateCmd.DataSink()) {
        saved_targets += sink.Type();
    }

    fprintf(stderr, "Stopping logging...\n");

    // Stop logging on HDD storage only
    // (mirrors XorayaConnection::getHddMeasurementList)
    cmd::LogStop cmdStop(cmd::LogStop::Force::Skip_Pending_Data);
    cmdStop.Set(cmd::types::DataTarget::TypeId::LoggerStorage);
    err = ctrl->DoCmd(cmdStop);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: failed to stop logging: %s\n",
                err.AsString().c_str());
        return false;
    }

    had_to_stop = true;
    return true;
}

/**
 * Restarts HDD logging after a stop_logging_if_needed() call.
 * Mirrors the stop: only LoggerStorage is restarted since that is the
 * only target we stop (LogStop::Set(LoggerStorage)).
 *
 * @param ctrl  Connected controller
 */
static void restart_logging(LoggerCtrl& ctrl, uint32_t /*saved_targets*/)
{
    fprintf(stderr, "Restarting logging...\n");
    cmd::types::DataTarget target(cmd::types::DataTarget::TypeId::LoggerStorage);
    cmd::LogStart cmdStart(target);
    auto err = ctrl->DoCmd(cmdStart);
    if (!err.IsNone()) {
        fprintf(stderr, "Warning: failed to restart logging: %s\n",
                err.AsString().c_str());
    }
}

// ---------------------------------------------------------------------------
// cmd_list
// ---------------------------------------------------------------------------

int cmd_list(const std::string& device)
{
    auto ctrl = LoggerClient::CreateCtrl(LoggerClient::LCT_Universal);

    // 2. Connect
    fprintf(stderr, "Connecting to '%s'...\n", device.c_str());
    auto err = ctrl->Connect(device);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: connection to '%s' failed: %s\n",
                device.c_str(), err.AsString().c_str());
        return 1;
    }
    fprintf(stderr, "Connected.\n");

    // 3. Stop logging if needed
    bool     had_to_stop   = false;
    uint32_t saved_targets = 0;
    if (!stop_logging_if_needed(ctrl, had_to_stop, saved_targets)) {
        ctrl->Disconnect();
        return 1;
    }

    // 4. Read HDD directory
    cmd::HddDirMeasurement cmdDir;
    err = ctrl->DoCmd(cmdDir);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: HDD directory read failed: %s\n",
                err.AsString().c_str());
        if (had_to_stop) restart_logging(ctrl, saved_targets);
        ctrl->Disconnect();
        return 1;
    }

    const hdd::MeasurementList& list = cmdDir.Entries();
    size_t count = list.entries();

    if (count == 0) {
        printf("Logger '%s': no measurement on HDD.\n", device.c_str());
        if (had_to_stop) restart_logging(ctrl, saved_targets);
        ctrl->Disconnect();
        return 0;
    }

    // 5. Display table
    printf("\nLogger: %s  (%zu measurement(s))\n\n", device.c_str(), count);
    printf(" %3s  %-19s  %-19s  %10s  %9s  %s\n",
           "#", "Start (UTC)", "End (UTC)", "Messages", "Size", "Type");
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

    // 6. Restart logging if it was stopped
    if (had_to_stop) restart_logging(ctrl, saved_targets);

    // 7. Disconnect
    ctrl->Disconnect();
    return 0;
}

// ===========================================================================
// Helpers de suppression — Phase 4
// ===========================================================================

/**
 * Deletes a list of Gen2 measurements via HddDeleteMeasurement.
 * Called after a successful download, or directly by cmd_delete.
 *
 * @param ctrl         Connected controller (logging already stopped)
 * @param to_delete    Vector of measurements to delete
 * @return             0 on success, 1 on SDK error
 */
static int delete_gen2(LoggerCtrl& ctrl,
                       const std::vector<hdd::Measurement>& to_delete)
{
    // Build the MeasurementList expected by HddDeleteMeasurement
    hdd::MeasurementList del_list;
    for (const auto& m : to_delete)
        del_list.add(m);

    cmd::HddDeleteMeasurement cmdDel;
    cmdDel.Measurements(del_list);

    auto err = ctrl->DoCmd(cmdDel);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: deletion failed: %s\n",
                err.AsString().c_str());
        return 1;
    }
    return 0;
}

/**
 * Deletes Gen3 measurements via HddDeleteFinalMeasurement (time range).
 * Gen3 does not expose list-based deletion — deletion is by Begin/End.
 * DoCmd is called once per measurement.
 *
 * @param ctrl         Connected controller
 * @param to_delete    Vector of measurements to delete
 * @return             0 on success, 1 if at least one deletion fails
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
            fprintf(stderr, "Error: deletion of measurement [%s → %s] failed: %s\n",
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
// Gen2: HddGetMeasurement + WriterMf4
// Mirrors XorayaConnection::onDownloadEntries() — same reused filter,
// same callAgain()/finalizeShadow() loop, same end detection via
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
        fprintf(stderr, "Error: HddDirMeasurement failed: %s\n",
                err.AsString().c_str());
        return 1;
    }

    const hdd::MeasurementList& all = hdd_dir.Entries();
    size_t count = all.entries();

    if (count == 0) {
        printf("No measurement available.\n");
        return 0;
    }

    // 2. Select measurements
    std::vector<hdd::Measurement> targets;
    if (index < 0) {
        targets.reserve(count);
        for (size_t i = 0; i < count; ++i)
            targets.push_back(all.get(i));
        printf("Downloading %zu measurement(s)...\n", count);
    } else {
        if (static_cast<size_t>(index) >= count) {
            fprintf(stderr, "Error: index %d out of range (0..%zu)\n",
                    index, count - 1);
            return 1;
        }
        targets.push_back(all.get(static_cast<size_t>(index)));
        printf("Downloading measurement %d...\n", index);
    }

    // 3. Create the filter ONCE before the loop, AddFilter before SetProperty.
    //    XorayaConnection: lrx->ClearFilter() + AddFilter() outside the loop,
    //    then SetProperty("Filename") updated for each measurement inside the loop.
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

        printf("\n  → Measurement %zu\n", (index < 0 ? i : static_cast<size_t>(index)));

        // SetProperty AFTER AddFilter — identical to XorayaConnection
        filter->SetProperty("Filename",       base_path);
        filter->SetProperty("MaxFileSize",    "52428800");
        filter->SetProperty("SplitOnMaxSize", "true");
        filter->SetProperty("Extension",      "mf4");

        // callAgain() loop — exact mirror of XorayaConnection::onDownloadEntries()
        cmd::HddGetMeasurement cmdGet;
        cmdGet.Measurement(m);

        bool call_again = true;
        while (call_again && !g_abort) {
            Util::ErrorHdl errH = ctrl->DoCmd(cmdGet);
            if (!errH.IsNone()) {
                fprintf(stderr, "Error: HddGetMeasurement failed: %s\n",
                        errH.AsString().c_str());
                return 1;
            }

            // Wait for completion: IsReceiving() || IsDataConsuming() → false
            // XorayaConnection uses msleep(1000); we use 200 ms
            // for better Ctrl+C responsiveness while remaining functional.
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
            printf("\r  ✓  %llu / %llu bytes%30s\n",
                   (unsigned long long)final_rx,
                   (unsigned long long)final_max, "");

            // callAgain() / finalizeShadow() — identical to XorayaConnection
            call_again = cmdGet.callAgain();
            if (!call_again && m.HasShadow()) {
                cmdGet.finalizeShadow();
            }
        }

        if (g_abort) break;

        downloaded.push_back(m);

        // DLC patch: libxorayasdk 1.00.0046 writes 0x00 at byte 4 of CAN_DataFrame
        // (DLC_field) in all fixed records. Byte 5 (flag2) contains the correct
        // value — copy byte5 → byte4 for each split file.
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
                printf("  [patch_dlc] %d file(s) patched, %d DLC field(s) corrected\n",
                       patched_files, total_dlc);
        }
    }

    // Overall average speed
    if (!downloaded.empty() && total_rx_all > 0) {
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t_global_start).count();
        double avg_mbit = (elapsed > 0.0)
                          ? (double)total_rx_all * 8.0 / 1e6 / elapsed
                          : 0.0;
        printf("  Overall average speed: %.0f Mbit/s\n", avg_mbit);
    }

    if (g_abort) return 1;

    // Post-download deletion
    if (delete_after && !downloaded.empty()) {
        printf("\n⚠  Deleting %zu measurement(s) from the logger...\n", downloaded.size());
        int rc_del = delete_gen2(ctrl, downloaded);
        if (rc_del != 0) {
            fprintf(stderr, "Error: deletion failed after successful download.\n");
            fprintf(stderr, "       Downloaded files are intact in '%s'.\n",
                    dest_dir.c_str());
            return 1;
        }
        printf("   Deletion complete.\n");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Gen3: engine::Copy (SelfOwned) + HddDirFinalMeasurement
// Reference: samples Xoraya/download_all/download_all.cpp → copy_gen3()
// ---------------------------------------------------------------------------

static int copy_gen3(LoggerCtrl& ctrl,
                      const std::string& dest_dir,
                      int index,
                      bool delete_after)
{
    // 1. Read HDD directory (Gen3: FinalMeasurement)
    cmd::HddDirFinalMeasurement hdd_dir;
    auto err = ctrl->DoCmd(hdd_dir);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: HddDirFinalMeasurement failed: %s\n",
                err.AsString().c_str());
        return 1;
    }

    const hdd::FinalMeasurementList& all = hdd_dir.Entries();
    size_t count = all.entries();

    if (count == 0) {
        printf("No measurement available.\n");
        return 0;
    }

    // 2. Select measurements to copy
    std::vector<hdd::FinalMeasurement> targets;
    if (index < 0) {
        targets.reserve(count);
        for (size_t i = 0; i < count; ++i)
            targets.push_back(all.get(i));
        printf("Copying %zu measurement(s)...\n", count);
    } else {
        if (static_cast<size_t>(index) >= count) {
            fprintf(stderr, "Error: index %d out of range (0..%zu)\n",
                    index, count - 1);
            return 1;
        }
        targets.push_back(all.get(static_cast<size_t>(index)));
        printf("Copying measurement %d...\n", index);
    }

    // 3. Configure the copy engine
    engine::Copy cp;
    cp.setLCtrl(ctrl);
    cp.setMeasurements(targets);
    cp.SetCopyMode(engine::Copy::CopyMode::SelfOwned);
    cp.SetTargetPath(dest_dir);
    cp.setCallBackIntervallReport(500);

    CopyProgress cb;
    cp.SetCallBackHandler(&cb);

    // 4. Run the copy
    if (!cp.Run()) {
        fprintf(stderr, "Error: engine::Copy::Run() failed\n");
        return 1;
    }

    auto state = cp.WaitForEndOfCopy();
    if (state != engine::Copy::CopyState::Done) {
        fprintf(stderr, "Error: copy finished with state %d\n",
                static_cast<int>(state));
        return 1;
    }

    // Post-copy deletion — only if requested and copy succeeded
    if (delete_after) {
        printf("\n⚠  Deleting %zu measurement(s) from the logger...\n", targets.size());
        int rc_del = delete_gen3(ctrl, targets);
        if (rc_del != 0) {
            fprintf(stderr, "Error: deletion failed after successful copy.\n");
            fprintf(stderr, "       Copied files are intact in '%s'.\n",
                    dest_dir.c_str());
            return 1;
        }
        printf("   Deletion complete.\n");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public entry point
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
        fprintf(stderr, "Error: cannot create '%s': %s\n",
                dest_dir.c_str(), ec.message().c_str());
        return 1;
    }

    // 2. Connect
    auto ctrl = LoggerClient::CreateCtrl(LoggerClient::LCT_Universal);
    fprintf(stderr, "Connecting to '%s'...\n", device.c_str());
    auto err = ctrl->Connect(device);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: connection to '%s' failed: %s\n",
                device.c_str(), err.AsString().c_str());
        return 1;
    }

    // Retrieve the real logger alias (e.g. "DX11242-1") for filename construction.
    // GetName() returns the alias exposed by the SDK, regardless of the identifier
    // passed to Connect() (IP or network name). Falls back to device on failure.
    std::string logger_name = device;
    {
        std::string tmp;
        auto err_name = ctrl->GetName(tmp);
        if (err_name == x2e::X2Error::NoError() && !tmp.empty())
            logger_name = tmp;
        else
            fprintf(stderr, "Warning: GetName() failed (code %d), using '%s'\n",
                    static_cast<int32_t>(err_name), device.c_str());
    }

    // 3. Print type on its own complete line before any stop call to avoid
    //    output interleaving with "Stopping logging..." on stderr
    using TypeId = connection::ConnectionType::TypeId;
    const TypeId::type_t conn_type = ctrl->ConnectionType();
    switch (conn_type) {
        case TypeId::Generation_2:
        case TypeId::DataCube:
        case TypeId::DLNcluster:
            fprintf(stderr, "Connected. Type: Gen2 (engine::Download + MF4)\n");
            if (delete_after) fprintf(stderr, "Mode: download + delete after success\n");
            break;
        case TypeId::Generation_3:
        case TypeId::DataCubeNSeries:
            fprintf(stderr, "Connected. Type: Gen3 (engine::Copy)\n");
            if (delete_after) fprintf(stderr, "Mode: copy + delete after success\n");
            break;
        default:
            fprintf(stderr, "Connected. Type: unsupported\n");
            break;
    }

    // 4. Optional logging stop (--stop-logging)
    bool     had_to_stop   = false;
    uint32_t saved_targets = 0;
    if (stop_logging) {
        if (!stop_logging_if_needed(ctrl, had_to_stop, saved_targets)) {
            ctrl->Disconnect();
            return 1;
        }
    }

    // 5. Dispatch Gen2 / Gen3
    int rc = 0;
    switch (conn_type) {
        case TypeId::Generation_2:
        case TypeId::DataCube:
        case TypeId::DLNcluster:
            rc = download_gen2(ctrl, logger_name, dest_dir, index, delete_after);
            break;

        case TypeId::Generation_3:
        case TypeId::DataCubeNSeries:
            rc = copy_gen3(ctrl, dest_dir, index, delete_after);
            break;

        default:
            fprintf(stderr, "Error: unsupported device type for download.\n");
            fprintf(stderr, "       Supported types: Gen2, DataCube, DLNcluster, Gen3, DataCubeNSeries.\n");
            rc = 1;
            break;
    }

    if (stop_logging && had_to_stop)
        restart_logging(ctrl, saved_targets);

    ctrl->Disconnect();
    if (rc == 0)
        printf("\nFiles available in: %s\n", dest_dir.c_str());
    return rc;
}

// ===========================================================================
// cmd_delete — Phase 4: targeted manual deletion
// ===========================================================================

int cmd_delete(const std::string& device, int index)
{
    if (index < 0) {
        fprintf(stderr, "Error: invalid index (%d). An integer >= 0 is required.\n", index);
        return 1;
    }

    // 1. Connect
    auto ctrl = LoggerClient::CreateCtrl(LoggerClient::LCT_Universal);
    fprintf(stderr, "Connecting to '%s'...\n", device.c_str());
    auto err = ctrl->Connect(device);
    if (!err.IsNone()) {
        fprintf(stderr, "Error: connection to '%s' failed: %s\n",
                device.c_str(), err.AsString().c_str());
        return 1;
    }

    int rc = 0;
    using TypeId = connection::ConnectionType::TypeId;

    switch (ctrl->ConnectionType()) {

        // ---- Gen2: HddDeleteMeasurement ----
        case TypeId::Generation_2:
        case TypeId::DataCube:
        case TypeId::DLNcluster: {
            // Stop logging to access the HDD
            bool     had_to_stop   = false;
            uint32_t saved_targets = 0;
            if (!stop_logging_if_needed(ctrl, had_to_stop, saved_targets)) {
                ctrl->Disconnect();
                return 1;
            }

            // Read directory
            cmd::HddDirMeasurement hdd_dir;
            hdd_dir.enableShadow();
            err = ctrl->DoCmd(hdd_dir);
            if (!err.IsNone()) {
                fprintf(stderr, "Error: HddDirMeasurement failed: %s\n",
                        err.AsString().c_str());
                if (had_to_stop) restart_logging(ctrl, saved_targets);
                ctrl->Disconnect();
                return 1;
            }

            const hdd::MeasurementList& all = hdd_dir.Entries();
            size_t count = all.entries();

            if (static_cast<size_t>(index) >= count) {
                fprintf(stderr, "Error: index %d out of range (0..%zu)\n",
                        index, count - 1);
                if (had_to_stop) restart_logging(ctrl, saved_targets);
                ctrl->Disconnect();
                return 1;
            }

            printf("⚠  Deleting measurement %d from '%s'...\n",
                   index, device.c_str());

            std::vector<hdd::Measurement> target = { all.get(static_cast<size_t>(index)) };
            rc = delete_gen2(ctrl, target);

            if (rc == 0)
                printf("   Measurement %d deleted.\n", index);

            if (had_to_stop) restart_logging(ctrl, saved_targets);
            break;
        }

        // ---- Gen3: HddDeleteFinalMeasurement (time range) ----
        case TypeId::Generation_3:
        case TypeId::DataCubeNSeries: {
            cmd::HddDirFinalMeasurement hdd_dir;
            err = ctrl->DoCmd(hdd_dir);
            if (!err.IsNone()) {
                fprintf(stderr, "Error: HddDirFinalMeasurement failed: %s\n",
                        err.AsString().c_str());
                ctrl->Disconnect();
                return 1;
            }

            const hdd::FinalMeasurementList& all = hdd_dir.Entries();
            size_t count = all.entries();

            if (static_cast<size_t>(index) >= count) {
                fprintf(stderr, "Error: index %d out of range (0..%zu)\n",
                        index, count - 1);
                ctrl->Disconnect();
                return 1;
            }

            printf("⚠  Deleting measurement %d from '%s'...\n",
                   index, device.c_str());

            std::vector<hdd::FinalMeasurement> target = { all.get(static_cast<size_t>(index)) };
            rc = delete_gen3(ctrl, target);

            if (rc == 0)
                printf("   Measurement %d deleted.\n", index);
            break;
        }

        default:
            fprintf(stderr, "Error: unsupported device type for deletion.\n");
            rc = 1;
            break;
    }

    ctrl->Disconnect();
    return rc;
}
