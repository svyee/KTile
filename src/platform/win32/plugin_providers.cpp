#include "platform/win32/plugin_providers.h"

#include "infrastructure/i18n.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 数字写进 JSON：整数不带小数，非整数保留 1 位。
std::string jsonNumber(double value) {
    char buffer[64];
    const double rounded = (value * 10.0 + (value >= 0 ? 0.5 : -0.5)) / 10.0;
    if (rounded == static_cast<double>(static_cast<long long>(rounded))) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", rounded);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f", rounded);
    }
    return std::string(buffer);
}

bool queryCpuPercent(double* percent) {
    static ULONGLONG previousIdle = 0;
    static ULONGLONG previousKernel = 0;
    static ULONGLONG previousUser = 0;
    static bool hasPrevious = false;

    FILETIME idleTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return false;
    }

    const auto toUlongLong = [](const FILETIME& value) {
        ULARGE_INTEGER converted{};
        converted.LowPart = value.dwLowDateTime;
        converted.HighPart = value.dwHighDateTime;
        return converted.QuadPart;
    };

    const ULONGLONG idle = toUlongLong(idleTime);
    const ULONGLONG kernel = toUlongLong(kernelTime);
    const ULONGLONG user = toUlongLong(userTime);

    if (!hasPrevious) {
        previousIdle = idle;
        previousKernel = kernel;
        previousUser = user;
        hasPrevious = true;
        *percent = 0.0;
        return true;
    }

    const ULONGLONG idleDelta = idle - previousIdle;
    const ULONGLONG totalDelta = (kernel - previousKernel) + (user - previousUser);
    previousIdle = idle;
    previousKernel = kernel;
    previousUser = user;

    if (totalDelta == 0) {
        *percent = 0.0;
        return true;
    }
    // kernel 时间包含 idle，所以忙碌 = total - idle。
    *percent = 100.0 * static_cast<double>(totalDelta - idleDelta) /
               static_cast<double>(totalDelta);
    if (*percent < 0.0) {
        *percent = 0.0;
    }
    if (*percent > 100.0) {
        *percent = 100.0;
    }
    return true;
}

bool queryMemory(double* percent, ULONGLONG* usedMb, ULONGLONG* totalMb) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return false;
    }
    *percent = static_cast<double>(status.dwMemoryLoad);
    *totalMb = status.ullTotalPhys / (1024ULL * 1024ULL);
    *usedMb = (status.ullTotalPhys - status.ullAvailPhys) / (1024ULL * 1024ULL);
    return true;
}

bool queryDisk(double* percent, double* freeGb, double* totalGb) {
    std::wstring systemDrive = L"C:\\";
    wchar_t environment[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"SystemDrive", environment, MAX_PATH) > 0 &&
        environment[0] != L'\0') {
        systemDrive = std::wstring(environment) + L"\\";
    }

    ULARGE_INTEGER freeForCaller{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER totalFree{};
    if (!GetDiskFreeSpaceExW(systemDrive.c_str(), &freeForCaller, &total,
                             &totalFree)) {
        return false;
    }
    if (total.QuadPart == 0) {
        return false;
    }
    *percent = 100.0 * static_cast<double>(total.QuadPart - totalFree.QuadPart) /
               static_cast<double>(total.QuadPart);
    *freeGb = static_cast<double>(totalFree.QuadPart) /
              (1024.0 * 1024.0 * 1024.0);
    *totalGb = static_cast<double>(total.QuadPart) /
               (1024.0 * 1024.0 * 1024.0);
    return true;
}

bool queryBattery(double* percent, bool* charging, bool* present) {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        return false;
    }
    // bit 7 (128) = 没有系统电池；255 = 电量未知。
    if ((status.BatteryFlag & 128) != 0 || status.BatteryLifePercent == 255) {
        *present = false;
        return true;
    }
    *present = true;
    *percent = static_cast<double>(status.BatteryLifePercent);
    *charging = (status.BatteryFlag & 8) != 0;
    return true;
}

