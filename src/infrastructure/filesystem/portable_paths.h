// KTile - 基础设施层：便携路径解析
//
// 绿色软件核心约束：数据跟随 exe 存放，不写注册表、不写 AppData。
// 目录约定（exe 同级）：<exe目录>\data\items.json 磁贴数据、
// \data\config.json 应用配置、\data\icons\ 图标缓存。exe 目录不可写
// （如 Program Files 下）时自动降级到 %LOCALAPPDATA%\KTile\。
//
#pragma once

#include <string>

namespace ktile {
namespace infrastructure {

class PortablePaths {
public:
    // 依据当前进程 exe 位置初始化。
    // 返回 false 表示连降级目录都拿不到（极端情况）。
    bool initialize();

    // 是否成功使用 exe 同级目录（绿模式）
    bool isPortable() const noexcept { return portable_; }

    // exe 所在目录（不带结尾反斜杠）
    const std::wstring& executableDirectory() const noexcept {
        return executableDirectory_;
    }

    // 数据根目录（data 子目录）
    const std::wstring& dataDirectory() const noexcept { return dataDirectory_; }

    // 具体文件路径
    std::wstring itemsFilePath() const;
    std::wstring configFilePath() const;
    std::wstring iconsDirectory() const;
    std::wstring logFilePath() const;
    // 语言文件目录（exe 同级 lang/，每份一个 <code>.json）
    std::wstring langDirectory() const;

    // 确保目录存在（递归创建）。成功返回 true。
    bool ensureDirectoryExists(const std::wstring& path) const;

private:
    bool portable_ = false;
    std::wstring executableDirectory_;
    std::wstring dataDirectory_;
};

}  // namespace infrastructure
}  // namespace ktile
