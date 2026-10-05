#ifndef HASHCHAIN_HPP
#define HASHCHAIN_HPP

#include <string>

// ============================================================================
// 证据防篡改哈希链:
//   每段录像停止时计算 SHA-256,追加到 /userdata/hashchain.log:
//     <文件名> <字节数> <epoch> <sha256hex> <prev_sha256hex>
//   首段 prev 为 64 个 '0'(创世);后段 prev = 前段 sha —— 任何一段被篡改/
//   删除/插入,后续所有段校验断链,回放端一键发现。
// ============================================================================

// 录像停止后调用:算该文件哈希并入链;返回 false = 打开失败等(不抛异常)
bool hashchain_append(const std::string &filepath);

// 校验整条链:返回多行报告文本(逐段 OK/TAMPERED/MISSING + 总结行)
//   链接断(前段哈希不匹配)与文件被改都视为被篡改;文件不存在报 MISSING
std::string hashchain_verify();

#endif // HASHCHAIN_HPP
