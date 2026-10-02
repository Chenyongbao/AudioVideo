// SHA-256 每段录像校验 + manifest 清单（证据完整性链的最小模拟）
#include "middleware/integrity.hpp"
#include <openssl/sha.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <ctime>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>

namespace {

std::string hex(const uint8_t* d, int n) {
    static const char* t = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (int i = 0; i < n; ++i) { s += t[d[i] >> 4]; s += t[d[i] & 15]; }
    return s;
}

} // namespace

std::string sha256_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    SHA256_CTX c;
    SHA256_Init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) SHA256_Update(&c, buf, n);
    fclose(f);
    uint8_t out[SHA256_DIGEST_LENGTH];
    SHA256_Final(out, &c);
    return hex(out, SHA256_DIGEST_LENGTH);
}

namespace {
// 读 manifest 末条链哈希（无则返回全零 = 创世 prev）
std::string last_chain_hash(const std::string& manifest_path) {
    std::string last(64, '0');
    std::ifstream in(manifest_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("MARK ", 0) == 0) continue;
        if (line.size() >= 64 && line.find_first_not_of("0123456789abcdef") >= 64)
            last = line.substr(0, 64);
    }
    return last;
}
} // namespace

std::string chain_append(const std::string& manifest_path, const std::string& segment_path) {
    std::string file_h = sha256_file(segment_path);
    if (file_h.empty()) return {};
    std::string prev = last_chain_hash(manifest_path);
    // 链哈希 = SHA256(prev_chain || file_hash)
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, prev.data(), prev.size());
    SHA256_Update(&c, file_h.data(), file_h.size());
    uint8_t out[SHA256_DIGEST_LENGTH];
    SHA256_Final(out, &c);
    std::string chain = hex(out, SHA256_DIGEST_LENGTH);
    std::ofstream ofs(manifest_path, std::ios::app);
    ofs << chain << "  " << segment_path << "\n";
    return chain;
}

bool chain_verify(const std::string& manifest_path) {
    std::string prev(64, '0');
    std::ifstream in(manifest_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("MARK ", 0) == 0) continue;          // 标记行不参与链
        if (line.size() < 64) return false;
        std::string chain = line.substr(0, 64);
        std::string path = line.substr(66);                  // 跳过两空格
        std::string file_h = sha256_file(path);
        if (file_h.empty()) return false;                    // 文件丢失
        SHA256_CTX c;
        SHA256_Init(&c);
        SHA256_Update(&c, prev.data(), prev.size());
        SHA256_Update(&c, file_h.data(), file_h.size());
        uint8_t out[SHA256_DIGEST_LENGTH];
        SHA256_Final(out, &c);
        if (hex(out, SHA256_DIGEST_LENGTH) != chain) return false;  // 断链/篡改
        prev = chain;
    }
    return true;
}

bool append_manifest(const std::string& manifest_path, const std::string& segment_path) {
    std::string h = sha256_file(segment_path);
    if (h.empty()) return false;
    std::ofstream ofs(manifest_path, std::ios::app);
    ofs << h << "  " << segment_path << "\n";
    return true;
}

bool mark_segment(const std::string& manifest_path, const std::string& segment_path,
                  const std::string& note) {
    // 取本地时间（标记时刻；可信时间源接入后自动可信）
    char ts[32];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);
    std::ofstream ofs(manifest_path, std::ios::app);
    ofs << "MARK " << ts << " " << segment_path;
    if (!note.empty()) ofs << " " << note;
    ofs << "\n";
    return true;
}

int enforce_storage_quota(const std::string& dir, int64_t max_bytes,
                          const std::string& manifest_path) {
    // 收集 manifest 中被标记的重点文件（循环覆盖时跳过）
    std::vector<std::string> marked;
    {
        std::ifstream in(manifest_path);
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("MARK ", 0) == 0) {
                std::istringstream iss(line);
                std::string tag, ts, path;
                iss >> tag >> ts >> path;
                marked.push_back(path);
            }
    }

    // 遍历目录收集 seg_*.mp4 及其大小
    std::vector<std::pair<int64_t, std::string>> segs;  // (mtime, path)
    int64_t total = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.find("seg_") != 0 || name.find(".mp4") == std::string::npos) continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            segs.push_back({(int64_t)st.st_mtime, path});
            total += st.st_size;
        }
    }
    closedir(d);

    // 超额：按最旧优先删除，跳过重点文件
    std::sort(segs.begin(), segs.end());
    int removed = 0;
    for (auto& [mtime, path] : segs) {
        if (total <= max_bytes) break;
        if (std::find(marked.begin(), marked.end(), path) != marked.end()) continue;  // 重点保护
        struct stat st;
        if (stat(path.c_str(), &st) == 0) total -= st.st_size;
        unlink(path.c_str());
        ++removed;
    }
    if (removed == 0) return 0;

    // 同步重写 manifest：去掉指向已删文件的行
    std::ifstream in(manifest_path);
    std::vector<std::string> keep;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string tok1, tok2, path;
        iss >> tok1 >> tok2 >> path;      // hash 或 MARK tag
        if (path.empty()) path = tok2;    // "<hash>  <path>" 行只有两列
        bool gone = false;
        if (path.find(".mp4") != std::string::npos) {
            struct stat st;
            gone = stat(path.c_str(), &st) != 0;  // 文件已不存在则丢弃该行
        }
        if (!gone) keep.push_back(line);
    }
    std::ofstream out(manifest_path, std::ios::trunc);
    for (auto& l : keep) out << l << "\n";
    return removed;
}