std::string buildSystemJson() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    // 星期名来自语言文件（provider.system.weekday.N）—— 不硬编中文。
    char weekdayKey[40];
    std::snprintf(weekdayKey, sizeof(weekdayKey), "provider.system.weekday.%u",
                  static_cast<unsigned>(now.wDayOfWeek));
    const std::string weekday = infrastructure::i18n::tr(weekdayKey);

    char timeText[16];
    std::snprintf(timeText, sizeof(timeText), "%02u:%02u",
                  static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute));
    char dateText[16];
    std::snprintf(dateText, sizeof(dateText), "%04u-%02u-%02u",
                  static_cast<unsigned>(now.wYear),
                  static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay));

    double cpuPercent = 0.0;
    queryCpuPercent(&cpuPercent);

    double memoryPercent = 0.0;
    ULONGLONG usedMb = 0;
    ULONGLONG totalMb = 0;
    const bool memoryOk = queryMemory(&memoryPercent, &usedMb, &totalMb);

    double diskPercent = 0.0;
    double diskFreeGb = 0.0;
    double diskTotalGb = 0.0;
    const bool diskOk = queryDisk(&diskPercent, &diskFreeGb, &diskTotalGb);

    double batteryPercent = 0.0;
    bool batteryCharging = false;
    bool batteryPresent = false;
    queryBattery(&batteryPercent, &batteryCharging, &batteryPresent);

    std::string json;
    json.reserve(512);
    json += "{\"cpu\":{\"percent\":" + jsonNumber(cpuPercent) + "},";

    json += "\"memory\":{\"percent\":" + jsonNumber(memoryPercent);
    if (memoryOk) {
        json += ",\"usedMB\":" + std::to_string(static_cast<unsigned long long>(usedMb));
        json += ",\"totalMB\":" + std::to_string(static_cast<unsigned long long>(totalMb));
    }
    json += "},";

    json += "\"disk\":{\"percent\":" + jsonNumber(diskPercent);
    if (diskOk) {
        json += ",\"freeGB\":" + jsonNumber(diskFreeGb);
        json += ",\"totalGB\":" + jsonNumber(diskTotalGb);
    }
    json += "},";

    json += "\"battery\":{\"present\":";
    json += batteryPresent ? "true" : "false";
    json += ",\"percent\":" + jsonNumber(batteryPercent);
    json += ",\"charging\":";
    json += batteryCharging ? "true" : "false";
    json += "},";

    json += "\"time\":{\"hm\":\"" + std::string(timeText) + "\",";
    json += "\"date\":\"" + std::string(dateText) + "\",";
    json += "\"weekday\":\"" + weekday + "\"},";

    // metrics 数组：供 large 布局用 repeat 动态展开，顺序固定。
    json += "\"metrics\":[";
    // 图标值是「相对插件目录」的路径：svg 文件随 system-monitor 插件一起
    // 安装（plugin/system-monitor/icons/），主程序不内嵌这些图形。
    json += "{\"name\":\"CPU\",\"value\":\"" + jsonNumber(cpuPercent) +
            "%\",\"percent\":" + jsonNumber(cpuPercent) +
            ",\"icon\":\"icons/cpu\"}";
    json += ",{\"name\":\"" + infrastructure::i18n::tr("provider.system.memory") +
            "\",\"value\":\"" + jsonNumber(memoryPercent) +
            "%\",\"percent\":" + jsonNumber(memoryPercent) +
            ",\"icon\":\"icons/memory-stick\"}";
    json += ",{\"name\":\"" + infrastructure::i18n::tr("provider.system.disk") +
            "\",\"value\":\"" + jsonNumber(diskPercent) +
            "%\",\"percent\":" + jsonNumber(diskPercent) +
            ",\"icon\":\"icons/hard-drive\"}";
    if (batteryPresent) {
        json += ",{\"name\":\"" + infrastructure::i18n::tr("provider.system.battery") +
                "\",\"value\":\"" + jsonNumber(batteryPercent) +
                "%\",\"percent\":" + jsonNumber(batteryPercent) +
                ",\"icon\":\"icons/battery\"}";
    }
    json += "]}";

    return json;
}

}  // namespace

PluginRawResult runBuiltinPluginProvider(const std::string& provider) {
    PluginRawResult result;

    // v1 只提供 system；未来可加 "network"/"media" 等。
    if (provider.empty() || provider == "system") {
        result.body = buildSystemJson();
        result.ok = true;
        return result;
    }

    result.error = infrastructure::i18n::formatU("provider.system.unknown", {provider});
    return result;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
