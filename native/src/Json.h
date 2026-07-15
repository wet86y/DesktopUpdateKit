#pragma once
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace desktop_update_kit::json {
struct Value { using object = std::map<std::string, Value, std::less<>>; using array = std::vector<Value>; std::variant<std::nullptr_t, bool, double, std::string, object, array> data; };
bool parse(const std::string& text, Value& value, std::string& error);
const Value* member(const Value::object& object, const char* name); const std::string* string(const Value* value); std::optional<double> number(const Value* value); std::optional<bool> boolean(const Value* value); const Value::array* array(const Value* value); const Value::object* object(const Value* value);
}
