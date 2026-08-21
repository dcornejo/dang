// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_notifications.h"

#include "yang/resource_limits.h"

#include <charconv>
#include <ctime>
#include <deque>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <utility>

#include <pugixml.hpp>

#include "yang/netconf_filter.h"

namespace yang::netconf {
namespace {
constexpr std::string_view kNotificationNamespace =
    "urn:ietf:params:xml:ns:netconf:notification:1.0";
constexpr std::string_view kManagementNamespace =
    "urn:ietf:params:xml:ns:netmod:notification";

std::string_view LocalName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string NamespaceFor(const pugi::xml_node& node) {
  const std::string_view name = node.name();
  const std::size_t colon = name.find(':');
  const std::string attribute = colon == std::string_view::npos
      ? "xmlns" : "xmlns:" + std::string(name.substr(0, colon));
  for (pugi::xml_node current = node; current; current = current.parent()) {
    if (const pugi::xml_attribute binding = current.attribute(attribute.c_str()))
      return binding.value();
  }
  return {};
}

std::optional<int> Number(std::string_view value) {
  int result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size()
             ? std::optional(result) : std::nullopt;
}

std::string Wrap(std::string_view content,
                 std::chrono::system_clock::time_point time) {
  return "<notification xmlns=\"" + std::string(kNotificationNamespace) +
      "\"><eventTime>" + FormatNotificationTime(time) + "</eventTime>" +
      std::string(content) + "</notification>";
}

std::string Marker(std::string_view name,
                   std::chrono::system_clock::time_point time) {
  return Wrap("<" + std::string(name) + " xmlns=\"" +
                  std::string(kManagementNamespace) + "\"/>", time);
}

bool MatchesFilter(std::string_view notification, std::string_view filter) {
  pugi::xml_document parsed;
  if (!parsed.load_buffer(filter.data(), filter.size())) return false;
  const std::string_view type = parsed.document_element().attribute("type").value();
  FilterResult result = type == "xpath"
      ? ApplyXPathFilter(notification, filter)
      : ApplySubtreeFilter(notification, filter);
  if (!result.xml) return false;
  pugi::xml_document selected;
  if (!selected.load_string(result.xml->c_str())) return false;
  const pugi::xml_node root = selected.document_element();
  for (const pugi::xml_node child : root.children()) {
    const std::string_view name = child.name();
    if (name != "eventTime" && !name.ends_with(":eventTime")) return true;
  }
  return false;
}

bool ValidFilter(std::string_view filter) {
  if (filter.size() > DefaultResourceLimits().maximum_xml_bytes) return false;
  pugi::xml_document parsed;
  if (!parsed.load_buffer(filter.data(), filter.size())) return false;
  const pugi::xml_node root = parsed.document_element();
  const std::string_view type = root.attribute("type").value();
  if (!type.empty() && type != "subtree" && type != "xpath") return false;
  const std::string sample =
      "<notification xmlns=\"" + std::string(kNotificationNamespace) +
      "\"><eventTime>1970-01-01T00:00:00Z</eventTime>"
      "<sample xmlns=\"urn:sample\"/></notification>";
  const FilterResult result = type == "xpath"
      ? ApplyXPathFilter(sample, filter) : ApplySubtreeFilter(sample, filter);
  return !result.error.has_value();
}

struct InstanceSelector {
  config::QualifiedXmlName name;
  std::vector<std::pair<config::QualifiedXmlName, std::string>> keys;
};

std::optional<config::QualifiedXmlName> ExpandedName(std::string_view value) {
  if (value.empty() || value.front() != '{') return std::nullopt;
  const std::size_t close = value.find('}');
  if (close == std::string_view::npos || close + 1 == value.size())
    return std::nullopt;
  return config::QualifiedXmlName{std::string(value.substr(1, close - 1)),
                                  std::string(value.substr(close + 1))};
}

std::optional<std::vector<std::string_view>> SplitInstancePath(
    std::string_view path) {
  if (path.empty() || path.front() != '/') return std::nullopt;
  std::vector<std::string_view> segments;
  std::size_t begin = 1;
  int brackets = 0;
  char quote = 0;
  for (std::size_t index = begin; index <= path.size(); ++index) {
    const char character = index == path.size() ? '/' : path[index];
    if (quote != 0) {
      if (character == quote) quote = 0;
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = character;
    } else if (character == '[') {
      ++brackets;
    } else if (character == ']') {
      if (--brackets < 0) return std::nullopt;
    } else if (character == '/' && brackets == 0) {
      if (index == begin) return std::nullopt;
      segments.push_back(path.substr(begin, index - begin));
      begin = index + 1;
    }
  }
  if (quote != 0 || brackets != 0) return std::nullopt;
  return segments;
}

std::optional<InstanceSelector> ParseSelector(std::string_view segment) {
  const std::size_t predicate = segment.find('[');
  auto name = ExpandedName(segment.substr(0, predicate));
  if (!name) return std::nullopt;
  InstanceSelector result{std::move(*name), {}};
  std::size_t position = predicate;
  while (position != std::string_view::npos && position < segment.size()) {
    if (segment[position] != '[') return std::nullopt;
    const std::size_t equals = segment.find('=', position + 1);
    if (equals == std::string_view::npos) return std::nullopt;
    auto key = ExpandedName(segment.substr(position + 1,
                                            equals - position - 1));
    if (!key || equals + 1 >= segment.size()) return std::nullopt;
    const char quote = segment[equals + 1];
    if (quote != '\'' && quote != '"') return std::nullopt;
    const std::size_t value_end = segment.find(quote, equals + 2);
    if (value_end == std::string_view::npos || value_end + 1 >= segment.size() ||
        segment[value_end + 1] != ']') return std::nullopt;
    result.keys.emplace_back(
        std::move(*key),
        std::string(segment.substr(equals + 2, value_end - equals - 2)));
    position = value_end + 2;
    if (position == segment.size()) break;
  }
  return result;
}

bool InstanceExists(std::string_view data_xml,
                    std::span<const InstanceSelector> selectors) {
  pugi::xml_document document;
  if (!document.load_buffer(data_xml.data(), data_xml.size())) return false;
  std::vector<pugi::xml_node> candidates{document.document_element()};
  for (const InstanceSelector& selector : selectors) {
    std::vector<pugi::xml_node> matches;
    for (const pugi::xml_node parent : candidates) {
      for (const pugi::xml_node child : parent.children()) {
        if (child.type() != pugi::node_element ||
            NamespaceFor(child) != selector.name.namespace_uri ||
            LocalName(child.name()) != selector.name.local_name) continue;
        const bool keys_match = std::ranges::all_of(
            selector.keys, [&](const auto& expected) {
              for (const pugi::xml_node key : child.children()) {
                if (key.type() == pugi::node_element &&
                    NamespaceFor(key) == expected.first.namespace_uri &&
                    LocalName(key.name()) == expected.first.local_name &&
                    key.text().as_string() == expected.second) return true;
              }
              return false;
            });
        if (keys_match) matches.push_back(child);
      }
    }
    if (matches.empty()) return false;
    candidates = std::move(matches);
  }
  return true;
}
}  // namespace

NotificationManager::NotificationManager(const NacmPolicy* nacm,
    std::size_t maximum_queued_events, std::size_t maximum_queued_bytes,
    const config::RuntimeSchema* schema)
    : nacm_(nacm), schema_(schema), maximum_queued_events_(maximum_queued_events),
      maximum_queued_bytes_(maximum_queued_bytes) {}

bool NotificationManager::AddStream(NotificationStreamConfig stream) {
  if (stream.name.empty() || stream.replay_event_limit == 0) return false;
  const std::string name = stream.name;
  std::lock_guard lock(mutex_);
  return streams_.emplace(name, Stream{std::move(stream), {}}).second;
}

void NotificationManager::SetInstanceDataProvider(
    std::function<std::string()> provider) {
  std::lock_guard lock(mutex_);
  instance_data_provider_ = std::move(provider);
}

SubscriptionResult NotificationManager::Subscribe(
    SubscriptionRequest request, std::chrono::system_clock::time_point now) {
  std::lock_guard lock(mutex_);
  const std::optional<NacmPolicy> policy_snapshot =
      nacm_ == nullptr ? std::nullopt : std::optional<NacmPolicy>(*nacm_);
  const NacmPolicy* const nacm =
      policy_snapshot ? &*policy_snapshot : nullptr;
  if (request.session_id == 0) return {false, "invalid session", "invalid-value", ""};
  if (subscriptions_.contains(request.session_id))
    return {false, "a subscription already exists", "resource-denied", ""};
  const auto stream = streams_.find(request.stream);
  if (stream == streams_.end())
    return {false, "unknown event stream", "invalid-value", "stream"};
  if (request.stop_time && !request.start_time)
    return {false, "stopTime requires startTime", "missing-element", "stopTime"};
  if (request.start_time && *request.start_time > now)
    return {false, "startTime is in the future", "bad-element", "startTime"};
  if (request.stop_time && *request.stop_time <= *request.start_time)
    return {false, "stopTime must be later than startTime", "bad-element", "stopTime"};
  if (request.start_time && !stream->second.config.replay_supported)
    return {false, "stream does not support replay", "operation-not-supported", "startTime"};
  if (request.filter_xml && !ValidFilter(*request.filter_xml))
    return {false, "notification filter is invalid", "invalid-value", "filter"};
  Subscription subscription{std::move(request), {}, 0, false};
  if (subscription.request.start_time) {
    for (const Event& event : stream->second.replay) {
      if (event.time < *subscription.request.start_time ||
          (subscription.request.stop_time && event.time > *subscription.request.stop_time)) continue;
      if (nacm && !nacm->AuthorizeNotification(
              subscription.request.username, event.module, event.name,
              event.instance_path, event.ancestors,
              subscription.request.external_groups,
              event.default_deny_all)) continue;
      if (subscription.request.filter_xml &&
          !MatchesFilter(event.xml, *subscription.request.filter_xml)) continue;
      subscription.queued_bytes += event.xml.size();
      subscription.queue.push_back(event.xml);
      if (subscription.queue.size() > maximum_queued_events_ ||
          subscription.queued_bytes > maximum_queued_bytes_) {
        return {false, "replay exceeds subscription queue limits",
                "resource-denied", "startTime"};
      }
    }
    std::string complete = Marker("replayComplete", now);
    subscription.queued_bytes += complete.size();
    subscription.queue.push_back(std::move(complete));
    if (subscription.queue.size() > maximum_queued_events_ ||
        subscription.queued_bytes > maximum_queued_bytes_) {
      return {false, "replay exceeds subscription queue limits",
              "resource-denied", "startTime"};
    }
  }
  subscriptions_.emplace(subscription.request.session_id, std::move(subscription));
  return {true, {}, {}, {}};
}

bool NotificationManager::Publish(std::string_view stream_name,
    std::string_view module_name, std::string_view notification_name,
    std::string_view content_xml, std::chrono::system_clock::time_point event_time,
    bool default_deny_all, std::string_view instance_path) {
  if (content_xml.size() > DefaultResourceLimits().maximum_xml_bytes) {
    return false;
  }
  pugi::xml_document content;
  if (!content.load_buffer(content_xml.data(), content_xml.size()) ||
      !content.document_element() ||
      content.document_element().next_sibling()) return false;
  std::string resource_error;
  if (!XmlWithinResourceLimits(content, content_xml, DefaultResourceLimits(),
                               &resource_error)) {
    return false;
  }
  std::vector<NacmDataNode> ancestors;
  if (schema_ != nullptr && !instance_path.empty()) {
    const auto path_segments = SplitInstancePath(instance_path);
    if (!path_segments) return false;
    const std::vector<config::RuntimeSchemaNodeId> resolved =
        schema_->ResolveInstancePath(instance_path);
    if (resolved.empty() || resolved.size() != path_segments->size())
      return false;
    const config::RuntimeSchemaNode& event_schema = schema_->Get(resolved.back());
    if (event_schema.kind != semantic::SchemaNodeKind::kNotification ||
        event_schema.module_name != module_name ||
        event_schema.name.local_name != notification_name ||
        event_schema.name.namespace_uri !=
            NamespaceFor(content.document_element()) ||
        event_schema.name.local_name !=
            LocalName(content.document_element().name())) return false;
    default_deny_all = default_deny_all || event_schema.nacm_default_deny_all;
    std::vector<InstanceSelector> selectors;
    std::size_t path_end = 0;
    for (std::size_t index = 0; index + 1 < resolved.size(); ++index) {
      const config::RuntimeSchemaNode& metadata = schema_->Get(resolved[index]);
      auto selector = ParseSelector(path_segments->at(index));
      if (!selector || selector->name != metadata.name) return false;
      for (config::RuntimeSchemaNodeId key_id : metadata.keys) {
        const auto& key = schema_->Get(key_id).name;
        if (std::ranges::count_if(selector->keys, [&](const auto& predicate) {
              return predicate.first == key;
            }) != 1)
          return false;
      }
      if (selector->keys.size() != metadata.keys.size()) return false;
      selectors.push_back(std::move(*selector));
      path_end += path_segments->at(index).size() + 1;
      ancestors.push_back({metadata.module_name,
                           std::string(instance_path.substr(0, path_end)),
                           metadata.nacm_default_deny_all});
    }
    std::function<std::string()> instance_provider;
    {
      std::lock_guard lock(mutex_);
      instance_provider = instance_data_provider_;
    }
    if (!instance_provider) return false;
    try {
      if (!InstanceExists(instance_provider(), selectors)) return false;
    } catch (...) {
      return false;
    }
  } else if (schema_ != nullptr) {
    const pugi::xml_node root = content.document_element();
    const auto metadata = schema_->FindTopLevelOperation(
        {NamespaceFor(root), std::string(LocalName(root.name()))},
        semantic::SchemaNodeKind::kNotification);
    if (!metadata) return false;
    const config::RuntimeSchemaNode& event_schema = schema_->Get(*metadata);
    if (event_schema.module_name != module_name ||
        event_schema.name.local_name != notification_name) return false;
    default_deny_all = default_deny_all ||
        event_schema.nacm_default_deny_all;
  }
  const std::string xml = Wrap(content_xml, event_time);
  const std::optional<NacmPolicy> policy_snapshot =
      nacm_ == nullptr ? std::nullopt : std::optional<NacmPolicy>(*nacm_);
  const NacmPolicy* const nacm =
      policy_snapshot ? &*policy_snapshot : nullptr;
  std::lock_guard lock(mutex_);
  const auto stream = streams_.find(stream_name);
  if (stream == streams_.end()) return false;
  Event event{event_time, std::string(module_name), std::string(notification_name),
              xml, default_deny_all, std::string(instance_path),
              std::move(ancestors)};
  if (stream->second.config.replay_supported) {
    stream->second.replay.push_back(event);
    while (stream->second.replay.size() > stream->second.config.replay_event_limit)
      stream->second.replay.pop_front();
  }
  for (auto& [id, subscription] : subscriptions_) {
    (void)id;
    if (subscription.terminated || subscription.request.stream != stream_name ||
        (subscription.request.start_time &&
         event_time < *subscription.request.start_time) ||
        (subscription.request.stop_time && event_time > *subscription.request.stop_time)) continue;
    if (nacm && !nacm->AuthorizeNotification(
            subscription.request.username, module_name, notification_name,
            instance_path, event.ancestors,
            subscription.request.external_groups, default_deny_all)) continue;
    if (subscription.request.filter_xml &&
        !MatchesFilter(xml, *subscription.request.filter_xml)) continue;
    if (subscription.queue.size() >= maximum_queued_events_ ||
        subscription.queued_bytes + xml.size() > maximum_queued_bytes_) {
      subscription.queue.clear();
      subscription.queued_bytes = 0;
      subscription.terminated = true;
      continue;
    }
    subscription.queued_bytes += xml.size();
    subscription.queue.push_back(xml);
  }
  return true;
}

std::vector<std::string> NotificationManager::Drain(
    std::uint32_t session_id, std::chrono::system_clock::time_point now) {
  std::lock_guard lock(mutex_);
  const auto found = subscriptions_.find(session_id);
  if (found == subscriptions_.end()) return {};
  Subscription& subscription = found->second;
  if (subscription.request.stop_time && now >= *subscription.request.stop_time &&
      !subscription.terminated) {
    subscription.queue.push_back(Marker("notificationComplete", now));
    subscription.terminated = true;
  }
  std::vector<std::string> result(subscription.queue.begin(), subscription.queue.end());
  subscription.queue.clear();
  subscription.queued_bytes = 0;
  if (subscription.terminated) subscriptions_.erase(found);
  return result;
}

void NotificationManager::RemoveSession(std::uint32_t session_id) {
  std::lock_guard lock(mutex_);
  subscriptions_.erase(session_id);
}
bool NotificationManager::configured() const noexcept {
  std::lock_guard lock(mutex_);
  return streams_.contains("NETCONF");
}

std::optional<std::chrono::system_clock::time_point> ParseNotificationTime(
    std::string_view value) {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':') return std::nullopt;
  auto year = Number(value.substr(0, 4)); auto month = Number(value.substr(5, 2));
  auto day = Number(value.substr(8, 2)); auto hour = Number(value.substr(11, 2));
  auto minute = Number(value.substr(14, 2)); auto second = Number(value.substr(17, 2));
  if (!year || !month || !day || !hour || !minute || !second || *hour > 23 ||
      *minute > 59 || *second > 60) return std::nullopt;
  std::size_t zone = 19;
  while (zone < value.size() && value[zone] != 'Z' && value[zone] != '+' && value[zone] != '-') ++zone;
  if (zone == value.size()) return std::nullopt;
  const std::chrono::year_month_day date{std::chrono::year(*year),
      std::chrono::month(static_cast<unsigned>(*month)),
      std::chrono::day(static_cast<unsigned>(*day))};
  if (!date.ok()) return std::nullopt;
  std::chrono::system_clock::time_point result = std::chrono::sys_days(date);
  result += std::chrono::hours(*hour) + std::chrono::minutes(*minute) +
            std::chrono::seconds(*second);
  if (zone > 19) {
    if (value[19] != '.' || zone == 20 || zone - 20 > 9) return std::nullopt;
    std::int64_t fraction = 0;
    for (std::size_t index = 20; index < zone; ++index) {
      if (value[index] < '0' || value[index] > '9') return std::nullopt;
      fraction = fraction * 10 + (value[index] - '0');
    }
    for (std::size_t digits = zone - 20; digits < 9; ++digits) fraction *= 10;
    result += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(fraction));
  }
  if (value[zone] == 'Z') return zone + 1 == value.size() ? std::optional(result) : std::nullopt;
  if (zone + 6 != value.size() || value[zone + 3] != ':') return std::nullopt;
  auto zh = Number(value.substr(zone + 1, 2)); auto zm = Number(value.substr(zone + 4, 2));
  if (!zh || !zm || *zh > 23 || *zm > 59) return std::nullopt;
  const auto offset = std::chrono::hours(*zh) + std::chrono::minutes(*zm);
  return value[zone] == '+' ? result - offset : result + offset;
}

std::string FormatNotificationTime(std::chrono::system_clock::time_point value) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(value);
  std::tm utc{};
  gmtime_r(&raw, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}
}  // namespace yang::netconf
