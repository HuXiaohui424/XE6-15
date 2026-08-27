#include "voicelife/mcp/schedule_mcp_tools.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "schedule_mcp_tools_input.h"
#include "schedule_tool_output.h"
#include "voicelife/contracts/im/reminder_action_status_report.h"
#include "voicelife/contracts/im/schedule_query_result.h"
#include "voicelife/im/im_reporting_channel.h"
#include "voicelife/im/im_runtime.h"
#include "voicelife/mcp/mcp_server.h"
#include "voicelife/schedule/calendar.h"
#include "voicelife/schedule/schedule_commands.h"
#include "voicelife/schedule/schedule_operation_service.h"
#include "voicelife/schedule/schedule_reminder_service.h"
#include "voicelife/schedule/schedule_results.h"
#include "voicelife/schedule/schedule_rule_commands.h"
#include "voicelife/schedule/schedule_rule_results.h"
#include "voicelife/schedule/schedule_rule_service.h"
#include "voicelife/schedule/schedule_service.h"
#include "voicelife/schedule/schedule_types.h"

namespace voicelife::mcp {
namespace {

using schedule::DateTime;
using schedule::Schedule;
using schedule::ScheduleRule;
using schedule::ScheduleRuleService;
using schedule::ScheduleService;
using voicelife::MakeToolOutput;
using voicelife::ToolOutputArray;
using voicelife::ToolOutputObject;
using voicelife::ToolOutputValue;
using voicelife::mcp::schedule_tool_input::CreateProperties;
using voicelife::mcp::schedule_tool_input::CreateRuleCommand;
using voicelife::mcp::schedule_tool_input::CreateRuleProperties;
using voicelife::mcp::schedule_tool_input::DeleteProperties;
using voicelife::mcp::schedule_tool_input::DeleteRuleProperties;
using voicelife::mcp::schedule_tool_input::OperationQueryProperties;
using voicelife::mcp::schedule_tool_input::ParsedRepeat;
using voicelife::mcp::schedule_tool_input::ParseRepeat;
using voicelife::mcp::schedule_tool_input::ParseRuleProperties;
using voicelife::mcp::schedule_tool_input::QueryProperties;
using voicelife::mcp::schedule_tool_input::SkipOccurrenceProperties;
using voicelife::mcp::schedule_tool_input::UpdateOccurrenceProperties;
using voicelife::mcp::schedule_tool_input::UpdateProperties;
using voicelife::mcp::schedule_tool_input::UpdateRuleCommand;
using voicelife::mcp::schedule_tool_input::UpdateRuleProperties;

ToolResult Output(ToolOutputObject fields) { return ToolResult::Success(ToolOutputValue::Object(std::move(fields))); }

ToolResult SummaryOutput(ToolOutputObject fields, std::string summary) {
    ToolResult result = Output(std::move(fields));
    result.text_output = std::move(summary);
    return result;
}

const ToolOutputValue* ObjectField(const ToolOutputValue& value, std::string_view key) {
    if (!value.IsObject() || value.object == nullptr) return nullptr;
    for (const auto& [field, item] : *value.object) {
        if (field == key) return item.get();
    }
    return nullptr;
}

std::string StringField(const ToolOutputValue& value, std::string_view key) {
    const ToolOutputValue* field = ObjectField(value, key);
    return field != nullptr && field->IsString() ? field->string : std::string{};
}

std::string VoiceScheduleEntry(const ToolOutputValue& value, std::size_t index) {
    std::string text = "第 " + std::to_string(index) + " 条：";
    const std::string event = StringField(value, "event");
    text += event.empty() ? "未命名日程" : event;
    const std::string start = StringField(value, "start_time");
    const std::string end = StringField(value, "end_time");
    if (!start.empty()) {
        text += "，时间 " + start;
        if (!end.empty()) text += " 至 " + end;
    }
    const std::string location = StringField(value, "location");
    if (!location.empty()) text += "，地点 " + location;
    const std::string notes = StringField(value, "notes");
    if (!notes.empty()) text += "，备注 " + notes;
    return text;
}

std::string FullVoiceScheduleText(const ToolOutputArray& schedules, const ToolOutputArray& future_occurrences,
                                  const ToolOutputArray& exceptions) {
    const std::size_t count = schedules.size() + future_occurrences.size();
    if (count == 0) return "没有查询到日程。";
    std::string text = "查询到 " + std::to_string(count) + " 条日程。";
    std::size_t index = 1;
    for (const auto& item : schedules) {
        if (item != nullptr) text += VoiceScheduleEntry(*item, index++) + "。";
    }
    for (const auto& item : future_occurrences) {
        if (item != nullptr) text += VoiceScheduleEntry(*item, index++) + "。";
    }
    if (!exceptions.empty()) text += "另有 " + std::to_string(exceptions.size()) + " 项例外调整。";
    return text;
}

std::optional<JsonValue> ParseOutputJson(const ToolOutputValue& output) {
    JsonValue value;
    JsonParseOptions options;
    options.max_bytes = 128 * 1024;
    options.max_nodes = 4096;
    options.max_array_items = 128;
    options.max_allocator_bytes = 512 * 1024;
    if (!ParseJson(SerializeToolOutputValue(output), value, options).ok()) return std::nullopt;
    return value;
}

std::string QueryNowIso() {
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

ToolResult FailureOutput(std::string message) {
    return Output({
        MakeToolOutput("status", ToolOutputValue::String("failure")),
        MakeToolOutput("message", ToolOutputValue::String(std::move(message))),
    });
}

ToolResult ConflictOutput(std::string message, ToolOutputArray conflicts) {
    return Output({
        MakeToolOutput("status", ToolOutputValue::String("conflict")),
        MakeToolOutput("message", ToolOutputValue::String(std::move(message))),
        MakeToolOutput("conflicts", ToolOutputValue::Array(std::move(conflicts))),
    });
}

schedule::ScheduleStatusFilter ParseStatus(const std::string& value) {
    if (value == "all") return schedule::ScheduleStatusFilter::kAll;
    if (value == "active") return schedule::ScheduleStatusFilter::kActive;
    if (value == "cancelled") return schedule::ScheduleStatusFilter::kCancelled;
    if (value == "completed") return schedule::ScheduleStatusFilter::kCompleted;
    return schedule::ScheduleStatusFilter::kActive;
}

/** @brief 返回当前秒级系统时间。 @return 当前日程时间。 */
DateTime Now() { return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()); }

/** @brief 将实体类型字符串转为枚举；非法值返回空。 @param value 输入字符串。 @return 对应枚举。 */
std::optional<schedule::OperationEntityType> ParseEntityType(const std::string& value) {
    if (value == "schedule") return schedule::OperationEntityType::kSchedule;
    if (value == "rule") return schedule::OperationEntityType::kRule;
    if (value == "exception") return schedule::OperationEntityType::kException;
    return std::nullopt;
}

/** @brief 将操作类型字符串转为枚举；非法值返回空。 @param value 输入字符串。 @return 对应枚举。 */
std::optional<schedule::ScheduleOperationType> ParseOperationType(const std::string& value) {
    if (value == "create") return schedule::ScheduleOperationType::kCreate;
    if (value == "update") return schedule::ScheduleOperationType::kUpdate;
    if (value == "delete") return schedule::ScheduleOperationType::kDelete;
    return std::nullopt;
}

std::string FormatDateStart(const schedule::LocalDate& date) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d 00:00:00", date.year, date.month, date.day);
    return buffer;
}

std::string FormatDateEnd(const schedule::LocalDate& date) {
    const int64_t days = schedule::DaysFromCivil(date.year, date.month, date.day) + 1;
    schedule::LocalDate next;
    schedule::CivilFromDays(days, next.year, next.month, next.day);
    return FormatDateStart(next);
}

std::optional<DateTime> ParseDateStart(const PropertyList& properties) {
    const auto value = properties.value<std::string>("start_date");
    if (!value.has_value()) return std::nullopt;
    const auto date = schedule_tool_output::ParseLocalDate(*value);
    if (!date.has_value()) return std::nullopt;
    return schedule_tool_output::ParseDateTime(FormatDateStart(*date));
}

std::optional<DateTime> ParseDateEnd(const PropertyList& properties) {
    const auto value = properties.value<std::string>("end_date");
    if (!value.has_value()) return std::nullopt;
    const auto date = schedule_tool_output::ParseLocalDate(*value);
    if (!date.has_value()) return std::nullopt;
    return schedule_tool_output::ParseDateTime(FormatDateEnd(*date));
}

std::optional<ToolResult> SynchronizeReminder(schedule::ScheduleReminderService* reminder_service,
                                              schedule::ScheduleId schedule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->SynchronizeSchedule(schedule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("日程已保存，但提醒同步失败：" + status.message);
}

std::optional<ToolResult> CancelReminder(schedule::ScheduleReminderService* reminder_service,
                                         schedule::ScheduleId schedule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->CancelScheduleReminder(schedule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("日程已取消，但提醒取消失败：" + status.message);
}

std::optional<ToolResult> VerifyCancellationTarget(const schedule::Schedule& schedule, const PropertyList& properties) {
    const auto expected_event = properties.value<std::string>("expected_event");
    const auto expected_start_time = properties.value<std::string>("expected_start_time");
    if (!expected_event.has_value() || !expected_start_time.has_value()) {
        return FailureOutput("请先通过 schedule.query 确认目标，并回传 event 和 start_time");
    }
    if (!schedule.start_time.has_value() || schedule.event != *expected_event ||
        schedule_tool_output::FormatDateTime(*schedule.start_time) != *expected_start_time) {
        return FailureOutput("日程目标与确认内容不匹配，未执行取消");
    }
    return std::nullopt;
}

std::optional<ToolResult> SuspendRuleReminders(schedule::ScheduleReminderService* reminder_service,
                                               schedule::ScheduleRuleId rule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->SuspendRuleReminders(rule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("周期规则已修改，但旧提醒撤销失败：" + status.message);
}

std::optional<ToolResult> SynchronizeRule(schedule::ScheduleReminderService* reminder_service,
                                          schedule::ScheduleRuleId rule_id) {
    if (reminder_service == nullptr) return std::nullopt;
    const Status status = reminder_service->SynchronizeRule(rule_id);
    if (status.ok()) return std::nullopt;
    return FailureOutput("周期规则已修改，但提醒同步失败：" + status.message);
}

bool WithinRange(const std::optional<DateTime>& start, const std::optional<DateTime>& end, DateTime value) {
    if (start.has_value() && value < *start) return false;
    if (end.has_value() && value >= *end) return false;
    return true;
}

std::string IsoFromDateTime(DateTime value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

const char* ActionName(schedule::ScheduleReminderActionKind action) {
    return action == schedule::ScheduleReminderActionKind::kSnooze ? "snooze" : "acknowledge";
}

bool ReportVoiceActionResults(const std::vector<schedule::ReminderActionResult>& results,
                              ScheduleQueryReportingContext reporting_context) {
    auto* runtime = reporting_context.runtime;
    auto* channel = runtime == nullptr ? nullptr : runtime->reporting_channel();
    // 本地事实已经落库；IM 未 ready 时由 Runtime worker 补报，不能把“未发送”伪报为成功。
    if (channel == nullptr || runtime == nullptr || runtime->device_id().empty()) return false;
    bool all_submitted = true;
    for (const auto& result : results) {
        contracts::im::ReminderActionStatusReport report;
        report.schemaVersion = "1";
        report.eventId = "voice-action:" + result.operation_id;
        report.correlationId = result.operation_id;
        report.deviceId = runtime->device_id();
        report.reminderTriggerId = result.reminder_trigger_id;
        report.operationId = result.operation_id;
        report.action = ActionName(result.action);
        report.status = "succeeded";
        report.occurredAt = IsoFromDateTime(result.occurred_at);
        if (result.next_trigger_at.has_value()) report.nextTriggerAt = IsoFromDateTime(*result.next_trigger_at);
        report.source = "voice";
        const auto submitted = channel->SubmitReminderActionStatusReport(report);
        if (submitted.status != voicelife::im::ReportStatus::kSubmitted) all_submitted = false;
    }
    return all_submitted;
}

}  // namespace

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService* rule_service,
                                schedule::ScheduleOperationService* operation_service,
                                schedule::ScheduleReminderService* reminder_service,
                                ScheduleQueryReportingContext reporting_context) {
    auto parse_once_command = [](const PropertyList& properties, schedule::UpdateScheduleCommand* update = nullptr)
        -> std::optional<std::string> {
        const auto start_text = properties.value<std::string>("start_time");
        const auto end_text = properties.value<std::string>("end_time");
        if (start_text.has_value()) {
            const auto parsed = schedule_tool_output::ParseDateTime(*start_text);
            if (!parsed.has_value()) return "start_time 格式必须是 YYYY-MM-DD HH:mm:ss，且必须是真实有效的时间";
            if (update != nullptr) update->start_time = parsed;
        }
        if (end_text.has_value()) {
            const auto parsed = schedule_tool_output::ParseDateTime(*end_text);
            if (!parsed.has_value()) return "end_time 格式必须是 YYYY-MM-DD HH:mm:ss，且必须是真实有效的时间";
            if (update != nullptr) update->end_time = parsed;
        }
        return std::nullopt;
    };
    auto once_update_command = [&parse_once_command](const PropertyList& properties) {
        schedule::UpdateScheduleCommand command;
        command.schedule_id = properties.value<int64_t>("schedule_id").value_or(0);
        command.event = properties.value<std::string>("event");
        command.location = properties.value<std::string>("location").has_value()
                               ? schedule::NullableScheduleUpdate<std::string>{properties.value<std::string>("location")}
                               : schedule::NullableScheduleUpdate<std::string>{};
        command.notes = properties.value<std::string>("notes").has_value()
                            ? schedule::NullableScheduleUpdate<std::string>{properties.value<std::string>("notes")}
                            : schedule::NullableScheduleUpdate<std::string>{};
        const auto error = parse_once_command(properties, &command);
        if (error.has_value()) return std::pair<std::optional<schedule::UpdateScheduleCommand>, std::string>{std::nullopt, *error};
        command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
        return std::pair<std::optional<schedule::UpdateScheduleCommand>, std::string>{command, {}};
    };

    Status status = server.add_tool(
        "schedule.create",
        "创建一条一次性日程并直接写入 schedule 表。只能创建独立的一次性日程；不要传 rule_id、original_start_time、repeat 或任何周期规则字段。event 必填，其余业务字段按参数描述决定是否传入。",
        CreateProperties(), [&service, reminder_service](const PropertyList& properties) {
            schedule::CreateScheduleCommand command;
            command.event = properties.value<std::string>("event").value_or("");
            const auto start_text = properties.value<std::string>("start_time");
            const auto end_text = properties.value<std::string>("end_time");
            if (start_text.has_value()) {
                command.start_time = schedule_tool_output::ParseDateTime(*start_text);
                if (!command.start_time.has_value()) return FailureOutput("start_time 格式必须是 YYYY-MM-DD HH:mm:ss，且必须是真实有效的时间");
            }
            if (end_text.has_value()) {
                command.end_time = schedule_tool_output::ParseDateTime(*end_text);
                if (!command.end_time.has_value()) return FailureOutput("end_time 格式必须是 YYYY-MM-DD HH:mm:ss，且必须是真实有效的时间");
            }
            command.location = properties.value<std::string>("location");
            command.notes = properties.value<std::string>("notes");
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
            const auto result = service.create_schedule(command);
            if (!result.result.ok() || !result.result.value.has_value()) {
                const std::string message = result.result.status.message.empty() ? "一次性日程创建失败" : result.result.status.message;
                if (result.result.status.code == ErrorCode::kConflict)
                    return ConflictOutput(message, schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                return FailureOutput(message);
            }
            const Schedule& saved = *result.result.value;
            if (const auto reminder = SynchronizeReminder(reminder_service, saved.id); reminder.has_value()) return *reminder;
            const std::string message = "已创建一次性日程“" + saved.event + "”，schedule_id=" + std::to_string(saved.id);
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String(message)),
                           MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(saved)),
                           MakeToolOutput("conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
    if (!status.ok()) return status;

    if (rule_service != nullptr) {
        status = server.add_tool(
        "schedule.create_rule",
        "创建周期性日程：在 schedule_rule 表创建周期规则，并在同一业务操作中生成首条 schedule 实例。周期字段必须直接作为顶层参数传入，不使用 repeat 对象；event、freq_type、start_date、start_time 必填，其余字段按参数描述决定。",
        CreateRuleProperties(), [rule_service, reminder_service](const PropertyList& properties) {
            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法创建周期规则");
            const ParsedRepeat parsed = ParseRuleProperties(properties, true);
            if (!parsed.ok()) return FailureOutput(parsed.error);
            const auto result = rule_service->create_schedule_rule(CreateRuleCommand(properties, parsed));
            if (!result.status.ok()) {
                if (result.status.code == ErrorCode::kConflict)
                    return ConflictOutput(result.status.message, schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                return FailureOutput(result.status.message.empty() ? "周期日程创建失败" : result.status.message);
            }
            if (!result.rule.has_value()) return FailureOutput("周期日程创建失败：服务未返回已保存的周期规则");
            const auto first = result.first_schedule;
            if (!first.has_value()) return FailureOutput("周期日程创建失败：服务未返回首条 schedule 实例");
            if (const auto reminder = SynchronizeRule(reminder_service, result.rule->id); reminder.has_value()) return *reminder;
            const std::string message = "已创建周期日程“" + result.rule->event + "”，rule_id=" + std::to_string(result.rule->id) +
                                        "，首条日程 schedule_id=" + std::to_string(first->id);
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String(message)),
                           MakeToolOutput("rule", schedule_tool_output::RuleOutput(*result.rule)),
                           MakeToolOutput("first_schedule", schedule_tool_output::ScheduleOutput(*first, &*result.rule)),
                           MakeToolOutput("conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
        if (!status.ok()) return status;
    }

    status = server.add_tool_with_context(
        "schedule.query",
        "统一查询一次性日程和周期日程。返回结果始终按 one_time_schedules、recurring_rules、recurring_schedules、future_occurrences、exceptions 分类；schedule_id 与 rule_id 互斥，具体使用方式见参数描述。",
        QueryProperties(), [&service, rule_service, reporting_context](const ToolCall& call) {
            const PropertyList properties = QueryProperties().with_values(call.arguments);
            const auto start = ParseDateStart(properties);
            const auto end = ParseDateEnd(properties);
            if (properties.value<std::string>("start_date").has_value() && !start.has_value()) return FailureOutput("start_date 格式必须是 YYYY-MM-DD");
            if (properties.value<std::string>("end_date").has_value() && !end.has_value()) return FailureOutput("end_date 格式必须是 YYYY-MM-DD");
            if (start.has_value() && end.has_value() && *start > *end) return FailureOutput("start_date 不能晚于 end_date");
            const auto schedule_id = properties.value<int64_t>("schedule_id");
            const auto rule_id = properties.value<int64_t>("rule_id");
            if (schedule_id.has_value() && rule_id.has_value()) return FailureOutput("schedule_id 和 rule_id 不能同时传入；查询 schedule 使用前者，查询周期规则使用后者");
            schedule::QueryScheduleCommand command;
            command.schedule_id = schedule_id;
            command.rule_id = rule_id;
            command.keyword = properties.value<std::string>("keyword");
            command.start_from = start;
            command.start_to = end;
            command.status = ParseStatus(properties.value<std::string>("status").value_or("active"));
            command.limit = 50;
            command.offset = 0;
            const auto result = service.query_schedule(command);
            if (!result.result.ok()) return FailureOutput(result.result.status.message.empty() ? "查询已物化日程失败" : result.result.status.message);
            ToolOutputArray one_time, recurring_schedules;
            for (const auto& item : result.result.value) {
                if (item.rule_id.has_value()) recurring_schedules.emplace_back(MakeToolOutput(schedule_tool_output::ScheduleOutput(item)));
                else one_time.emplace_back(MakeToolOutput(schedule_tool_output::ScheduleOutput(item)));
            }
            ToolOutputArray recurring_rules, future_occurrences, exceptions;
            if (rule_service != nullptr && !schedule_id.has_value()) {
                schedule::QueryScheduleRulesCommand rule_command;
                rule_command.rule_id = rule_id;
                rule_command.keyword = properties.value<std::string>("keyword");
                rule_command.status = command.status;
                rule_command.occurrence_start = start;
                rule_command.occurrence_end = end;
                rule_command.limit = 50;
                const auto rules = rule_service->query_schedule_rules(rule_command);
                if (!rules.status.ok()) return FailureOutput(rules.status.message.empty() ? "查询周期规则失败" : rules.status.message);
                for (const auto& view : rules.rules) {
                    recurring_rules.emplace_back(MakeToolOutput(schedule_tool_output::RuleOutput(view.rule)));
                    for (const auto& item : view.exceptions) {
                        if (exceptions.size() < contracts::im::kMaxScheduleQueryItems &&
                            WithinRange(start, end, item.original_start_time))
                            exceptions.emplace_back(MakeToolOutput(schedule_tool_output::ExceptionOutput(item)));
                    }
                    for (const auto& item : view.upcoming_occurrences) {
                        if (future_occurrences.size() < contracts::im::kMaxScheduleQueryItems &&
                            WithinRange(start, end, item))
                            future_occurrences.emplace_back(MakeToolOutput(schedule_tool_output::FutureOccurrenceOutput(view.rule, item)));
                    }
                }
            }
            const int64_t result_count = static_cast<int64_t>(one_time.size() + recurring_schedules.size() + future_occurrences.size());
            const std::string keyword = properties.value<std::string>("keyword").value_or("");
            const std::string prefix = keyword.empty() ? "查询到" : "根据“" + keyword + "”关键字查询到";
            const std::string message = prefix + " " + std::to_string(one_time.size()) + " 条一次性日程、" +
                                        std::to_string(recurring_rules.size()) + " 条周期规则、" +
                                        std::to_string(recurring_schedules.size()) + " 条周期实例和 " +
                                        std::to_string(future_occurrences.size()) + " 条未来 occurrence";
            const std::string voice_text = FullVoiceScheduleText(one_time, future_occurrences, exceptions);
            const auto schedules_json = ParseOutputJson(ToolOutputValue::Array(one_time));
            const auto future_json = ParseOutputJson(ToolOutputValue::Array(future_occurrences));
            const auto exceptions_json = ParseOutputJson(ToolOutputValue::Array(exceptions));
            if (!schedules_json.has_value() || !future_json.has_value() || !exceptions_json.has_value())
                return FailureOutput("查询结果序列化失败");
            if (reporting_context.runtime != nullptr && reporting_context.runtime->reporting_channel() != nullptr &&
                !reporting_context.runtime->device_id().empty()) {
                contracts::im::ScheduleQueryResultIntent intent;
                intent.schemaVersion = "1";
                intent.businessEventId = "schedule-query:" + call.request_id;
                intent.correlationId = call.request_id;
                intent.userId = reporting_context.runtime->user_id();
                intent.deviceId = reporting_context.runtime->device_id();
                intent.keyword = properties.value<std::string>("keyword");
                intent.status = properties.value<std::string>("status").value_or("active");
                intent.startDate = properties.value<std::string>("start_date");
                intent.endDate = properties.value<std::string>("end_date");
                intent.resultCount = result_count;
                intent.schedules = *schedules_json;
                intent.futureOccurrences = *future_json;
                intent.exceptions = *exceptions_json;
                intent.queriedAt = QueryNowIso();
                const auto report = reporting_context.runtime->reporting_channel()->SubmitScheduleQueryResult(intent);
                const char* report_state = report.status == voicelife::im::ReportStatus::kSubmitted ? "submitted" :
                                           report.status == voicelife::im::ReportStatus::kRetryable ? "retryable_failed" : "failed";
                return SummaryOutput({MakeToolOutput("status", ToolOutputValue::String("success")),
                                      MakeToolOutput("message", ToolOutputValue::String(message)),
                                      MakeToolOutput("result_count", ToolOutputValue::Integer(result_count)),
                                      MakeToolOutput("one_time_schedules", ToolOutputValue::Array(std::move(one_time))),
                                      MakeToolOutput("recurring_rules", ToolOutputValue::Array(std::move(recurring_rules))),
                                      MakeToolOutput("recurring_schedules", ToolOutputValue::Array(std::move(recurring_schedules))),
                                      MakeToolOutput("future_occurrences", ToolOutputValue::Array(std::move(future_occurrences))),
                                      MakeToolOutput("exceptions", ToolOutputValue::Array(std::move(exceptions))),
                                      MakeToolOutput("im_delivery", ToolOutputValue::String(report_state))},
                                     report.status == voicelife::im::ReportStatus::kSubmitted
                                         ? voice_text + "完整结果已通过 IM 提交。"
                                         : voice_text + "IM 结果提交失败，可重试。");
            }
            return SummaryOutput({MakeToolOutput("status", ToolOutputValue::String("success")),
                                  MakeToolOutput("message", ToolOutputValue::String(message)),
                                  MakeToolOutput("result_count", ToolOutputValue::Integer(result_count)),
                                  MakeToolOutput("one_time_schedules", ToolOutputValue::Array(std::move(one_time))),
                                  MakeToolOutput("recurring_rules", ToolOutputValue::Array(std::move(recurring_rules))),
                                  MakeToolOutput("recurring_schedules", ToolOutputValue::Array(std::move(recurring_schedules))),
                                  MakeToolOutput("future_occurrences", ToolOutputValue::Array(std::move(future_occurrences))),
                                  MakeToolOutput("exceptions", ToolOutputValue::Array(std::move(exceptions))),
                                  MakeToolOutput("im_delivery", ToolOutputValue::Null())},
                                 voice_text);
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.update",
        "修改一次性日程或已经物化到 schedule 表的周期实例。必须只传 schedule_id；不要传 rule_id、original_start_time 或周期规则字段。至少传入一个要修改的字段。",
        UpdateProperties(), [&service, reminder_service, once_update_command](const PropertyList& properties) {
            const auto [maybe_command, error] = once_update_command(properties);
            if (!maybe_command.has_value()) return FailureOutput(error);
            const auto result = service.update_schedule(*maybe_command);
            if (!result.result.ok() || !result.result.value.has_value()) {
                const std::string message = result.result.status.message.empty() ? "日程修改失败" : result.result.status.message;
                if (result.result.status.code == ErrorCode::kConflict) return ConflictOutput(message, schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                return FailureOutput(message);
            }
            const auto& saved = *result.result.value;
            if (const auto reminder = SynchronizeReminder(reminder_service, saved.id); reminder.has_value()) return *reminder;
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String("已修改 schedule_id=" + std::to_string(saved.id) + " 的日程")),
                           MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(saved)),
                           MakeToolOutput("conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
    if (!status.ok()) return status;

    if (rule_service != nullptr) {
        status = server.add_tool(
        "schedule.update_occurrence",
        "修改未来周期中的某一次尚未物化 occurrence。必须传 rule_id + original_start_time；这两个字段只用于定位周期规则中的某一天，不能用于一次性日程或已物化实例。已物化时请改用 schedule.update。至少传入一个覆盖字段。",
        UpdateOccurrenceProperties(), [rule_service](const PropertyList& properties) {
            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法修改 occurrence");
            const auto original = schedule_tool_output::ParseDateTime(properties.value<std::string>("original_start_time").value_or(""));
            if (!original.has_value()) return FailureOutput("original_start_time 必须是严格的 YYYY-MM-DD HH:mm:ss 完整本地时间");
            schedule::UpdateScheduleOccurrenceCommand command;
            command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
            command.original_start_time = *original;
            if (properties.value<std::string>("event").has_value()) command.event = *properties.value<std::string>("event");
            if (properties.value<std::string>("location").has_value()) command.location = *properties.value<std::string>("location");
            if (properties.value<std::string>("notes").has_value()) command.notes = *properties.value<std::string>("notes");
            if (properties.value<std::string>("start_time").has_value()) {
                const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("start_time"));
                if (!parsed.has_value()) return FailureOutput("start_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                command.start_time = *parsed;
            }
            if (properties.value<std::string>("end_time").has_value()) {
                const auto parsed = schedule_tool_output::ParseDateTime(*properties.value<std::string>("end_time"));
                if (!parsed.has_value()) return FailureOutput("end_time 格式必须是 YYYY-MM-DD HH:mm:ss");
                command.end_time = *parsed;
            }
            command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
            const auto result = rule_service->update_schedule_occurrence(command);
            if (!result.status.ok()) {
                std::string message = result.status.message.empty() ? "未来 occurrence 修改失败" : result.status.message;
                if (result.status.code == ErrorCode::kConflict) message += "；如果该 occurrence 已物化，请先查询并改用 schedule.update 的 schedule_id";
                return FailureOutput(message);
            }
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String("已修改周期规则 rule_id=" + std::to_string(command.rule_id) + " 在 " + properties.value<std::string>("original_start_time").value() + " 的未来 occurrence")),
                           MakeToolOutput("exception", result.exception.has_value() ? schedule_tool_output::ExceptionOutput(*result.exception) : ToolOutputValue::Null()),
                           MakeToolOutput("conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
        if (!status.ok()) return status;
    }

    if (rule_service != nullptr) {
        status = server.add_tool(
        "schedule.update_rule",
        "修改整条周期规则并按新规则重建未来实例。必须只传 rule_id；不要传 schedule_id 或 original_start_time。除 rule_id 外至少传入一个规则字段。",
        UpdateRuleProperties(), [rule_service, reminder_service](const PropertyList& properties) {
            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法修改规则");
            const ParsedRepeat parsed = ParseRuleProperties(properties, false);
            if (!parsed.ok()) return FailureOutput(parsed.error);
            if (const auto suspended = SuspendRuleReminders(reminder_service, properties.value<int64_t>("rule_id").value_or(0)); suspended.has_value()) return *suspended;
            const auto result = rule_service->update_schedule_rule(UpdateRuleCommand(properties, parsed));
            if (!result.status.ok()) {
                const std::string message = result.status.message.empty() ? "周期规则修改失败" : result.status.message;
                if (result.status.code == ErrorCode::kConflict)
                    return ConflictOutput(message, schedule_tool_output::ScheduleArrayOutput(result.conflicts));
                return FailureOutput(message);
            }
            if (!result.rule.has_value()) return FailureOutput("周期规则修改失败：服务未返回已保存规则");
            if (const auto reminder = SynchronizeRule(reminder_service, result.rule->id); reminder.has_value()) return *reminder;
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String("已修改周期规则 rule_id=" + std::to_string(result.rule->id))),
                           MakeToolOutput("rule", schedule_tool_output::RuleOutput(*result.rule)),
                           MakeToolOutput("first_schedule", result.schedules.empty() ? ToolOutputValue::Null() : schedule_tool_output::ScheduleOutput(result.schedules.front(), result.rule.has_value() ? &*result.rule : nullptr)),
                           MakeToolOutput("conflicts", ToolOutputValue::Array(schedule_tool_output::ScheduleArrayOutput(result.conflicts))),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
        if (!status.ok()) return status;
    }

    status = server.add_tool(
        "schedule.delete",
        "取消一次性日程或已经物化到 schedule 表的周期实例。必须传 schedule_id、expected_event、expected_start_time；这三个字段用于确认具体记录。不要传 rule_id 或 original_start_time。",
        DeleteProperties(), [&service, reminder_service](const PropertyList& properties) {
            const schedule::ScheduleId id = properties.value<int64_t>("schedule_id").value_or(0);
            schedule::QueryScheduleCommand query;
            query.schedule_id = id;
            query.status = schedule::ScheduleStatusFilter::kAll;
            query.limit = 1;
            const auto loaded = service.query_schedule(query);
            if (!loaded.result.ok() || loaded.result.value.empty()) return FailureOutput("找不到 schedule_id=" + std::to_string(id) + " 对应的日程");
            if (const auto check = VerifyCancellationTarget(loaded.result.value.front(), properties); check.has_value()) return *check;
            const auto result = service.cancel_schedule({.schedule_id = id});
            if (!result.result.ok()) return FailureOutput(result.result.status.message.empty() ? "日程取消失败" : result.result.status.message);
            if (const auto reminder = CancelReminder(reminder_service, id); reminder.has_value()) return *reminder;
            Schedule cancelled = loaded.result.value.front();
            cancelled.status = schedule::ScheduleStatus::kCancelled;
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String("已取消 schedule_id=" + std::to_string(id) + " 的日程")),
                           MakeToolOutput("schedule", schedule_tool_output::ScheduleOutput(cancelled)),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
    if (!status.ok()) return status;

    if (rule_service != nullptr) {
        status = server.add_tool(
        "schedule.delete_rule",
        "取消整条周期规则及其已物化实例，并停止后续 occurrence 生成。必须只传 rule_id；不要传 schedule_id 或 original_start_time。",
        DeleteRuleProperties(), [rule_service, reminder_service](const PropertyList& properties) {
            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法取消规则");
            const auto id = properties.value<int64_t>("rule_id").value_or(0);
            const auto result = rule_service->cancel_schedule_rule({.rule_id = id});
            if (!result.status.ok()) return FailureOutput(result.status.message.empty() ? "周期规则取消失败" : result.status.message);
            if (const auto reminder = SuspendRuleReminders(reminder_service, id); reminder.has_value()) return *reminder;
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String("已取消周期规则 rule_id=" + std::to_string(id) + "，后续 occurrence 将不再生成")),
                           MakeToolOutput("rule", result.rule.has_value() ? schedule_tool_output::RuleOutput(*result.rule) : ToolOutputValue::Null()),
                           MakeToolOutput("cancelled_schedule_count", ToolOutputValue::Integer(result.cancelled_count)),
                           MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
        });
        if (!status.ok()) return status;
    }

    if (rule_service != nullptr) {
        status = server.add_tool(
        "schedule.skip_occurrence",
        "跳过未来周期中的某一次尚未物化 occurrence，实际写入 schedule_rule_exception，而不是删除周期规则。必须传 rule_id + original_start_time + expected_event；不要传 schedule_id。已物化时请改用 schedule.delete。",
        SkipOccurrenceProperties(), [rule_service](const PropertyList& properties) {
            if (rule_service == nullptr) return FailureOutput("当前运行时未启用周期日程能力，无法跳过 occurrence");
            const auto original = schedule_tool_output::ParseDateTime(properties.value<std::string>("original_start_time").value_or(""));
            if (!original.has_value()) return FailureOutput("original_start_time 必须是严格的 YYYY-MM-DD HH:mm:ss 完整本地时间");
            schedule::SkipScheduleOccurrenceCommand command{.rule_id = properties.value<int64_t>("rule_id").value_or(0), .original_start_time = *original};
            const auto result = rule_service->skip_schedule_occurrence(command);
            if (!result.status.ok()) return FailureOutput(result.status.message.empty() ? "跳过未来 occurrence 失败；如果已物化请改用 schedule.delete" : result.status.message);
            if (result.exception.has_value() && result.exception->type == schedule::ExceptionType::kSkip) {
                return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                               MakeToolOutput("message", ToolOutputValue::String("已跳过周期规则 rule_id=" + std::to_string(command.rule_id) + " 在 " + properties.value<std::string>("original_start_time").value() + " 的 occurrence")),
                               MakeToolOutput("exception", schedule_tool_output::ExceptionOutput(*result.exception)),
                               MakeToolOutput("warnings", ToolOutputValue::Array(ToolOutputArray{}))});
            }
            return FailureOutput("跳过 occurrence 未返回有效 exception");
        });
        if (!status.ok()) return status;
    }

    // 操作记录与提醒交互工具保持独立；仅在装配相应服务时公开。
    if (operation_service == nullptr) return Status::Ok();

    status = server.add_tool(
        "schedule.operation_query", "查询最近的操作记录，支持按对象类型、操作类型和名称筛选。",
        OperationQueryProperties(), [operation_service](const PropertyList& properties) {
            schedule::QueryOperationCommand command;
            const auto entity_type = properties.value<std::string>("entity_type");
            if (entity_type.has_value()) {
                const auto parsed = ParseEntityType(*entity_type);
                if (!parsed.has_value()) return FailureOutput("entity_type 取值为 schedule、rule、exception");
                command.entity_type = parsed;
            }
            const auto type = properties.value<std::string>("type");
            if (type.has_value()) {
                const auto parsed = ParseOperationType(*type);
                if (!parsed.has_value()) return FailureOutput("type 取值为 create、update、delete");
                command.type = parsed;
            }
            command.keyword = properties.value<std::string>("keyword");
            const DateTime now = Now();
            command.operated_from = now - std::chrono::minutes{15};
            command.operated_to = now;
            command.limit = 50;
            const auto result = operation_service->query_operations(command);
            if (!result.result.ok()) return FailureOutput(result.result.status.message.empty() ? "操作记录查询失败" : result.result.status.message);
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")),
                           MakeToolOutput("message", ToolOutputValue::String("已查询到 " + std::to_string(result.total) + " 条操作记录")),
                           MakeToolOutput("total", ToolOutputValue::Integer(result.total)),
                           MakeToolOutput("operations", ToolOutputValue::Array(schedule_tool_output::OperationArrayOutput(result.result.value)))});
        });
    if (!status.ok()) return status;

    status = server.add_tool(
        "schedule.reminder_acknowledge",
        "当用户明确确认已获知提醒内容时调用。批量处理最近 10 分钟内已触发但未确认的提醒。",
        PropertyList{}, [reminder_service, reporting_context](const PropertyList&) {
            if (reminder_service == nullptr) return FailureOutput("当前运行时未启用提醒能力");
            const auto result = reminder_service->ExecuteRecentReminderActions(schedule::ScheduleReminderActionKind::kAcknowledge);
            if (!result.ok()) return FailureOutput(result.status.message.empty() ? "确认提醒失败" : result.status.message);
            ToolOutputArray events;
            for (const auto& action : *result.value) for (const auto& event : action.events) events.emplace_back(MakeToolOutput(ToolOutputValue::String(event)));
            const bool reported = ReportVoiceActionResults(*result.value, reporting_context);
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")), MakeToolOutput("message", ToolOutputValue::String("已确认提醒")), MakeToolOutput("affected_count", ToolOutputValue::Integer(static_cast<int64_t>(result.value->size()))), MakeToolOutput("events", ToolOutputValue::Array(std::move(events))), MakeToolOutput("im_delivery", ToolOutputValue::String(reported ? "submitted" : "retryable_failed"))});
        });
    if (!status.ok()) return status;
    return server.add_tool(
        "schedule.reminder_snooze", "当用户表达延迟提醒意图时调用，为当前已触发提醒单独注册一次稍后提醒。",
        PropertyList{}, [reminder_service, reporting_context](const PropertyList&) {
            if (reminder_service == nullptr) return FailureOutput("当前运行时未启用提醒能力");
            const auto result = reminder_service->ExecuteRecentReminderActions(schedule::ScheduleReminderActionKind::kSnooze);
            if (!result.ok()) return FailureOutput(result.status.message.empty() ? "延迟提醒失败" : result.status.message);
            const bool reported = ReportVoiceActionResults(*result.value, reporting_context);
            return Output({MakeToolOutput("status", ToolOutputValue::String("success")), MakeToolOutput("message", ToolOutputValue::String("已延迟提醒")), MakeToolOutput("affected_count", ToolOutputValue::Integer(static_cast<int64_t>(result.value->size()))), MakeToolOutput("im_delivery", ToolOutputValue::String(reported ? "submitted" : "retryable_failed"))});
        });
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service) {
    return RegisterScheduleMcpTools(server, service, nullptr, nullptr, nullptr, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service, nullptr, nullptr, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service, &operation_service, nullptr, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service,
                                schedule::ScheduleReminderService* reminder_service) {
    return RegisterScheduleMcpTools(server, service, &rule_service, &operation_service, reminder_service, {});
}

Status RegisterScheduleMcpTools(McpServer& server, ScheduleService& service, ScheduleRuleService& rule_service,
                                schedule::ScheduleOperationService& operation_service,
                                schedule::ScheduleReminderService* reminder_service,
                                ScheduleQueryReportingContext reporting_context) {
    return RegisterScheduleMcpTools(server, service, &rule_service, &operation_service, reminder_service,
                                    std::move(reporting_context));
}

}  // namespace voicelife::mcp
