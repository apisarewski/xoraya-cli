#include "StatusWriter.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

static const char* PATH     = "/tmp/xoraya-status.json";
static const char* PATH_TMP = "/tmp/xoraya-status.json.tmp";

static std::mutex s_mtx;
static std::string s_dest_dir;   // set by setScanning(), included in all writes

static void write_atomic(const std::string& json)
{
    FILE* f = fopen(PATH_TMP, "w");
    if (!f) return;
    fputs(json.c_str(), f);
    fclose(f);
    rename(PATH_TMP, PATH);
}

static long now_ts()
{
    return static_cast<long>(time(nullptr));
}

static std::string sanitise(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += '\''; break;
            case '\\': out += "\\\\"; break;
            case '\n': case '\r': case '\t': out += ' '; break;
            default:   out += c;
        }
    }
    return out;
}

void StatusWriter::setScanning(const std::string& dest_dir)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_dest_dir = sanitise(dest_dir);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"scanning\","
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::setDevice(const std::string& name, int total_files)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    std::string safe = sanitise(name);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"scanning\","
        "\"device\":\"%s\","
        "\"files_total\":%d,"
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        safe.c_str(), total_files, s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::setProgress(int file_idx, int total,
                               float pct, float speed_mbps, int eta_s)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"downloading\","
        "\"files_done\":%d,\"files_total\":%d,"
        "\"pct\":%.1f,\"speed_mbps\":%.2f,\"eta_s\":%d,"
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        file_idx, total, (double)pct, (double)speed_mbps, eta_s,
        s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::setDone(int files_done, uint64_t bytes_total)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"done\","
        "\"files_done\":%d,\"bytes_total\":%llu,"
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        files_done,
        (unsigned long long)bytes_total,
        s_dest_dir.c_str(),
        now_ts());
    write_atomic(buf);
}

void StatusWriter::setError(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    std::string safe = sanitise(msg);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"error\","
        "\"msg\":\"%s\","
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        safe.c_str(), s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::clear()
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_dest_dir.clear();
    remove(PATH);
}
