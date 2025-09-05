#ifndef JSON_HPP
#define JSON_HPP

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace nlohmann {

class json {
public:
    enum class value_type {
        null,
        object,
        array,
        string,
        boolean,
        number_integer,
        number_float
    };

private:
    value_type type_ = value_type::null;
    std::map<std::string, json>* object_value = nullptr;
    std::vector<json>* array_value = nullptr;
    std::string* string_value = nullptr;
    bool boolean_value = false;
    long long integer_value = 0;
    double float_value = 0.0;

    void clear() {
        delete object_value;
        delete array_value;
        delete string_value;
        object_value = nullptr;
        array_value = nullptr;
        string_value = nullptr;
        type_ = value_type::null;
    }

public:
    // 构造函数
    json() = default;
    
    json(const std::string& str) : type_(value_type::string) {
        string_value = new std::string(str);
    }
    
    json(const char* str) : type_(value_type::string) {
        string_value = new std::string(str);
    }
    
    json(bool b) : type_(value_type::boolean), boolean_value(b) {}
    
    json(int i) : type_(value_type::number_integer), integer_value(i) {}
    
    json(long long i) : type_(value_type::number_integer), integer_value(i) {}
    
    json(double d) : type_(value_type::number_float), float_value(d) {}

    // 拷贝构造函数
    json(const json& other) {
        *this = other;
    }

    // 赋值运算符
    json& operator=(const json& other) {
        if (this != &other) {
            clear();
            type_ = other.type_;
            
            switch (type_) {
                case value_type::object:
                    if (other.object_value) {
                        object_value = new std::map<std::string, json>(*other.object_value);
                    }
                    break;
                case value_type::array:
                    if (other.array_value) {
                        array_value = new std::vector<json>(*other.array_value);
                    }
                    break;
                case value_type::string:
                    if (other.string_value) {
                        string_value = new std::string(*other.string_value);
                    }
                    break;
                case value_type::boolean:
                    boolean_value = other.boolean_value;
                    break;
                case value_type::number_integer:
                    integer_value = other.integer_value;
                    break;
                case value_type::number_float:
                    float_value = other.float_value;
                    break;
                default:
                    break;
            }
        }
        return *this;
    }

    // 析构函数
    ~json() {
        clear();
    }

    // 类型检查
    bool is_null() const { return type_ == value_type::null; }
    bool is_object() const { return type_ == value_type::object; }
    bool is_array() const { return type_ == value_type::array; }
    bool is_string() const { return type_ == value_type::string; }
    bool is_boolean() const { return type_ == value_type::boolean; }
    bool is_number() const { return type_ == value_type::number_integer || type_ == value_type::number_float; }

    // 对象操作
    json& operator[](const std::string& key) {
        if (type_ == value_type::null) {
            type_ = value_type::object;
            object_value = new std::map<std::string, json>();
        }
        if (type_ != value_type::object) {
            throw std::runtime_error("Cannot use operator[] with non-object type");
        }
        return (*object_value)[key];
    }

    const json& operator[](const std::string& key) const {
        if (type_ != value_type::object || !object_value) {
            throw std::runtime_error("Cannot use operator[] with non-object type");
        }
        auto it = object_value->find(key);
        if (it == object_value->end()) {
            throw std::runtime_error("Key not found");
        }
        return it->second;
    }

    // 迭代器支持
    class iterator {
        std::map<std::string, json>::iterator obj_it;
        bool is_obj;
    public:
        iterator(std::map<std::string, json>::iterator it) : obj_it(it), is_obj(true) {}
        
        std::pair<std::string, json&> operator*() {
            return {obj_it->first, obj_it->second};
        }
        
        iterator& operator++() {
            ++obj_it;
            return *this;
        }
        
        bool operator!=(const iterator& other) const {
            return obj_it != other.obj_it;
        }
    };

    iterator begin() {
        if (type_ != value_type::object || !object_value) {
            throw std::runtime_error("Cannot iterate non-object type");
        }
        return iterator(object_value->begin());
    }

    iterator end() {
        if (type_ != value_type::object || !object_value) {
            throw std::runtime_error("Cannot iterate non-object type");
        }
        return iterator(object_value->end());
    }

    // items() 方法，返回键值对
    std::vector<std::pair<std::string, json&>> items() {
        if (type_ != value_type::object || !object_value) {
            throw std::runtime_error("Cannot get items from non-object type");
        }
        std::vector<std::pair<std::string, json&>> result;
        for (auto& pair : *object_value) {
            result.emplace_back(pair.first, pair.second);
        }
        return result;
    }

    // 序列化为字符串
    std::string dump() const {
        switch (type_) {
            case value_type::null:
                return "null";
            case value_type::boolean:
                return boolean_value ? "true" : "false";
            case value_type::number_integer:
                return std::to_string(integer_value);
            case value_type::number_float:
                return std::to_string(float_value);
            case value_type::string:
                return "\"" + escape_string(*string_value) + "\"";
            case value_type::array:
                if (!array_value) return "[]";
                {
                    std::string result = "[";
                    for (size_t i = 0; i < array_value->size(); ++i) {
                        if (i > 0) result += ",";
                        result += (*array_value)[i].dump();
                    }
                    result += "]";
                    return result;
                }
            case value_type::object:
                if (!object_value) return "{}";
                {
                    std::string result = "{";
                    bool first = true;
                    for (const auto& pair : *object_value) {
                        if (!first) result += ",";
                        result += "\"" + escape_string(pair.first) + "\":" + pair.second.dump();
                        first = false;
                    }
                    result += "}";
                    return result;
                }
        }
        return "null";
    }

    // 从字符串解析
    static json parse(const std::string& str) {
        size_t pos = 0;
        return parse_value(str, pos);
    }

private:
    std::string escape_string(const std::string& str) const {
        std::string result;
        for (char c : str) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

    static void skip_whitespace(const std::string& str, size_t& pos) {
        while (pos < str.length() && std::isspace(str[pos])) {
            ++pos;
        }
    }

    static json parse_value(const std::string& str, size_t& pos) {
        skip_whitespace(str, pos);
        
        if (pos >= str.length()) {
            throw std::runtime_error("Unexpected end of input");
        }

        char c = str[pos];
        
        if (c == '{') {
            return parse_object(str, pos);
        } else if (c == '[') {
            return parse_array(str, pos);
        } else if (c == '"') {
            return parse_string(str, pos);
        } else if (c == 't' || c == 'f') {
            return parse_boolean(str, pos);
        } else if (c == 'n') {
            return parse_null(str, pos);
        } else if (c == '-' || std::isdigit(c)) {
            return parse_number(str, pos);
        } else {
            throw std::runtime_error("Unexpected character");
        }
    }

    static json parse_object(const std::string& str, size_t& pos) {
        json obj;
        obj.type_ = value_type::object;
        obj.object_value = new std::map<std::string, json>();
        
        ++pos; // skip '{'
        skip_whitespace(str, pos);
        
        if (pos < str.length() && str[pos] == '}') {
            ++pos;
            return obj;
        }
        
        while (pos < str.length()) {
            skip_whitespace(str, pos);
            
            // 解析key
            if (str[pos] != '"') {
                throw std::runtime_error("Expected string key");
            }
            json key = parse_string(str, pos);
            std::string key_str = *key.string_value;
            
            skip_whitespace(str, pos);
            if (pos >= str.length() || str[pos] != ':') {
                throw std::runtime_error("Expected ':'");
            }
            ++pos; // skip ':'
            
            // 解析value
            json value = parse_value(str, pos);
            (*obj.object_value)[key_str] = value;
            
            skip_whitespace(str, pos);
            if (pos >= str.length()) break;
            
            if (str[pos] == '}') {
                ++pos;
                break;
            } else if (str[pos] == ',') {
                ++pos;
            } else {
                throw std::runtime_error("Expected ',' or '}'");
            }
        }
        
        return obj;
    }

    static json parse_array(const std::string& str, size_t& pos) {
        json arr;
        arr.type_ = value_type::array;
        arr.array_value = new std::vector<json>();
        
        ++pos; // skip '['
        skip_whitespace(str, pos);
        
        if (pos < str.length() && str[pos] == ']') {
            ++pos;
            return arr;
        }
        
        while (pos < str.length()) {
            json value = parse_value(str, pos);
            arr.array_value->push_back(value);
            
            skip_whitespace(str, pos);
            if (pos >= str.length()) break;
            
            if (str[pos] == ']') {
                ++pos;
                break;
            } else if (str[pos] == ',') {
                ++pos;
            } else {
                throw std::runtime_error("Expected ',' or ']'");
            }
        }
        
        return arr;
    }

    static json parse_string(const std::string& str, size_t& pos) {
        ++pos; // skip '"'
        std::string result;
        
        while (pos < str.length() && str[pos] != '"') {
            if (str[pos] == '\\' && pos + 1 < str.length()) {
                ++pos;
                switch (str[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += str[pos]; break;
                }
            } else {
                result += str[pos];
            }
            ++pos;
        }
        
        if (pos >= str.length()) {
            throw std::runtime_error("Unterminated string");
        }
        
        ++pos; // skip '"'
        return json(result);
    }

    static json parse_boolean(const std::string& str, size_t& pos) {
        if (str.substr(pos, 4) == "true") {
            pos += 4;
            return json(true);
        } else if (str.substr(pos, 5) == "false") {
            pos += 5;
            return json(false);
        } else {
            throw std::runtime_error("Invalid boolean value");
        }
    }

    static json parse_null(const std::string& str, size_t& pos) {
        if (str.substr(pos, 4) == "null") {
            pos += 4;
            return json();
        } else {
            throw std::runtime_error("Invalid null value");
        }
    }

    static json parse_number(const std::string& str, size_t& pos) {
        size_t start = pos;
        bool is_float = false;
        
        if (str[pos] == '-') ++pos;
        
        while (pos < str.length() && std::isdigit(str[pos])) {
            ++pos;
        }
        
        if (pos < str.length() && str[pos] == '.') {
            is_float = true;
            ++pos;
            while (pos < str.length() && std::isdigit(str[pos])) {
                ++pos;
            }
        }
        
        std::string num_str = str.substr(start, pos - start);
        
        if (is_float) {
            return json(std::stod(num_str));
        } else {
            return json(std::stoll(num_str));
        }
    }
};

} // namespace nlohmann

#endif // JSON_HPP
