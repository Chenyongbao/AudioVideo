// ============================================================================
// 证据防篡改哈希链实现(OpenSSL libcrypto EVP API)
// 链式结构:每段记录含前段 sha,任何篡改/删段/插段都会断链
// ============================================================================
#include "hashchain.hpp"

#include <openssl/sha.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- 单文件 SHA-256(流式,不整读内存) ----
static bool sha256_file(const std::string &path, uint8_t out[32], long long *sizeOut) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    uint8_t buf[64 * 1024];
    size_t n;
    long long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        SHA256_Update(&ctx, buf, n);
        total += (long long)n;
    }
    fclose(f);
    SHA256_Final(out, &ctx);
    if (sizeOut) *sizeOut = total;
    return true;
}

static void toHex(const uint8_t h[32], char out[65]) {
    static const char *T = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = T[h[i] >> 4];
        out[i * 2 + 1] = T[h[i] & 0xF];
    }
    out[64] = 0;
}

// 链文件行: name bytes epoch sha prev
struct ChainEntry {
    char name[128] = {0};
    std::string sha, prev;
    long long bytes = 0;
};

static std::vector<ChainEntry> loadChain(const char *path) {
    std::vector<ChainEntry> v;
    FILE *f = fopen(path, "r");
    if (!f) return v;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        ChainEntry e;
        char sha[65] = {0}, prev[65] = {0};
        if (sscanf(line, "%127s %lld %*lld %64s %64s",
                   e.name, &e.bytes, sha, prev) == 4) {
            e.sha = sha; e.prev = prev;
            v.push_back(e);
        }
    }
    fclose(f);
    return v;
}

bool hashchain_append(const std::string &filepath) {
    // 只收 rec_*.mp4
    if (filepath.find("rec_") == std::string::npos ||
        filepath.size() < 8 || filepath.compare(filepath.size() - 4, 4, ".mp4") != 0)
        return false;

    uint8_t hash[32];
    long long bytes = 0;
    if (!sha256_file(filepath, hash, &bytes)) return false;
    char hex[65];
    toHex(hash, hex);

    // 取链尾(最后一段的 sha 作 prev)
    std::vector<ChainEntry> chain = loadChain("/userdata/hashchain.log");
    std::string prev(64, '0');
    if (!chain.empty()) prev = chain.back().sha;

    // 文件名(去掉目录)
    const char *p = filepath.c_str();
    const char *slash = strrchr(p, '/');
    const char *name = slash ? slash + 1 : p;

    FILE *f = fopen("/userdata/hashchain.log", "a");
    if (!f) return false;
    fprintf(f, "%s %lld %lld %s %s\n", name, bytes, (long long)time(nullptr), hex, prev.c_str());
    fclose(f);
    fprintf(stderr, "[hash] chained %s (%lld B)\n", name, bytes);
    return true;
}

std::string hashchain_verify() {
    std::vector<ChainEntry> chain = loadChain("/userdata/hashchain.log");
    if (chain.empty())
        return "EMPTY 链为空(尚无录像入链)\n";

    std::string report;
    std::string expectPrev(64, '0');
    int okCnt = 0, badCnt = 0, missingCnt = 0;
    bool chainBroken = false;

    for (const auto &e : chain) {
        const std::string nm = e.name;              // char[] → string(后续拼接统一)
        std::string full = "/userdata/" + nm;
        char line[256];
        if (chainBroken) {
            // 前面已断链:后面全部不可信(链式保护的意义)
            snprintf(line, sizeof(line), "UNTRUSTED %s (断链后段落)\n", nm.c_str());
            badCnt++;
            report += line;
            continue;
        }
        FILE *f = fopen(full.c_str(), "rb");
        if (!f) {
            snprintf(line, sizeof(line), "MISSING %s (文件不存在)\n", nm.c_str());
            missingCnt++;
            report += line;
            continue;
        }
        fclose(f);

        uint8_t hash[32];
        long long bytes = 0;
        sha256_file(full, hash, &bytes);
        char hex[65];
        toHex(hash, hex);

        if (hex != e.sha || bytes != e.bytes) {
            snprintf(line, sizeof(line), "TAMPERED %s (内容与哈希不符)\n", nm.c_str());
            badCnt++;
            chainBroken = true;          // 断链:后续段落不可信
            report += line;
            continue;
        }
        if (e.prev != expectPrev) {
            snprintf(line, sizeof(line), "TAMPERED %s (链断裂:与前段哈希不衔接)\n", nm.c_str());
            badCnt++;
            chainBroken = true;
            report += line;
            continue;
        }
        snprintf(line, sizeof(line), "OK %s (%lld B)\n", nm.c_str(), e.bytes);
        okCnt++;
        expectPrev = e.sha;
        report += line;
    }
    char sum[128];
    snprintf(sum, sizeof(sum), "SUMMARY %d ok, %d tampered, %d missing — %s\n",
             okCnt, badCnt, missingCnt,
             (badCnt || missingCnt) ? "证据链已被破坏!" : "证据链完整");
    report += sum;
    return report;
}
