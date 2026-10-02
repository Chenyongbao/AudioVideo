#pragma once
#include <string>

// 计算文件 SHA-256（分段落盘后调用）
std::string sha256_file(const std::string& path);

// 追加一行到 manifest："<sha256>  <文件名>"
bool append_manifest(const std::string& manifest_path, const std::string& segment_path);
