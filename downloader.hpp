#pragma once

#include <string>
#include <cstddef>

/**
 * Lists measurements stored on the HDD of a logger.
 *
 * Sequence:
 *   Connect → LogState → (LogStop if needed) → HddDirMeasurement
 *   → display → (LogStart if stopped) → Disconnect
 *
 * @param device  Logger name or IP (e.g. "XORAYA-001" or "192.168.1.10")
 * @return        0 on success, 1 on error
 */
int cmd_list(const std::string& device);

/**
 * Downloads measurements from a Xoraya logger to a local directory.
 *
 * By default: non-destructive. Post-download deletion is explicitly
 * opt-in via delete_after=true (CLI flag: --delete-after-download).
 *
 * Deletion rules when delete_after=true:
 *   1. Deletion only occurs after a successful download.
 *   2. It is clearly announced in the console.
 *   3. A deletion failure returns an error (even if the download succeeded).
 *
 * @param device        Logger name or IP
 * @param dest_dir      Destination directory (created if absent)
 * @param index         Index of the measurement to download, or -1 for all
 * @param delete_after  false by default — pass true only with --delete-after-download
 * @param stop_logging  false by default — pass true to stop logging during download
 *                      and restart it afterwards (--stop-logging)
 * @return              0 on success, 1 on error
 */
int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index = -1,
                 bool delete_after = false,
                 bool stop_logging = false,
                 int last_n = -1);

/**
 * Deletes a measurement by index from the logger.
 *
 * Separate, explicit command, independent of download.
 * Dispatches to HddDeleteMeasurement (Gen2) or HddDeleteFinalMeasurement (Gen3).
 *
 * @param device  Logger name or IP
 * @param index   Index of the measurement to delete (must exist on the HDD)
 * @return        0 on success, 1 on error
 */
int cmd_delete(const std::string& device, int index);

/**
 * Requests cancellation of the ongoing download (callable from a SIGINT handler).
 *
 * Sets an internal flag and calls ForceCancel() on the active engine::Download,
 * which causes WaitForEndOfDownload() to exit with Abort state.
 * No effect if no download is in progress.
 */
void abort_downloads();
