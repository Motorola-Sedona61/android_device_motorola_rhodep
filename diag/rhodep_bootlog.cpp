// SPDX-License-Identifier: Apache-2.0
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef RHODEP_BOOTLOG_HOST_TEST
#include <log/log_read.h>
#include <sys/system_properties.h>
#endif

namespace {
constexpr size_t KiB = 1024;
constexpr size_t Reserve = 4 * 1024 * KiB;
constexpr size_t SnapshotLimit = 64 * KiB;
constexpr const char* Directory = "/metadata/rhodep-bootlogs";
constexpr const char* OwnedFiles[] = {
        "boot-id", "kernel-first.txt", "kernel-tail.txt", "kernel-tail.txt.1",
        "android-first.txt", "android-tail.txt", "android-tail.txt.1",
        "status-first.txt", "status-latest.txt", "status-30s.txt", "status-120s.txt",
        "status-first.txt.tmp", "status-latest.txt.tmp", "status-30s.txt.tmp",
        "status-120s.txt.tmp", "boot-id.tmp"};

double BootTime() {
    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

bool WriteAll(int fd, const std::string& value) {
    size_t offset = 0;
    while (offset < value.size()) {
        ssize_t n = write(fd, value.data() + offset, value.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

std::string ReadAt(int directory, const std::string& path, size_t limit) {
    int fd = openat(directory, path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "<unavailable: " + std::string(strerror(errno)) + ">\n";
    std::string value;
    char buffer[4096];
    while (value.size() < limit) {
        ssize_t n = read(fd, buffer, std::min(sizeof(buffer), limit - value.size()));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        value.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return value;
}

bool HasSpace(int directory, size_t bytes) {
    struct statfs fs{};
    return fstatfs(directory, &fs) == 0 &&
           static_cast<uint64_t>(fs.f_bavail) * fs.f_bsize >= Reserve + bytes + 256 * KiB;
}

int OpenDirectory(int parent, const char* name) {
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

bool Save(int directory, const char* name, const std::string& data, size_t limit) {
    size_t size = std::min(limit, data.size());
    if (!HasSpace(directory, size)) return false;
    std::string temporary = std::string(name) + ".tmp";
    int fd = openat(directory, temporary.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_DSYNC, 0600);
    if (fd < 0) return false;
    bool ok = WriteAll(fd, data.substr(0, size)) && fsync(fd) == 0;
    close(fd);
    if (!ok || renameat(directory, temporary.c_str(), directory, name) != 0) return false;
    return fsync(directory) == 0;
}

bool RemoveBootDirectory(int parent, const char* directory_name) {
    int old = OpenDirectory(parent, directory_name);
    if (old < 0) return errno == ENOENT;
    DIR* contents = fdopendir(dup(old));
    if (!contents) { close(old); return false; }
    bool known = true;
    while (dirent* entry = readdir(contents)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        bool owned = false;
        for (const char* name : OwnedFiles) if (strcmp(name, entry->d_name) == 0) owned = true;
        if (!owned) { known = false; break; }
    }
    closedir(contents);
    if (!known) { close(old); return false; }
    bool ok = true;
    for (const char* name : OwnedFiles) {
        if (unlinkat(old, name, 0) != 0 && errno != ENOENT) ok = false;
    }
    close(old);
    return ok && unlinkat(parent, directory_name, AT_REMOVEDIR) == 0 && fsync(parent) == 0;
}

bool RemovePrevious(int parent) {
    return RemoveBootDirectory(parent, "previous");
}

bool IsBootId(const std::string& value) {
    if (value.size() != 37 || value.back() != '\n') return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (strchr("0123456789abcdef", value[i]) == nullptr) return false;
    }
    return true;
}

int PrepareBoot(int parent, const std::string& boot_id) {
    int current = OpenDirectory(parent, "current");
    bool rotate = current >= 0;
    if (current >= 0) {
        std::string old_id = ReadAt(current, "boot-id", 80);
        if (old_id == boot_id) return current;
        close(current);
        if (!IsBootId(old_id)) {
            if (!HasSpace(parent, 512 * KiB) || !RemoveBootDirectory(parent, "current")) return -1;
            rotate = false;
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    // Keep the last useful capture when there is no room for a new one.
    if (!HasSpace(parent, 512 * KiB)) return -1;
    if (rotate && (!RemovePrevious(parent) ||
                   renameat(parent, "current", parent, "previous") != 0)) return -1;
    if (fsync(parent) != 0 || mkdirat(parent, "current", 0700) != 0) return -1;
    current = OpenDirectory(parent, "current");
    if (current < 0 || !Save(current, "boot-id", boot_id, 80) || fsync(parent) != 0) {
        if (current >= 0) close(current);
        return -1;
    }
    return current;
}

class Stream {
  public:
    Stream(int directory, const char* name, size_t first_limit, size_t tail_limit)
        : directory_(directory), first_limit_(first_limit), tail_limit_(tail_limit),
          first_name_(std::string(name) + "-first.txt"),
          tail_name_(std::string(name) + "-tail.txt") {}
    ~Stream() {
        if (first_ >= 0) close(first_);
        if (tail_ >= 0) close(tail_);
    }
    bool Open() {
        first_ = OpenFile(first_name_, &first_bytes_);
        tail_ = OpenFile(tail_name_, &tail_bytes_);
        return first_ >= 0 && tail_ >= 0 && fsync(directory_) == 0;
    }
    bool Append(const std::string& value) {
        buffer_.append(value);
        return buffer_.size() < 32 * KiB || Flush();
    }
    bool Flush() {
        if (buffer_.empty()) return true;
        if (!HasSpace(directory_, buffer_.size() * 2)) return false;
        size_t head_size = std::min(buffer_.size(), first_limit_ -
                                    std::min(first_limit_, first_bytes_));
        if (head_size && !WriteAll(first_, buffer_.substr(0, head_size))) return false;
        first_bytes_ += head_size;
        size_t offset = 0;
        while (offset < buffer_.size()) {
            if (tail_bytes_ >= tail_limit_ && !Rotate()) return false;
            size_t size = std::min(buffer_.size() - offset, tail_limit_ - tail_bytes_);
            if (!WriteAll(tail_, buffer_.substr(offset, size))) return false;
            offset += size;
            tail_bytes_ += size;
        }
        buffer_.clear();
        return true;
    }
  private:
    int OpenFile(const std::string& name, size_t* bytes) {
        int fd = openat(directory_, name.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_DSYNC, 0600);
        struct stat st{};
        if (fd < 0) return -1;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
        *bytes = static_cast<size_t>(st.st_size);
        return fd;
    }
    bool Rotate() {
        close(tail_);
        tail_ = -1;
        if (renameat(directory_, tail_name_.c_str(), directory_,
                     (tail_name_ + ".1").c_str()) != 0 || fsync(directory_) != 0) return false;
        tail_ = openat(directory_, tail_name_.c_str(),
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_DSYNC, 0600);
        tail_bytes_ = 0;
        return tail_ >= 0 && fsync(directory_) == 0;
    }
    int directory_, first_ = -1, tail_ = -1;
    size_t first_limit_, tail_limit_, first_bytes_ = 0, tail_bytes_ = 0;
    std::string first_name_, tail_name_, buffer_;
};

#ifndef RHODEP_BOOTLOG_HOST_TEST
std::string Property(const char* name) {
    char value[PROP_VALUE_MAX]{};
    __system_property_get(name, value);
    return value;
}

void AddFile(std::string* snapshot, const std::string& path, size_t limit) {
    if (snapshot->size() >= SnapshotLimit) return;
    *snapshot += "\n--- " + path + " ---\n" + ReadAt(AT_FDCWD, path, limit);
}

std::string Snapshot(const std::string& boot_id, uint64_t kernel_bytes,
                     uint64_t overruns, int logd_error, const char* reason) {
    std::string data = "boot_id=" + boot_id + "uptime=" + std::to_string(BootTime()) +
                       " reason=" + reason + " kernel_bytes=" + std::to_string(kernel_bytes) +
                       " kmsg_overruns=" + std::to_string(overruns) +
                       " logd_error=" + std::to_string(logd_error) + "\n";
    for (const char* key : {"ro.bootmode", "ro.boot.mode", "ro.boot.slot_suffix",
             "ro.debuggable", "ro.adb.secure", "sys.boot_completed", "dev.bootcomplete",
             "apex.all.ready", "apexd.status", "ro.persistent_properties.ready",
             "init.svc.logd", "init.svc.apexd", "init.svc.adbd", "init.svc.zygote",
             "init.svc.surfaceflinger", "init.svc.bootanim", "init.svc.servicemanager",
             "init.svc.hwservicemanager", "init.svc.vendor.qti.hardware.display.composer",
             "sys.usb.config", "sys.usb.state", "sys.usb.configfs", "sys.usb.ffs.ready",
             "sys.usb.controller", "persist.sys.usb.config", "service.bootanim.exit"}) {
        data += std::string(key) + "=" + Property(key) + "\n";
    }
    for (const char* file : {"status", "wchan", "stack", "syscall"}) {
        AddFile(&data, std::string("/proc/1/") + file, 4096);
    }
    AddFile(&data, "/proc/cmdline", 4096);
    AddFile(&data, "/proc/mounts", 8192);
    AddFile(&data, "/proc/modules", 8192);
    AddFile(&data, "/config/usb_gadget/g1/UDC", 256);
    AddFile(&data, "/sys/class/android_usb/android0/state", 256);
    DIR* udcs = opendir("/sys/class/udc");
    if (udcs) {
        unsigned count = 0;
        while (dirent* entry = readdir(udcs)) {
            if (entry->d_name[0] == '.' || ++count > 4) continue;
            for (const char* node : {"state", "function", "current_speed", "maximum_speed"}) {
                AddFile(&data, std::string("/sys/class/udc/") + entry->d_name + "/" + node, 256);
            }
        }
        closedir(udcs);
    }
    DIR* processes = opendir("/proc");
    if (processes) {
        unsigned matched = 0, examined = 0;
        while (dirent* entry = readdir(processes)) {
            if (++examined > 8192 || matched >= 24 || data.size() >= SnapshotLimit) break;
            if (strspn(entry->d_name, "0123456789") != strlen(entry->d_name)) continue;
            std::string path = std::string("/proc/") + entry->d_name;
            std::string name = ReadAt(AT_FDCWD, path + "/comm", 128);
            bool wanted = false;
            for (const char* target : {"modprobe", "apexd", "logd", "adbd", "zygote",
                                      "system_server", "bootanimation", "surfaceflinger",
                                      "init", "ueventd"}) {
                if (name.find(target) != std::string::npos) wanted = true;
            }
            if (!wanted) continue;
            ++matched;
            AddFile(&data, path + "/status", 2048);
            AddFile(&data, path + "/wchan", 256);
            AddFile(&data, path + "/stack", 2048);
        }
        closedir(processes);
    }
    data.resize(std::min(data.size(), SnapshotLimit));
    return data;
}

void AndroidLogs(int directory, std::atomic<bool>* stop, std::atomic<int>* error) {
    Stream stream(directory, "android", 512 * KiB, 512 * KiB);
    if (!stream.Open()) { *error = errno ? errno : EIO; return; }
    log_time cursor(0, 0);
    double flushed = BootTime();
    while (!*stop) {
        logger_list* logs = android_logger_list_alloc_time(ANDROID_LOG_NONBLOCK, cursor, 0);
        if (!logs) { *error = ENOMEM; break; }
        for (log_id_t id : {LOG_ID_MAIN, LOG_ID_RADIO, LOG_ID_SYSTEM, LOG_ID_CRASH}) {
            android_logger_open(logs, id);
        }
        log_msg message{};
        unsigned round = 0;
        int n = 0;
        while (!*stop && round++ < 1024 && (n = android_logger_list_read(logs, &message)) > 0) {
            const auto& entry = message.entry;
            if (entry.hdr_size < sizeof(logger_entry) || message.len() > static_cast<unsigned>(n) ||
                entry.len < 1 || !message.msg()) continue;
            const char* payload = message.msg();
            std::string text(payload + 1, entry.len - 1);
            for (char& c : text) {
                if (c == '\0') c = ' ';
                else if (static_cast<unsigned char>(c) < 32 && c != '\n' && c != '\t') c = '?';
            }
            char prefix[192];
            snprintf(prefix, sizeof(prefix), "%u.%09u pid=%d tid=%u uid=%u logid=%u priority=%u ",
                     entry.sec, entry.nsec, entry.pid, entry.tid, entry.uid, entry.lid,
                     static_cast<unsigned char>(payload[0]));
            if (!stream.Append(prefix + text + "\n")) { *error = ENOSPC; *stop = true; break; }
            if (BootTime() - flushed >= 0.25) {
                if (!stream.Flush()) { *error = ENOSPC; *stop = true; break; }
                flushed = BootTime();
            }
            if (entry.sec > cursor.tv_sec ||
                (entry.sec == cursor.tv_sec && entry.nsec >= cursor.tv_nsec)) {
                cursor.tv_sec = entry.sec;
                cursor.tv_nsec = entry.nsec + 1;
                if (cursor.tv_nsec >= 1000000000) { ++cursor.tv_sec; cursor.tv_nsec = 0; }
            }
        }
        if (!*stop) *error = n < 0 && n != -EAGAIN ? -n : 0;
        android_logger_list_free(logs);
        if (!stream.Flush()) { *error = ENOSPC; *stop = true; break; }
        for (unsigned wait = 0; wait < 10 && !*stop; ++wait) usleep(100000);
    }
    stream.Flush();
}

int Capture() {
    const std::string bootmode = Property("ro.bootmode");
    const std::string mode = Property("ro.boot.mode");
    if (Property("ro.debuggable") != "1" ||
        (!bootmode.empty() && bootmode != "normal" && !(mode.empty() && bootmode == "unknown")) ||
        (!mode.empty() && mode != "normal") ||
        Property("ro.boot.hab.product") != "rhodep" ||
        access("/system/bin/recovery", F_OK) == 0) {
        return 1;
    }
    setpriority(PRIO_PROCESS, 0, 10);
    const std::string boot_id = ReadAt(AT_FDCWD, "/proc/sys/kernel/random/boot_id", 80);
    if (!IsBootId(boot_id)) return 1;
    struct statfs fs{};
    if (statfs("/metadata", &fs) != 0 || fs.f_type != EXT4_SUPER_MAGIC) return 2;
    umask(077);
    if (mkdir(Directory, 0700) != 0 && errno != EEXIST) return 2;
    int parent = OpenDirectory(AT_FDCWD, Directory);
    if (parent < 0) return 2;
    struct stat metadata{}, root{};
    if (fstat(parent, &metadata) != 0 || stat("/", &root) != 0 ||
        metadata.st_dev == root.st_dev) return 2;
    int lock = openat(parent, ".lock", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 3;
    int directory = PrepareBoot(parent, boot_id);
    if (directory < 0) return 4;
    Stream kernel(directory, "kernel", 256 * KiB, 384 * KiB);
    if (!kernel.Open()) return 5;
    std::atomic<bool> stop{false};
    std::atomic<int> logd_error{0};
    std::thread android(AndroidLogs, directory, &stop, &logd_error);
    int kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    uint64_t bytes = 0, overruns = 0;
    double start = BootTime(), last_snapshot = -10, last_flush = start, completed = 0;
    bool first = true, checkpoint30 = false, checkpoint120 = false;
    const char* reason = "capturing";
    while (!stop && BootTime() - start < 900) {
        if (kmsg < 0) kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        pollfd input{kmsg, POLLIN, 0};
        poll(&input, 1, 100);
        for (unsigned round = 0; kmsg >= 0 && round < 32; ++round) {
            char record[8192];
            ssize_t n = read(kmsg, record, sizeof(record));
            if (n < 0 && errno == EPIPE) { ++overruns; continue; }
            if (n <= 0) break;
            bytes += static_cast<uint64_t>(n);
            if (!kernel.Append(std::string(record, static_cast<size_t>(n)))) {
                reason = "metadata write/space limit"; stop = true; break;
            }
        }
        double now = BootTime();
        if (now - last_flush >= 0.25) {
            if (!kernel.Flush()) { reason = "metadata write/space limit"; stop = true; }
            last_flush = now;
        }
        if (now - last_snapshot >= 5) {
            std::string snapshot = Snapshot(boot_id, bytes, overruns, logd_error, reason);
            bool ok = Save(directory, "status-latest.txt", snapshot, SnapshotLimit);
            if (first) { ok = Save(directory, "status-first.txt", snapshot, SnapshotLimit) && ok; first = false; }
            if (now >= 30 && !checkpoint30) {
                ok = Save(directory, "status-30s.txt", snapshot, SnapshotLimit) && ok;
                checkpoint30 = true;
            }
            if (now >= 120 && !checkpoint120) {
                ok = Save(directory, "status-120s.txt", snapshot, SnapshotLimit) && ok;
                checkpoint120 = true;
            }
            if (!ok) { reason = "metadata write/space limit"; stop = true; }
            last_snapshot = now;
        }
        if (Property("sys.boot_completed") == "1") {
            if (!completed) completed = now;
            if (now - completed >= 30) { reason = "boot completed plus 30s"; break; }
        }
    }
    stop = true;
    if (strcmp(reason, "capturing") == 0) reason = "capture time limit or Android writer stopped";
    android.join();
    kernel.Flush();
    Save(directory, "status-latest.txt", Snapshot(boot_id, bytes, overruns, logd_error, reason),
         SnapshotLimit);
    if (kmsg >= 0) close(kmsg);
    close(directory);
    close(lock);
    close(parent);
    return 0;
}
#endif
}  // namespace

#ifndef RHODEP_BOOTLOG_HOST_TEST
int main() { return Capture(); }
#endif
