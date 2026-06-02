#include "storage.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

static bool is_mount_point(const std::string& path)
{
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char dev[256], mnt[256];
        if (sscanf(line, "%255s %255s", dev, mnt) == 2) {
            if (path == mnt) {
                found = true;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

std::string detect_dest_dir(std::string& err)
{
    const passwd* pw = getpwuid(getuid());
    if (!pw) {
        err = "could not determine current username";
        return "";
    }
    std::string base = std::string("/media/") + pw->pw_name + "/";

    DIR* dir = opendir(base.c_str());
    if (!dir) {
        err = "no external drive found (could not open " + base + ")";
        return "";
    }

    std::vector<std::string> found;
    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string candidate = base + entry->d_name;
        if (is_mount_point(candidate)) {
            found.push_back(candidate);
        }
    }
    closedir(dir);

    if (found.empty()) {
        err = "no external drive found under " + base +
              "\n       Plug in a USB drive or SSD and try again.";
        return "";
    }
    if (found.size() > 1) {
        err = "multiple external drives found — plug in only one.";
        for (const auto& p : found) err += "\n       Found: " + p;
        return "";
    }
    return found[0] + "/Dexterlogs";
}
