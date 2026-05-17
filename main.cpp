#include <dxfeed_graal_cpp_api/api.hpp>
#include <expected>
#include <iostream>

namespace patterns {
  enum class ParseError { UNKNOWN_PATTERN };

  struct Text {
    std::string value;


    static std::expected<Text, ParseError> parse(std::string_view ) const;
  };

  // $N{0,999}
  struct NumberPattern {
    int min;
    int max;
  };

  struct SymbolNamePattern {
    using Part = std::variant<Text, NumberPattern>;

    std::vector<Part> parts;
  };
}  // namespace patterns

struct Gena {
  
};

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