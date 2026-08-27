#include "schedule_mcp_tools_input.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "schedule_tool_output.h"

namespace voicelife::mcp::schedule_tool_input {
namespace {

std::optional<schedule::Frequency> ParseFrequency(const std::string& text) {
    if (text == "daily") return schedule::Frequency::kDaily;
    if (text == "weekly") return schedule::Frequency::kWeekly;
    if (text == "monthly") return schedule::Frequency::kMonthly;
    if (text == "yearly") return schedule::Frequency::kYearly;
    return std::nullopt;
}

std::optional<schedule::MonthlyMode> ParseMonthlyMode(const std::string& text) {
    if (text == "specific_day") return schedule::MonthlyMode::kSpecificDay;
    if (text == "last_day") return schedule::MonthlyMode::kLastDay;
    return std::nullopt;
}

bool Has(const PropertyList& properties, std::string_view name) {
    return properties.value<std::string>(std::string(name)).has_value() ||
           properties.value<int64_t>(std::string(name)).has_value() ||
           properties.value<bool>(std::string(name)).has_value();
}

ParsedRepeat ParseFlat(const PropertyList& properties, bool require_anchor) {
    ParsedRepeat parsed;
    const auto frequency = properties.value<std::string>("freq_type");
    if (frequency.has_value()) {
        parsed.freq_type = ParseFrequency(*frequency);
        if (!parsed.freq_type.has_value()) {
            parsed.error = "freq_type 必须是 daily、weekly、monthly 或 yearly；请不要传入其他周期名称";
            return parsed;
        }
    }

    const auto start_time = properties.value<std::string>("start_time");
    if (start_time.has_value()) {
        parsed.start_time = schedule_tool_output::ParseLocalTime(*start_time);
        if (!parsed.start_time.has_value()) {
            parsed.error = "start_time 必须是严格的 HH:mm:ss 格式，例如 08:30:00";
            return parsed;
        }
    }
    const auto end_time = properties.value<std::string>("end_time");
    if (end_time.has_value()) {
        parsed.end_time = schedule_tool_output::ParseLocalTime(*end_time);
        if (!parsed.end_time.has_value()) {
            parsed.error = "end_time 必须是严格的 HH:mm:ss 格式，例如 09:30:00";
            return parsed;
        }
    }
    const auto start_date = properties.value<std::string>("start_date");
    if (start_date.has_value()) {
        parsed.start_date = schedule_tool_output::ParseLocalDate(*start_date);
        if (!parsed.start_date.has_value()) {
            parsed.error = "start_date 必须是严格的 YYYY-MM-DD 格式，例如 2026-08-27";
            return parsed;
        }
    }
    const auto end_date = properties.value<std::string>("end_date");
    if (end_date.has_value()) {
        parsed.end_date = schedule_tool_output::ParseLocalDate(*end_date);
        if (!parsed.end_date.has_value()) {
            parsed.error = "end_date 必须是严格的 YYYY-MM-DD 格式，例如 2026-12-31";
            return parsed;
        }
    }

    const auto interval = properties.value<int64_t>("interval_val");
    if (interval.has_value()) {
        if (*interval < 1 || *interval > std::numeric_limits<int32_t>::max()) {
            parsed.error = "interval_val 必须是 1 到 2147483647 之间的整数";
            return parsed;
        }
        parsed.interval_val = static_cast<int32_t>(*interval);
    }
    const auto weekdays = properties.value<int64_t>("weekdays_mask");
    if (weekdays.has_value()) {
        if (*weekdays < 1 || *weekdays > 127) {
            parsed.error = "weekdays_mask 必须是 1 到 127 之间的整数；仅 weekly 周期需要传入";
            return parsed;
        }
        parsed.weekdays_mask = static_cast<uint8_t>(*weekdays);
    }
    const auto day = properties.value<int64_t>("day_of_month");
    if (day.has_value()) {
        if (*day < 1 || *day > 31) {
            parsed.error = "day_of_month 必须是 1 到 31 之间的整数；按月指定日期时传入";
            return parsed;
        }
        parsed.day_of_month = static_cast<uint8_t>(*day);
    }
    const auto month = properties.value<int64_t>("month_of_year");
    if (month.has_value()) {
        if (*month < 1 || *month > 12) {
            parsed.error = "month_of_year 必须是 1 到 12 之间的整数；yearly 周期需要传入";
            return parsed;
        }
        parsed.month_of_year = static_cast<uint8_t>(*month);
    }
    const auto mode = properties.value<std::string>("monthly_mode");
    if (mode.has_value()) {
        parsed.monthly_mode = ParseMonthlyMode(*mode);
        if (!parsed.monthly_mode.has_value()) {
            parsed.error = "monthly_mode 只能是 specific_day 或 last_day；仅 monthly 周期使用";
            return parsed;
        }
    }
    const auto count = properties.value<int64_t>("occurrence_count");
    if (count.has_value()) {
        if (*count < 1 || *count > std::numeric_limits<int32_t>::max()) {
            parsed.error = "occurrence_count 必须是 1 到 2147483647 之间的整数";
            return parsed;
        }
        parsed.occurrence_count = static_cast<int32_t>(*count);
    }

    if (require_anchor && (!parsed.freq_type.has_value() || !parsed.start_time.has_value() ||
                           !parsed.start_date.has_value())) {
        parsed.error = "创建周期日程时必须传入 freq_type、start_date 和 start_time；这些字段不能省略";
    }
    return parsed;
}

}  // namespace

ParsedRepeat ParseRepeat(const std::optional<JsonValue>& repeat, bool require_anchor) {
    // 仅为旧的内部调用保留解析入口；公开工具不再声明 repeat 对象。
    if (!repeat.has_value()) return {};
    if (!repeat->IsObject()) {
        ParsedRepeat parsed;
        parsed.error = "repeat 必须是对象；新工具请直接传入扁平周期字段";
        return parsed;
    }
    PropertyList properties;
    for (const auto& [key, value] : repeat->object) {
        if (value.kind == JsonValue::Kind::kString) {
            properties.add_property(Property::Optional(key, PropertyType::kString));
        } else if (value.kind == JsonValue::Kind::kNumber && value.number == static_cast<int64_t>(value.number)) {
            properties.add_property(Property::Optional(key, PropertyType::kInteger));
        } else {
            ParsedRepeat parsed;
            parsed.error = "repeat." + key + " 类型无效；请使用公开工具的扁平参数";
            return parsed;
        }
    }
    ToolArguments arguments;
    for (const auto& [key, value] : repeat->object) {
        if (value.kind == JsonValue::Kind::kString) arguments.emplace(key, value.string);
        if (value.kind == JsonValue::Kind::kNumber) arguments.emplace(key, static_cast<int64_t>(value.number));
    }
    return ParseFlat(properties.with_values(arguments), require_anchor);
}

ParsedRepeat ParseRuleProperties(const PropertyList& properties, bool require_anchor) {
    return ParseFlat(properties, require_anchor);
}

schedule::CreateScheduleRuleCommand CreateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat) {
    schedule::CreateScheduleRuleCommand command;
    command.event = properties.value<std::string>("event").value_or("");
    command.location = properties.value<std::string>("location");
    command.notes = properties.value<std::string>("notes");
    command.freq_type = repeat.freq_type.value_or(schedule::Frequency::kDaily);
    command.interval_val = repeat.interval_val.value_or(1);
    command.weekdays_mask = repeat.weekdays_mask;
    command.day_of_month = repeat.day_of_month;
    command.month_of_year = repeat.month_of_year;
    command.monthly_mode = repeat.monthly_mode;
    command.start_time = repeat.start_time.value_or(schedule::LocalTime{});
    command.start_date = repeat.start_date;
    command.end_time = repeat.end_time;
    command.end_date = repeat.end_date;
    command.occurrence_count = repeat.occurrence_count;
    command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
    return command;
}

