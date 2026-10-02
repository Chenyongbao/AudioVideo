#pragma once
// ============================================================================
// 证据完整性模块 —— manifest.sha256 账本 + 三种写入。
// ----------------------------------------------------------------------------
// 【账本格式】纯文本逐行追加（取证工具直接可读）：
//   <链哈希>  <文件路径>          —— 每段落盘后写入（chain_append）
//   MARK <ISO时间> <路径> <备注>  —— 重点标记行（mark_segment，不参与链）
// 【哈希链防篡改】链哈希 = SHA256(前段链哈希 ‖ 本段文件哈希)，首行 prev 为全零：
//   任何一段被替换/删除/篡改，其后所有链哈希校验即失败 —— 攻击者无法只重算
//   一行；要伪造需连文件带全链一起重做。（B11：从"明文追加"升级而来的设计）
// 【与 index.json 的关系】本文件是证据账本（防篡改，只追加）；
//   index.json 是检索视图（可随时重建）—— 职责分离，见 indexer.hpp。
// ============================================================================
#include <string>

// 计算文件 SHA-256（分段落盘后调用）
std::string sha256_file(const std::string& path);

// 追加一行到 manifest："<sha256>  <文件名>"
bool append_manifest(const std::string& manifest_path, const std::string& segment_path);

// 哈希链防篡改升级：链式哈希 = SHA256(前段链哈希 || 本段文件哈希)，
// manifest 变为追加式账本——任何一段被替换/删除，后续所有链哈希校验即失败。
// 行格式："<chained_sha256>  <文件名>"，首行 prev_hash 为全零。
// 返回写入的链哈希（供外部保存断点；正常使用无需关心）。
std::string chain_append(const std::string& manifest_path, const std::string& segment_path);

// 校验整条哈希链：重算每段的 链哈希 并比对。全链有效返回 true。
bool chain_verify(const std::string& manifest_path);

// 重点文件标记（GA/T 947.2 6.2.20）：在 manifest 追加 "MARK <iso时间> <文件名> <备注>"
bool mark_segment(const std::string& manifest_path, const std::string& segment_path,
                  const std::string& note = "");

// 满盘循环覆盖（6.4.12）：目录总大小超 max_bytes 时删除最旧分段（跳过被标记的重点文件）。
// 返回删除的文件数。删除后同步重写 manifest（去掉已删文件条目）。
int enforce_storage_quota(const std::string& dir, int64_t max_bytes,
                          const std::string& manifest_path);
