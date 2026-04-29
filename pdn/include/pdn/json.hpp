#ifndef PDN_JSON_HPP
#define PDN_JSON_HPP

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdn {
namespace json {

class Value {
 public:
  enum class Type {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
  };

  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  Value();
  explicit Value(bool value);
  explicit Value(double value);
  explicit Value(std::string value);
  explicit Value(Array value);
  explicit Value(Object value);

  Type type() const { return type_; }

  bool isNull() const { return type_ == Type::kNull; }
  bool isBool() const { return type_ == Type::kBool; }
  bool isNumber() const { return type_ == Type::kNumber; }
  bool isString() const { return type_ == Type::kString; }
  bool isArray() const { return type_ == Type::kArray; }
  bool isObject() const { return type_ == Type::kObject; }

  bool asBool() const;
  double asNumber() const;
  const std::string& asString() const;
  const Array& asArray() const;
  const Object& asObject() const;

  const Value& at(std::size_t index) const;
  const Value& at(const std::string& key) const;
  bool contains(const std::string& key) const;

 private:
  Type type_ {Type::kNull};
  bool bool_value_ {};
  double number_value_ {};
  std::string string_value_;
  Array array_value_;
  Object object_value_;
};

Value parse(const std::string& text);

}  // namespace json
}  // namespace pdn

#endif  // PDN_JSON_HPP
