#include <dxfeed_graal_cpp_api/api.hpp>
#include <expected>
#include <iostream>

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;
using i64 = std::int64_t;
using i32 = std::int32_t;
using i16 = std::int16_t;
using i8 = std::int8_t;

namespace patterns {
enum class ParseError { UNKNOWN_PATTERN };

struct Text {
  std::string value;

  static std::expected<Text, ParseError> parse(std::string_view) {
    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};

// @@N{0..999} -> 0, 1, 2 ... 999
// @@N{0..999,2} -> 0, 2, 4 ... 998
// @@N{0..999,-2} -> UNKNOWN_PATTERN
// @@N{000..999} -> 000, 001, 002 ... 999
// @@N{000..999,3} -> 000, 003, 006 ... 999
struct NumberPattern {
  i64 min;
  i64 max;
  i64 step;
  i64 digits;  // 0 - dynamic

  static std::expected<NumberPattern, ParseError> parse(std::string_view text) {
    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};

struct SymbolNamePattern {
  using Part = std::variant<Text, NumberPattern>;

  std::vector<Part> parts;

  static std::expected<SymbolNamePattern, ParseError> parse(std::string_view text) {
    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};

struct QuotePattern {
  static std::expected<QuotePattern, ParseError> parse(std::string_view text) {
    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};

struct TradePattern {
  static std::expected<TradePattern, ParseError> parse(std::string_view text) {
    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};

struct SummaryPattern {
  static std::expected<SummaryPattern, ParseError> parse(std::string_view text) {
    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};

struct SubscriptionPattern {
  using EventPattern = std::variant<QuotePattern, TradePattern, SummaryPattern>;

  SymbolNamePattern symbolNamePattern;
  std::vector<EventPattern> eventPatterns;

  static std::expected<SubscriptionPattern, ParseError> parse(std::string_view text) {
    const auto parts = dxfcpp::splitStr(text, ';');

    if (parts.size() == 3 && parts[0] == "SUB") {
      auto r = SymbolNamePattern::parse(parts[1]);

      if (!r) {
        return std::unexpected(r.error());
      }

      const auto eventPatternParts = dxfcpp::splitStr(parts[2], ',');

      if (eventPatternParts.empty()) {
        return std::unexpected(ParseError::UNKNOWN_PATTERN);
      }

      std::vector<EventPattern> eventPatterns{};

      for (auto& eventPattern : eventPatternParts) {
        if (auto r = QuotePattern::parse(eventPattern); r.has_value()) {
          eventPatterns.emplace_back(*r);
        } else if (auto r = TradePattern::parse(eventPattern); r.has_value()) {
          eventPatterns.emplace_back(*r);
        } else if (auto r = SummaryPattern::parse(eventPattern); r.has_value()) {
          eventPatterns.emplace_back(*r);
        } else {
          return std::unexpected(ParseError::UNKNOWN_PATTERN);
        }
      }

      return SubscriptionPattern{std::move(*r), std::move(eventPatterns)};
    }

    return std::unexpected(ParseError::UNKNOWN_PATTERN);
  }
};
}  // namespace patterns

struct Gena {};

using namespace dxfcpp;

class Publisher {};

int main() {
  try {
    const auto endpoint = DXEndpoint::newBuilder()->withRole(DXEndpoint::Role::PUBLISHER)->withName("PUB")->build();
    const auto publisher = endpoint->getPublisher();

    const auto sub = publisher->getSubscription(TextMessage::TYPE);

  } catch (const RuntimeException& e) {
    std::cerr << e << std::endl;
    return 1;
  }

  return 0;
}