schedule::UpdateScheduleRuleCommand UpdateRuleCommand(const PropertyList& properties, const ParsedRepeat& repeat) {
    schedule::UpdateScheduleRuleCommand command;
    command.rule_id = properties.value<int64_t>("rule_id").value_or(0);
    command.event = properties.value<std::string>("event");
    if (Has(properties, "location")) command.location = properties.value<std::string>("location");
    if (Has(properties, "notes")) command.notes = properties.value<std::string>("notes");
    if (repeat.freq_type.has_value()) command.freq_type = repeat.freq_type;
    if (repeat.interval_val.has_value()) command.interval_val = repeat.interval_val;
    if (repeat.weekdays_mask.has_value()) command.weekdays_mask = repeat.weekdays_mask;
    if (repeat.day_of_month.has_value()) command.day_of_month = repeat.day_of_month;
    if (repeat.month_of_year.has_value()) command.month_of_year = repeat.month_of_year;
    if (repeat.monthly_mode.has_value()) command.monthly_mode = repeat.monthly_mode;
    if (repeat.start_time.has_value()) command.start_time = repeat.start_time;
    if (repeat.end_time.has_value()) command.end_time = repeat.end_time;
    if (repeat.start_date.has_value()) command.start_date = repeat.start_date;
    if (repeat.end_date.has_value()) command.end_date = repeat.end_date;
    if (repeat.occurrence_count.has_value()) command.occurrence_count = repeat.occurrence_count;
    command.ignore_conflict = properties.value<bool>("ignore_conflict").value_or(false);
    return command;
}

