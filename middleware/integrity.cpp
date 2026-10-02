// SHA-256 每段录像校验 + manifest 清单（证据完整性链的最小模拟）
#include "middleware/integrity.hpp"
#include <openssl/sha.h>
#include <cstdio>
#include <fstream>
#include <sstream>

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

bool append_manifest(const std::string& manifest_path, const std::string& segment_path) {
    std::string h = sha256_file(segment_path);
    if (h.empty()) return false;
    std::ofstream ofs(manifest_path, std::ios::app);
    ofs << h << "  " << segment_path << "\n";
    return true;
}
