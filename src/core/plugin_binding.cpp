#include "core/plugin_binding.h"

#include <cstdlib>
#include <cstring>

namespace ktile {
namespace core {
namespace {

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : path) {
        if (ch == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

bool isNumericIndex(const std::string& part, std::size_t* index) {
    if (part.empty()) {
        return false;
    }
    for (char ch : part) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    *index = static_cast<std::size_t>(std::strtoul(part.c_str(), nullptr, 10));
    return true;
}

const PluginValue* lookupLocal(
    const std::vector<std::pair<std::string, const PluginValue*>>* locals,
    const std::string& name) {
    if (locals == nullptr) {
        return nullptr;
    }
    for (const auto& entry : *locals) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return nullptr;
}

const PluginValue* resolveSelector(
    const PluginBindingContext& context, const std::string& name) {
    if (context.selectors == nullptr) {
        return nullptr;
    }
    for (const auto& entry : *context.selectors) {
        if (entry.first != name) {
            continue;
        }
        if (entry.second.empty()) {
            return context.root;
        }
        // selector 本身也是点号路径，从 root 解析（不递归 select）。
        PluginBindingContext base;
        base.root = context.root;
        return pluginResolvePath(base, entry.second);
    }
    return nullptr;
}

}  // namespace

const PluginValue* pluginResolvePath(const PluginBindingContext& context,
                                     const std::string& path) {
    if (context.root == nullptr || path.empty()) {
        return nullptr;
    }

    std::vector<std::string> parts = splitPath(path);
    std::size_t cursor = 0;
    const PluginValue* node = nullptr;

    const PluginValue* local = lookupLocal(context.locals, parts[0]);
    if (local != nullptr) {
        node = local;
        cursor = 1;
    } else {
        const PluginValue* selected = resolveSelector(context, parts[0]);
        if (selected != nullptr) {
            node = selected;
            cursor = 1;
        } else {
            node = context.root;
        }
    }

    for (; cursor < parts.size(); ++cursor) {
        if (node == nullptr) {
            return nullptr;
        }
        std::size_t index = 0;
        if (isNumericIndex(parts[cursor], &index)) {
            node = node->at(index);
        } else {
            node = node->find(parts[cursor]);
        }
    }
    return node;
}

std::string pluginResolveTemplate(const std::string& tmpl,
                                  const PluginBindingContext& context) {
    std::string output;
    output.reserve(tmpl.size());

    std::size_t cursor = 0;
    while (cursor < tmpl.size()) {
        const std::size_t open = tmpl.find("${", cursor);
        if (open == std::string::npos) {
            output.append(tmpl, cursor, std::string::npos);
            break;
        }
        output.append(tmpl, cursor, open - cursor);

        const std::size_t close = tmpl.find('}', open + 2);
        if (close == std::string::npos) {
            // 未闭合的占位符原样保留，方便作者发现拼写错误。
            output.append(tmpl, open, std::string::npos);
            break;
        }

        const std::string path = tmpl.substr(open + 2, close - open - 2);
        const PluginValue* value = pluginResolvePath(context, path);
        if (value != nullptr) {
            output.append(value->displayString());
        }
        cursor = close + 1;
    }
    return output;
}

double pluginResolveNumber(const std::string& tmpl,
                           const PluginBindingContext& context,
                           double fallback) {
    const PluginValue* direct = nullptr;
    // 整串就是一个占位符时直接取数值，避免走字符串再解析。
    if (tmpl.size() > 3 && tmpl.compare(0, 2, "${") == 0 &&
        tmpl.back() == '}') {
        const std::string path = tmpl.substr(2, tmpl.size() - 3);
        direct = pluginResolvePath(context, path);
        if (direct != nullptr && direct->isNumber()) {
            return direct->number;
        }
    }

    const std::string text = pluginResolveTemplate(tmpl, context);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return parsed;
}

std::string pluginResolveParamTemplate(
    const std::string& tmpl,
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::string output;
    output.reserve(tmpl.size());

    std::size_t cursor = 0;
    while (cursor < tmpl.size()) {
        const std::size_t open = tmpl.find("${", cursor);
        if (open == std::string::npos) {
            output.append(tmpl, cursor, std::string::npos);
            break;
        }
        output.append(tmpl, cursor, open - cursor);

        const std::size_t close = tmpl.find('}', open + 2);
        if (close == std::string::npos) {
            output.append(tmpl, open, std::string::npos);
            break;
        }

        const std::string key = tmpl.substr(open + 2, close - open - 2);
        for (const auto& entry : params) {
            if (entry.first == key) {
                output.append(entry.second);
                break;
            }
        }
        cursor = close + 1;
    }
    return output;
}

std::uint32_t pluginParseColor(const std::string& text, std::uint32_t fallback) noexcept {
    if (text.empty() || text[0] != '#') {
        return fallback;
    }
    const std::string hex = text.substr(1);
    if (hex.size() != 6 && hex.size() != 8) {
        return fallback;
    }
    std::uint32_t value = 0;
    for (char ch : hex) {
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<std::uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<std::uint32_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<std::uint32_t>(ch - 'A' + 10);
        } else {
            return fallback;
        }
        value = (value << 4) | digit;
    }
    if (hex.size() == 6) {
        value |= 0xFF000000U;
    }
    return value;
}

}  // namespace core
}  // namespace ktile
