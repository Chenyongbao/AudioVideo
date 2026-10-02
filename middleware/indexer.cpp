// B9 本机检索索引：扫描分段 + manifest 关联（MARK/链校验）→ index.json
// 检索视图与证据账本分离：manifest 追加式防篡改，index.json 只读快照可随时重建。
#include "middleware/indexer.hpp"
#include "middleware/integrity.hpp"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace {

struct Entry {
    std::string path;
    int64_t mtime = 0;
    int64_t size = 0;
    bool marked = false;
    std::string mark_time, mark_note;
};

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// 毫秒时间戳文件名 seg_<ms>.mp4 → "YYYY-MM-DD HH:MM:SS"（本地时区）
std::string msToIso(int64_t ms) {
    time_t t = ms / 1000;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

} // namespace

int build_index(const std::string& dir, const IndexOptions& opts) {
    // 1) manifest 解析：MARK 行 → 文件标记表
    std::map<std::string, std::pair<std::string, std::string>> marks;  // path → (time, note)
    {
        std::ifstream in(dir + "/manifest.sha256");
        if (!in) return -1;
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("MARK ", 0) == 0) {
                std::istringstream iss(line);
                std::string tag, ts, path, note;
                iss >> tag >> ts >> path;
                std::getline(iss, note);
                if (!note.empty() && note[0] == ' ') note.erase(0, 1);
                marks[path] = {ts, note};
            }
    }

    // 2) 目录扫描 seg_*.mp4
    std::vector<Entry> entries;
    DIR* d = opendir(dir.c_str());
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.find("seg_") != 0 || name.find(".mp4") == std::string::npos) continue;
        Entry en;
        en.path = dir + "/" + name;
        struct stat st;
        if (stat(en.path.c_str(), &st) != 0) continue;
        en.mtime = (int64_t)st.st_mtime * 1000;
        en.size = st.st_size;
        auto it = marks.find(en.path);
        if (it != marks.end()) {
            en.marked = true;
            en.mark_time = it->second.first;
            en.mark_note = it->second.second;
        }
        entries.push_back(en);
    }
    closedir(d);
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    // 3) 哈希链整体校验（一次）
    bool chain_ok = false;
    if (opts.verify_chain) chain_ok = chain_verify(dir + "/manifest.sha256");

    // 4) 写 index.json
    FILE* f = fopen((dir + "/index.json").c_str(), "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"chain_verified\": %s,\n  \"segments\": [\n",
            chain_ok ? "true" : "false");
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& en = entries[i];
        // 文件名毫秒 = 单调钟（开机起）分段起点，非墙钟：保留原始值 start_ms 供排序/接口用；
        // ISO 时间用文件 mtime（≈ 该段收尾时刻，语义如实）
        size_t pos = en.path.rfind("seg_");
        int64_t start_ms = (pos != std::string::npos) ? atoll(en.path.c_str() + pos + 4) : 0;
        fprintf(f,
                "    {\"file\": \"%s\", \"start_ms\": %lld, \"modified\": \"%s\", \"size\": %lld, "
                "\"marked\": %s, \"mark_time\": \"%s\", \"note\": \"%s\"}%s\n",
                jsonEscape(en.path).c_str(),
                (long long)start_ms,
                msToIso(en.mtime).c_str(),
                (long long)en.size,
                en.marked ? "true" : "false",
                jsonEscape(en.mark_time).c_str(),
                jsonEscape(en.mark_note).c_str(),
                i + 1 < entries.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return (int)entries.size();
}
