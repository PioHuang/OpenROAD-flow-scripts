#include <pdn/json.hpp>

#include <cctype>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

namespace pdn {
namespace json {

Value::Value() = default;

Value::Value(bool value) : type_(Type::kBool), bool_value_(value) {}

Value::Value(double value) : type_(Type::kNumber), number_value_(value) {}

Value::Value(std::string value) : type_(Type::kString), string_value_(std::move(value)) {}

Value::Value(Array value) : type_(Type::kArray), array_value_(std::move(value)) {}

Value::Value(Object value) : type_(Type::kObject), object_value_(std::move(value)) {}

bool Value::asBool() const
{
  if (!isBool()) {
    throw std::runtime_error("JSON value is not a bool.");
  }
  return bool_value_;
}

double Value::asNumber() const
{
  if (!isNumber()) {
    throw std::runtime_error("JSON value is not a number.");
  }
  return number_value_;
}

const std::string& Value::asString() const
{
  if (!isString()) {
    throw std::runtime_error("JSON value is not a string.");
  }
  return string_value_;
}

const Value::Array& Value::asArray() const
{
  if (!isArray()) {
    throw std::runtime_error("JSON value is not an array.");
  }
  return array_value_;
}

const Value::Object& Value::asObject() const
{
  if (!isObject()) {
    throw std::runtime_error("JSON value is not an object.");
  }
  return object_value_;
}

const Value& Value::at(std::size_t index) const
{
  return asArray().at(index);
}

const Value& Value::at(const std::string& key) const
{
  const auto& object = asObject();
  auto it = object.find(key);
  if (it == object.end()) {
    throw std::runtime_error("Missing JSON object key: " + key);
  }
  return it->second;
}

bool Value::contains(const std::string& key) const
{
  if (!isObject()) {
    return false;
  }
  return object_value_.find(key) != object_value_.end();
}

namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Value parse()
  {
    skipWhitespace();
    Value value = parseValue();
    skipWhitespace();
    if (!eof()) {
      throw error("Unexpected trailing characters.");
    }
    return value;
  }

 private:
  Value parseValue()
  {
    if (eof()) {
      throw error("Unexpected end of input.");
    }
    const char ch = peek();
    if (ch == '{') {
      return parseObject();
    }
    if (ch == '[') {
      return parseArray();
    }
    if (ch == '"') {
      return Value(parseString());
    }
    if (ch == 't') {
      consumeLiteral("true");
      return Value(true);
    }
    if (ch == 'f') {
      consumeLiteral("false");
      return Value(false);
    }
    if (ch == 'n') {
      consumeLiteral("null");
      return Value();
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
      return Value(parseNumber());
    }
    throw error("Invalid JSON value.");
  }

  Value parseObject()
  {
    expect('{');
    skipWhitespace();
    Value::Object object;
    if (peek() == '}') {
      advance();
      return Value(std::move(object));
    }

    while (true) {
      skipWhitespace();
      if (peek() != '"') {
        throw error("Expected string key.");
      }
      const std::string key = parseString();
      skipWhitespace();
      expect(':');
      skipWhitespace();
      object.emplace(key, parseValue());
      skipWhitespace();
      if (peek() == '}') {
        advance();
        break;
      }
      expect(',');
      skipWhitespace();
    }
    return Value(std::move(object));
  }

  Value parseArray()
  {
    expect('[');
    skipWhitespace();
    Value::Array array;
    if (peek() == ']') {
      advance();
      return Value(std::move(array));
    }

    while (true) {
      skipWhitespace();
      array.push_back(parseValue());
      skipWhitespace();
      if (peek() == ']') {
        advance();
        break;
      }
      expect(',');
      skipWhitespace();
    }
    return Value(std::move(array));
  }

  std::string parseString()
  {
    expect('"');
    std::string out;
    while (!eof()) {
      const char ch = advance();
      if (ch == '"') {
        return out;
      }
      if (ch == '\\') {
        if (eof()) {
          throw error("Invalid string escape.");
        }
        const char esc = advance();
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          default:
            throw error("Unsupported string escape.");
        }
        continue;
      }
      out.push_back(ch);
    }
    throw error("Unterminated string.");
  }

  double parseNumber()
  {
    const std::size_t start = position_;
    if (peek() == '-') {
      advance();
    }
    consumeDigits();
    if (!eof() && peek() == '.') {
      advance();
      consumeDigits();
    }
    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      advance();
      if (!eof() && (peek() == '+' || peek() == '-')) {
        advance();
      }
      consumeDigits();
    }
    const std::string token = text_.substr(start, position_ - start);
    std::stringstream stream(token);
    double value = 0.0;
    stream >> value;
    if (!stream || !stream.eof()) {
      throw error("Invalid number.");
    }
    return value;
  }

  void consumeDigits()
  {
    if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) {
      throw error("Expected digit.");
    }
    while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
      advance();
    }
  }

  void consumeLiteral(const char* literal)
  {
    while (*literal != '\0') {
      if (eof() || advance() != *literal) {
        throw error("Invalid literal.");
      }
      ++literal;
    }
  }

  void skipWhitespace()
  {
    while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) {
      advance();
    }
  }

  void expect(char expected)
  {
    if (eof() || advance() != expected) {
      std::string message = "Expected '";
      message.push_back(expected);
      message.push_back('\'');
      throw error(message);
    }
  }

  char peek() const { return text_.at(position_); }

  char advance() { return text_.at(position_++); }

  bool eof() const { return position_ >= text_.size(); }

  std::runtime_error error(const std::string& message) const
  {
    return std::runtime_error("JSON parse error at byte " + std::to_string(position_) + ": " + message);
  }

  const std::string& text_;
  std::size_t position_ {};
};

}  // namespace

Value parse(const std::string& text)
{
  return Parser(text).parse();
}

}  // namespace json
}  // namespace pdn
