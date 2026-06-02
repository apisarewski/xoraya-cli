#pragma once
#include <string>
#include <cstdint>

/**
 * Writes /tmp/xoraya-status.json atomically so the screen daemon
 * can read download progress without locking.
 * All methods are no-throw — write failures are silently ignored.
 */
class StatusWriter {
public:
    static void setScanning(const std::string& dest_dir = "");
    static void setDevice(const std::string& name, int total_files);
    static void setProgress(int file_idx, int total,
                            float pct, float speed_mbps, int eta_s);
    static void setDone(int files_done, uint64_t bytes_total);
    static void setError(const std::string& msg);
    static void clear();
};
