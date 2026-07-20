#include <dxfeed_graal_cpp_api/api.hpp>
#include <expected>
#include <iostream>
#include <string_view>
#include <variant>

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;
using i64 = std::int64_t;
using i32 = std::int32_t;
using i16 = std::int16_t;
using i8 = std::int8_t;

namespace patterns {
enum class ParseErrorEnum { UNKNOWN_PATTERN, POSITION_OUT_OF_RANGE, QUANTITY_MUST_BE_GREATER_THAN_ZERO };

template <typename Pattern>
struct ParseResult {
  Pattern pattern{};
  u64 nextPosition{};
};

struct ParseError {
  ParseErrorEnum error{};
  u64 position{};
};

struct EmptyPattern {};

template <typename Pattern>
using ParseExpected = std::expected<ParseResult<Pattern>, ParseError>;

struct UnsignedNumberPattern {
  u64 value{};

  static ParseExpected<UnsignedNumberPattern> parse(std::string_view text, u64 currentPosition) {
    if (currentPosition > text.size()) {
      return std::unexpected(ParseError{ParseErrorEnum::POSITION_OUT_OF_RANGE, currentPosition});
    }

    u64 value{};
    const auto [ptr, ec] = std::from_chars(text.data() + currentPosition, text.data() + text.size(), value);

    if (ec != std::errc()) {
      return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
    }

    return ParseResult<UnsignedNumberPattern>{.pattern = {value}, .nextPosition = static_cast<u64>(ptr - text.data())};
  }
};

struct EventPattern {
  static std::expected<ParseResult<EmptyPattern>, ParseError> parsePrefixChar(std::string_view text,
                                                                              u64 currentPosition, char prefixChar) {
    if (currentPosition > text.size()) {
      return std::unexpected(ParseError{ParseErrorEnum::POSITION_OUT_OF_RANGE, currentPosition});
    }

    if (text[currentPosition] != prefixChar) {
      return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
    }

    return ParseResult<EmptyPattern>{{}, currentPosition + 1};
  }

  static std::expected<ParseResult<UnsignedNumberPattern>, ParseError> parseMaxQuantity(std::string_view text,
                                                                                        u64 currentPosition) {
    return UnsignedNumberPattern::parse(text, currentPosition)
      .and_then([](const ParseResult<UnsignedNumberPattern>& r) -> ParseExpected<UnsignedNumberPattern> {
        if (r.pattern.value == 0) {
          return std::unexpected(ParseError{ParseErrorEnum::QUANTITY_MUST_BE_GREATER_THAN_ZERO, r.nextPosition});
        }
        return r;
      });
  }
};

template <typename Derived>
struct GenericEventPattern : EventPattern {
  UnsignedNumberPattern maxQuantity{};

  static ParseExpected<Derived> parse(std::string_view text, u64 currentPosition) {
    return parsePrefixChar(text, currentPosition, Derived::getPrefix())
      .and_then([&](const ParseResult<EmptyPattern>& prefixResult) {
        return parseMaxQuantity(text, prefixResult.nextPosition);
      })
      .transform([](const ParseResult<UnsignedNumberPattern>& qty) {
        return ParseResult<Derived>{Derived{qty.pattern}, qty.nextPosition};
      });
  }

  GenericEventPattern() = default;
  explicit GenericEventPattern(const UnsignedNumberPattern pattern) : maxQuantity(pattern) {}
};

struct QuotePattern : GenericEventPattern<QuotePattern> {
  using GenericEventPattern::GenericEventPattern;

  static char getPrefix() {
    return 'Q';
  }
};

struct TradePattern : GenericEventPattern<TradePattern> {
  using GenericEventPattern::GenericEventPattern;

  static char getPrefix() {
    return 'T';
  }
};

struct SummaryPattern : GenericEventPattern<SummaryPattern> {
  using GenericEventPattern::GenericEventPattern;

  static char getPrefix() {
    return 'S';
  }
};

using EventPatternVariant = std::variant<QuotePattern, TradePattern, SummaryPattern>;

template <typename Variant>
static std::expected<ParseResult<Variant>, ParseError> parseVariant(std::string_view text, u64 currentPosition) {
  std::optional<Variant> result;

  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    (... || [&] -> bool {
      using T = std::variant_alternative_t<Is, Variant>;

      if (auto r = T::parse(text, currentPosition)) {
        result.emplace(*r);

        return true;
      }

      return false;
    }());
  }(std::make_index_sequence<std::variant_size_v<Variant>>{});

  if (result) {
    return *result;
  }

  return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
}

struct SubscriptionPattern {
  std::vector<EventPattern> eventPatterns;

  static std::expected<SubscriptionPattern, ParseError> parse(std::string_view text, u64 currentPosition) {
    const auto& parts = dxfcpp::splitStr(text, ':');

    if (parts.size() == 2 && parts[0] == "SUB") {
      const auto eventPatternParts = dxfcpp::splitStr(parts[1], ';');

      if (eventPatternParts.empty()) {
        return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
      }

      std::vector<EventPattern> eventPatterns{};

      for (auto& part : eventPatternParts) {
        auto result = parseVariant<EventPatternVariant>(part);

        if (!result) {
          return std::unexpected(result.error());
        }

        eventPatterns.emplace_back(*result);
      }

      return SubscriptionPattern{std::move(*r), std::move(eventPatterns)};
    }

    return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
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

    const auto sub = publisher->getSubscription(Quote::TYPE);

  } catch (const RuntimeException& e) {
    std::cerr << e << std::endl;
    return 1;
  }

  return 0;
}