#pragma once
// ============================================================================
// 本机检索索引（B9）—— manifest（证据账本）与 index.json（检索视图）分离。
// ----------------------------------------------------------------------------
// 【为什么两者分离】
//   manifest 防篡改：只追加、含哈希链，取证校验只认它；
//   index.json     用户体验：给检索/回放 UI 用的只读快照，损坏可随时重建，
//   不承担证据职责。板端移植时 index 即检索模块的数据源（按时间/标记过滤）。
// 【数据来源】目录扫描 seg_*.mp4（mtime≈收尾时刻）+ MARK 行关联 +
//   chain_verify 整体校验结果。
// ============================================================================
#include <string>

struct IndexOptions {
    bool verify_chain = true;   // 生成时是否校验哈希链（慢：每段重读全文件）
};

// 扫描 dir 下 seg_*.mp4，结合 manifest 生成 dir/index.json。
// 返回写入的条目数（-1 = 目录/manifest 异常）。
int build_index(const std::string& dir, const IndexOptions& opts = {});
