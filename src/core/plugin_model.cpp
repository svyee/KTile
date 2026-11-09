#include "core/plugin_model.h"

#include <cmath>
#include <cstdio>

namespace ktile {
namespace core {

PluginValue PluginValue::makeNull() {
    return PluginValue();
}

PluginValue PluginValue::makeBool(bool value) {
    PluginValue result;
    result.type = Type::Bool;
    result.boolean = value;
    return result;
}

PluginValue PluginValue::makeNumber(double value) {
    PluginValue result;
    result.type = Type::Number;
    result.number = value;
    return result;
}

PluginValue PluginValue::makeString(std::string value) {
    PluginValue result;
    result.type = Type::String;
    result.text = std::move(value);
    return result;
}

PluginValue PluginValue::makeArray() {
    PluginValue result;
    result.type = Type::Array;
    return result;
}

PluginValue PluginValue::makeObject() {
    PluginValue result;
    result.type = Type::Object;
    return result;
}

const PluginValue* PluginValue::find(const std::string& name) const noexcept {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& entry : members) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

const PluginValue* PluginValue::at(std::size_t index) const noexcept {
    if (type != Type::Array || index >= elements.size()) {
        return nullptr;
    }
    return &elements[index];
}

std::string PluginValue::displayString() const {
    switch (type) {
        case Type::Null:
            return std::string();

        case Type::Bool:
            return boolean ? "true" : "false";

        case Type::Number: {
            // 整数不带小数点；非整数保留 1 位小数（天气温度、CPU 占用都够用）。
            // 不用 std::to_string：它会固定输出 6 位小数。
            char buffer[64];
            const double rounded = std::floor(number * 10.0 + 0.5) / 10.0;
            if (std::fabs(rounded - std::floor(rounded)) < 0.0001) {
                std::snprintf(buffer, sizeof(buffer), "%.0f", rounded);
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.1f", rounded);
            }
            return std::string(buffer);
        }

        case Type::String:
            return text;

        case Type::Array: {
            // 数组没有天然的可读形式，渲染层不应该直接展示；返回长度提示。
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "[%zu]",
                          static_cast<std::size_t>(elements.size()));
            return std::string(buffer);
        }

        case Type::Object:
            return std::string();
    }
    return std::string();
}

const PluginLayoutSpec* PluginManifest::layoutByKey(const std::string& key) const noexcept {
    for (const auto& entry : layouts) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const PluginLayoutSpec* PluginManifest::layoutForSize(int tileWidth,
                                                      int tileHeight) const noexcept {
    return selectPluginLayout(*this, tileWidth, tileHeight);
}

const PluginLayoutSpec* selectPluginLayout(const PluginManifest& manifest,
                                           int tileWidth, int tileHeight) noexcept {
    const char* key = "default";
    if (tileWidth > tileHeight) {
        key = "wide";
    } else if (tileHeight > tileWidth) {
        key = "tall";
    } else if (tileWidth > 64) {
        key = "large";
    } else {
        key = "small";
    }

    // 尺寸专属布局缺失时逐级回退：
    //   具体尺寸 → default → 第一个可用布局
    if (const PluginLayoutSpec* exact = manifest.layoutByKey(key)) {
        return exact;
    }
    if (const PluginLayoutSpec* fallback = manifest.layoutByKey("default")) {
        return fallback;
    }
    if (!manifest.layouts.empty()) {
        return &manifest.layouts.front().second;
    }
    return nullptr;
}

const std::string* PluginManifest::iconForRawValue(const std::string& raw) const noexcept {
    for (const auto& entry : iconMap) {
        if (entry.first == raw) {
            return &entry.second;
        }
    }
    return nullptr;
}

}  // namespace core
}  // namespace ktile
