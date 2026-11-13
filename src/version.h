#pragma once

// =============================================================================
// version.h —— 项目名称与版本号
//
// 用 inline constexpr 写在头文件中：C++17 的 inline 变量允许多个编译单元
// （.cpp）同时包含本头文件而不会产生“重复定义”链接错误。
// /api/status 与 /metrics 会输出版本号，用于确认线上运行的是哪个构建。
// =============================================================================

namespace blob {

// 项目展示名（启动日志、HTTP 响应头 Server 字段等处使用）
inline constexpr const char* kProjectName = "Blob Arena";
// 语义化版本号：1.0.0-rc1 表示第一个候选发布版本
inline constexpr const char* kProjectVersion = "1.0.0-rc1";

}  // namespace blob