PropertyList CreateProperties() {
    return PropertyList({
        Property("event", PropertyType::kString)
            .with_description("【必填】一次性日程标题。仅用于创建一条独立日程；不要在此工具传入任何周期规则字段"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("【可选】一次性日程开始时间，严格使用 YYYY-MM-DD HH:mm:ss；没有明确时间时不传"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("【可选】一次性日程结束时间，严格使用 YYYY-MM-DD HH:mm:ss；只有同时传 start_time 时才传，且必须晚于 start_time"),
        Property::Optional("location", PropertyType::kString).with_description("【可选】一次性日程地点；没有地点时不传"),
        Property::Optional("notes", PropertyType::kString).with_description("【可选】一次性日程备注；没有备注时不传"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("【可选】是否忽略与已有日程的时间冲突；默认 false。冲突时只有明确允许覆盖才传 true"),
    });
}

PropertyList CreateRuleProperties() {
    return PropertyList({
        Property("event", PropertyType::kString)
            .with_description("【必填】周期日程标题。该工具会创建 schedule_rule，并物化首条 schedule 实例"),
        Property("freq_type", PropertyType::kString)
            .with_description("【必填】周期频率，只能是 daily、weekly、monthly、yearly；不要传不支持的自然语言规则"),
        Property("start_date", PropertyType::kString)
            .with_description("【必填】周期规则开始日期，严格使用 YYYY-MM-DD；这是规则锚点，不是 occurrence 的查询时间"),
        Property("start_time", PropertyType::kString)
            .with_description("【必填】每次周期 occurrence 的开始时间，严格使用 HH:mm:ss"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("【可选】每次 occurrence 的结束时间，严格使用 HH:mm:ss；传入时必须晚于 start_time"),
        Property::Optional("location", PropertyType::kString).with_description("【可选】周期日程地点；没有地点时不传"),
        Property::Optional("notes", PropertyType::kString).with_description("【可选】周期日程备注；没有备注时不传"),
        Property("interval_val", PropertyType::kInteger, int64_t{1})
            .with_description("【可选】重复间隔，默认 1，必须为正整数；例如 daily+1 表示每天，weekly+2 表示每两周"),
        Property::Optional("weekdays_mask", PropertyType::kInteger)
            .with_description("【条件可选】仅 weekly 使用，按位表示星期一至星期日；非 weekly 不要传，范围 1 到 127"),
        Property::Optional("day_of_month", PropertyType::kInteger)
            .with_description("【条件可选】monthly 或 yearly 按固定日期重复时使用，范围 1 到 31；使用 last_day 时不要传"),
        Property::Optional("month_of_year", PropertyType::kInteger)
            .with_description("【条件可选】仅 yearly 使用，表示月份，范围 1 到 12；其他频率不要传"),
        Property::Optional("monthly_mode", PropertyType::kString)
            .with_description("【条件可选】仅 monthly 使用，只能是 specific_day 或 last_day；指定固定日期时配合 day_of_month"),
        Property::Optional("end_date", PropertyType::kString)
            .with_description("【可选】周期结束日期，严格使用 YYYY-MM-DD；不限制结束日期时不传"),
        Property::Optional("occurrence_count", PropertyType::kInteger)
            .with_description("【可选】最多发生次数，必须为正整数；不限制次数时不传"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("【可选】是否忽略首条实例与已有日程的冲突；默认 false，只有明确允许冲突时传 true"),
    });
}

PropertyList QueryProperties() {
    return PropertyList({
        Property::Optional("schedule_id", PropertyType::kInteger)
            .with_description("【条件可选】查询一条已物化 schedule；可指向一次性日程或周期实例。与 rule_id 互斥，不查询未来未物化 occurrence"),
        Property::Optional("rule_id", PropertyType::kInteger)
            .with_description("【条件可选】查询一条周期规则及其已物化实例、未来 occurrence、exception；与 schedule_id 互斥"),
        Property::Optional("keyword", PropertyType::kString)
            .with_description("【可选】按事件标题关键词筛选；不按地点或备注匹配时不要假设会命中"),
        Property::Optional("status", PropertyType::kString)
            .with_description("【可选】状态筛选，只能是 all、active、cancelled、completed；默认 active"),
        Property::Optional("start_date", PropertyType::kString)
            .with_description("【可选】查询窗口起始日期，严格使用 YYYY-MM-DD；不限制起点时不传"),
        Property::Optional("end_date", PropertyType::kString)
            .with_description("【可选】查询窗口结束日期，严格使用 YYYY-MM-DD；不限制终点时不传"),
    });
}

PropertyList UpdateProperties() {
    return PropertyList({
        Property("schedule_id", PropertyType::kInteger)
            .with_description("【必填】要修改的 schedule 表记录 ID。可用于一次性日程或已经物化的周期实例；由 schedule.query 返回。不要传 rule_id 或 original_start_time"),
        Property::Optional("event", PropertyType::kString).with_description("【可选】新的标题；不修改标题时不传"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("【可选】新的完整开始时间，严格使用 YYYY-MM-DD HH:mm:ss；不修改开始时间时不传"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("【可选】新的完整结束时间，严格使用 YYYY-MM-DD HH:mm:ss；不修改结束时间时不传，且必须晚于有效 start_time"),
        Property::Optional("location", PropertyType::kString).with_description("【可选】新的地点；不修改地点时不传"),
        Property::Optional("notes", PropertyType::kString).with_description("【可选】新的备注；不修改备注时不传"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("【可选】是否忽略修改后时间冲突，默认 false；只有明确允许冲突时传 true"),
    });
}

PropertyList UpdateOccurrenceProperties() {
    return PropertyList({
        Property("rule_id", PropertyType::kInteger)
            .with_description("【必填】周期规则 ID；只用于定位未来周期 occurrence，不是一次性日程 ID"),
        Property("original_start_time", PropertyType::kString)
            .with_description("【必填】规则原本生成的那一次 occurrence 的完整本地开始时间，严格使用 YYYY-MM-DD HH:mm:ss；必须与 rule_id 一起传，不能只传 HH:mm:ss"),
        Property::Optional("event", PropertyType::kString).with_description("【可选】仅覆盖这一次 occurrence 的标题；不修改标题时不传"),
        Property::Optional("start_time", PropertyType::kString)
            .with_description("【可选】仅覆盖这一次 occurrence 的完整开始时间，严格使用 YYYY-MM-DD HH:mm:ss；不修改时间时不传"),
        Property::Optional("end_time", PropertyType::kString)
            .with_description("【可选】仅覆盖这一次 occurrence 的完整结束时间，严格使用 YYYY-MM-DD HH:mm:ss；不修改结束时间时不传"),
        Property::Optional("location", PropertyType::kString).with_description("【可选】仅覆盖这一次 occurrence 的地点；不修改地点时不传"),
        Property::Optional("notes", PropertyType::kString).with_description("【可选】仅覆盖这一次 occurrence 的备注；不修改备注时不传"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false})
            .with_description("【可选】是否忽略这一次修改造成的冲突，默认 false"),
    });
}

PropertyList UpdateRuleProperties() {
    return PropertyList({
        Property("rule_id", PropertyType::kInteger)
            .with_description("【必填】要修改的整条周期规则 ID；由 schedule.query 的 recurring_rules 返回。不要传 schedule_id 或 original_start_time"),
        Property::Optional("event", PropertyType::kString).with_description("【可选】新的规则标题；不修改标题时不传"),
        Property::Optional("freq_type", PropertyType::kString).with_description("【可选】新的周期频率，只能是 daily、weekly、monthly、yearly；不修改频率时不传"),
        Property::Optional("start_date", PropertyType::kString).with_description("【可选】新的规则开始日期，严格使用 YYYY-MM-DD；不修改锚点时不传"),
        Property::Optional("start_time", PropertyType::kString).with_description("【可选】新的规则开始时间，严格使用 HH:mm:ss；不修改时间时不传"),
        Property::Optional("end_time", PropertyType::kString).with_description("【可选】新的规则结束时间，严格使用 HH:mm:ss；不修改时间时不传"),
        Property::Optional("location", PropertyType::kString).with_description("【可选】新的规则地点；不修改地点时不传"),
        Property::Optional("notes", PropertyType::kString).with_description("【可选】新的规则备注；不修改备注时不传"),
        Property::Optional("interval_val", PropertyType::kInteger).with_description("【可选】新的正整数重复间隔；不修改间隔时不传"),
        Property::Optional("weekdays_mask", PropertyType::kInteger).with_description("【条件可选】weekly 规则的新星期掩码；非 weekly 不要传，范围 1 到 127"),
        Property::Optional("day_of_month", PropertyType::kInteger).with_description("【条件可选】monthly/yearly 的新固定日期；不使用固定日期时不传"),
        Property::Optional("month_of_year", PropertyType::kInteger).with_description("【条件可选】yearly 的新月份，范围 1 到 12；非 yearly 不要传"),
        Property::Optional("monthly_mode", PropertyType::kString).with_description("【条件可选】monthly 的新模式，只能是 specific_day 或 last_day"),
        Property::Optional("end_date", PropertyType::kString).with_description("【可选】新的规则结束日期，严格使用 YYYY-MM-DD；不修改时不传"),
        Property::Optional("occurrence_count", PropertyType::kInteger).with_description("【可选】新的最大发生次数；不修改时不传"),
        Property("ignore_conflict", PropertyType::kBoolean, bool{false}).with_description("【可选】是否忽略规则重建后的冲突，默认 false"),
    });
}

PropertyList DeleteProperties() {
    return PropertyList({
        Property("schedule_id", PropertyType::kInteger)
            .with_description("【必填】要取消的 schedule 表记录 ID，可指向一次性日程或已物化周期实例；由 schedule.query 返回。不要传 rule_id 或 original_start_time"),
        Property("expected_event", PropertyType::kString)
            .with_description("【必填】删除前必须从 schedule.query 原样回传该记录的 event，用于确认不会取消错误目标"),
        Property("expected_start_time", PropertyType::kString)
            .with_description("【必填】删除前必须从 schedule.query 原样回传该记录的 start_time；无开始时间的记录不能通过此确认工具取消"),
    });
}

PropertyList DeleteRuleProperties() {
    return PropertyList({
        Property("rule_id", PropertyType::kInteger)
            .with_description("【必填】要取消的整条周期规则 ID；由 recurring_rules 查询结果返回。会停止后续 occurrence，不要传 schedule_id 或 original_start_time"),
    });
}

PropertyList SkipOccurrenceProperties() {
    return PropertyList({
        Property("rule_id", PropertyType::kInteger)
            .with_description("【必填】周期规则 ID；只用于定位一个未来 occurrence"),
        Property("original_start_time", PropertyType::kString)
            .with_description("【必填】要跳过的原始 occurrence 完整本地开始时间，严格使用 YYYY-MM-DD HH:mm:ss；不是规则的 HH:mm:ss 时间部分"),
        Property("expected_event", PropertyType::kString)
            .with_description("【必填】从 schedule.query 的 future_occurrences 原样回传 event，用于确认跳过的是正确 occurrence"),
    });
}

PropertyList OperationQueryProperties() {
    return PropertyList({
        Property::Optional("entity_type", PropertyType::kString)
            .with_description("【可选】操作对象类型，只能是 schedule、rule、exception；不筛选对象类型时不传"),
        Property::Optional("type", PropertyType::kString)
            .with_description("【可选】操作类型，只能是 create、update、delete；不筛选操作类型时不传"),
        Property::Optional("keyword", PropertyType::kString).with_description("【可选】按操作对象名称筛选；不筛选名称时不传"),
    });
}

}  // namespace voicelife::mcp::schedule_tool_input
