#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "voicelife/contracts/json.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::mcp::schedule_tool_input {

/** @brief 解析后的扁平周期规则字段。 */
struct ParsedRepeat {
    std::optional<schedule::Frequency> freq_type;
    std::optional<schedule::LocalTime> start_time;
    std::optional<schedule::LocalTime> end_time;
    std::optional<schedule::LocalDate> start_date;
    std::optional<schedule::LocalDate> end_date;
    std::optional<int32_t> interval_val;
    std::optional<uint8_t> weekdays_mask;
    std::optional<uint8_t> day_of_month;
    std::optional<uint8_t> month_of_year;
    std::optional<schedule::MonthlyMode> monthly_mode;
    std::optional<int32_t> occurrence_count;
    std::string error;

    [[nodiscard]] bool ok() const { return error.empty(); }
};

/** @brief 兼容旧调用方的 repeat 解析；新公开工具不再声明 repeat 对象。 */
ParsedRepeat ParseRepeat(const std::optional<JsonValue>& repeat, bool require_anchor);

/** @brief 从扁平工具参数解析周期规则字段。 */
ParsedRepeat ParseRuleProperties(const PropertyList& properties, bool require_anchor);

/** @brief 从周期创建工具参数构造创建规则命令。 */
schedule::CreateScheduleRuleCommand CreateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat);

/** @brief 从周期规则修改工具参数构造更新规则命令。 */
schedule::UpdateScheduleRuleCommand UpdateRuleCommand(const PropertyList& properties,
                                                       const ParsedRepeat& repeat);

/** @brief 创建一次性日程工具参数定义。 */
PropertyList CreateProperties();
/** @brief 创建周期日程工具参数定义；周期字段全部扁平化。 */
PropertyList CreateRuleProperties();
/** @brief 创建统一查询工具参数定义。 */
PropertyList QueryProperties();
/** @brief 创建一次性或已物化实例修改工具参数定义。 */
PropertyList UpdateProperties();
/** @brief 创建未来 occurrence 修改工具参数定义。 */
PropertyList UpdateOccurrenceProperties();
/** @brief 创建整条周期规则修改工具参数定义。 */
PropertyList UpdateRuleProperties();
/** @brief 创建一次性或已物化实例取消工具参数定义。 */
PropertyList DeleteProperties();
/** @brief 创建整条周期规则取消工具参数定义。 */
PropertyList DeleteRuleProperties();
/** @brief 创建未来 occurrence 跳过工具参数定义。 */
PropertyList SkipOccurrenceProperties();
/** @brief 创建操作记录查询工具参数定义。 */
PropertyList OperationQueryProperties();

}  // namespace voicelife::mcp::schedule_tool_input
