#pragma once
// ============================================================================
// 共享契约总入口（冻结于主 agent，子任务只读依赖）
//
// 冻结内容：ArchiveRecord / SummaryJob / Proposal / AccessContext /
//           MemoryProvider / 可见性与错误码 / 预算常量。
// 任何子任务需要修改这些头文件时，必须先向主 agent 提交变更说明。
// ============================================================================

#include "core/contracts/AccessContext.h"
#include "core/contracts/ArchiveRecord.h"
#include "core/contracts/Errors.h"
#include "core/contracts/Limits.h"
#include "core/contracts/ProposalContracts.h"
#include "core/contracts/SummaryContracts.h"
#include "core/contracts/Visibility.h"
#include "providers/memory/MemoryProvider.h"
