#pragma once
// 本机检索索引（B9）：扫描分段目录 + 解析 manifest（哈希链/MARK 行）
// 生成 index.json —— 检索视图与证据账本(manifest)分离：
//   manifest = 防篡改账本（追加式）；index.json = 给检索/UI 用的只读快照。
// 板端移植时此 JSON 即检索模块的数据源（按时间/标记过滤）。
#include <string>

struct IndexOptions {
    bool verify_chain = true;   // 生成时是否校验哈希链（慢：每段重读全文件）
};

// 扫描 dir 下 seg_*.mp4，结合 manifest 生成 dir/index.json。
// 返回写入的条目数（-1 = 目录/manifest 异常）。
int build_index(const std::string& dir, const IndexOptions& opts = {});
