#include <dxfeed_graal_cpp_api/api.hpp>
#include <expected>
#include <format>
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
enum class ParseErrorEnum {
  UNKNOWN_PATTERN,
  POSITION_OUT_OF_RANGE,
  QUANTITY_MUST_BE_GREATER_THAN_ZERO,
  DUPLICATE_PATTERN
};

template <typename Pattern>
struct ParseResult {
  Pattern pattern{};
  u64 nextPosition{};

  [[nodiscard]] std::string dump() const {
    return pattern.dump();
  }
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
    if (currentPosition >= text.size()) {
      return std::unexpected(ParseError{ParseErrorEnum::POSITION_OUT_OF_RANGE, currentPosition});
    }

    u64 value{};
    const auto [ptr, ec] = std::from_chars(text.data() + currentPosition, text.data() + text.size(), value);

    if (ec != std::errc()) {
      return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
    }

    auto nextPosition = static_cast<u64>(ptr - text.data());

    return ParseResult<UnsignedNumberPattern>{.pattern = {value}, .nextPosition = nextPosition};
  }
};

struct EventPattern {
  static ParseExpected<EmptyPattern> parsePrefixChar(std::string_view text, u64 currentPosition, char prefixChar) {
    if (currentPosition >= text.size()) {
      return std::unexpected(ParseError{ParseErrorEnum::POSITION_OUT_OF_RANGE, currentPosition});
    }

    if (text[currentPosition] != prefixChar) {
      return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
    }

    return ParseResult<EmptyPattern>{{}, currentPosition + 1};
  }

  static ParseExpected<UnsignedNumberPattern> parseMaxQuantity(std::string_view text, u64 currentPosition) {
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

  [[nodiscard]] std::string dump() const {
    return std::format("{}{}", Derived::getPrefix(), maxQuantity.value);
  }

  GenericEventPattern() = default;
  explicit GenericEventPattern(const UnsignedNumberPattern pattern) : maxQuantity(pattern) {
  }
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
  u64 nextPosition{};

  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    (... || [&] -> bool {
      using T = std::variant_alternative_t<Is, Variant>;

      if (auto r = T::parse(text, currentPosition)) {
        result.emplace(r->pattern);
        nextPosition = r->nextPosition;

        return true;
      }

      return false;
    }());
  }(std::make_index_sequence<std::variant_size_v<Variant>>{});

  if (result) {
    return ParseResult<Variant>{*result, nextPosition};
  }

  return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
}

struct SubscriptionPattern {
  std::vector<EventPatternVariant> eventPatterns;

  static std::expected<ParseResult<SubscriptionPattern>, ParseError> parse(std::string_view text, u64 currentPosition) {
    if (currentPosition >= text.size()) {
      return std::unexpected(ParseError{ParseErrorEnum::POSITION_OUT_OF_RANGE, currentPosition});
    }

    static constexpr std::string_view PREFIX = "SUB:";

    if (text.substr(currentPosition, PREFIX.size()) != PREFIX) {
      return std::unexpected(ParseError{ParseErrorEnum::UNKNOWN_PATTERN, currentPosition});
    }

    u64 pos = currentPosition + PREFIX.size();
    std::vector<EventPatternVariant> eventPatterns{};
    std::array<bool, std::variant_size_v<EventPatternVariant>> seen{};

    while (true) {
      const auto& itemResult = parseVariant<EventPatternVariant>(text, pos);

      if (!itemResult) {
        return std::unexpected(itemResult.error());
      }

      const std::size_t index = itemResult->pattern.index();

      if (seen[index]) {
        return std::unexpected(ParseError{ParseErrorEnum::DUPLICATE_PATTERN, pos});
      }
      seen[index] = true;

      eventPatterns.emplace_back(itemResult->pattern);
      pos = itemResult->nextPosition;

      if (pos < text.size() && text[pos] == ';') {
        pos += 1;
        continue;
      }

      break;
    }

    return ParseResult{SubscriptionPattern{std::move(eventPatterns)}, pos};
  }

  [[nodiscard]] std::string dump() const {
    std::string result = "SUB:";

    for (std::size_t i = 0; i < eventPatterns.size(); ++i) {
      if (i > 0) {
        result += ';';
      }

      result += std::visit(
        [](const auto& pattern) {
          return pattern.dump();
        },
        eventPatterns[i]);
    }

    return result;
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

  std::cout << std::format("{}\n", patterns::SubscriptionPattern::parse("SUB:Q100", 0).value().dump());
  std::cout << std::format("{}\n", patterns::SubscriptionPattern::parse("SUB:Q100;S1;T5", 0).value().dump());
  std::cout << std::format("{}\n", patterns::SubscriptionPattern::parse("SUB:S1000;Q100", 0).value().dump());
  std::cout << std::format("{}\n", patterns::SubscriptionPattern::parse("SUB:T10;Q100", 0).value().dump());

  return 0;
}