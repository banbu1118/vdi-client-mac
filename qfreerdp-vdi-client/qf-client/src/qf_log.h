#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>

namespace qf::log {

inline constexpr int kFormatWidth = 20;

inline std::once_flag& init_once_flag()
{
    static std::once_flag flag;
    return flag;
}

inline void init()
{
    std::call_once(init_once_flag(), [] {
        // Default level: info (shows connection, channel events, etc.)
        // Override with SPDLOG_LEVEL env var for runtime control, e.g.:
        //   set SPDLOG_LEVEL=debug
        //
        // Logs go to stderr only (no persistent log file). When launched
        // from VDIClient, QProcess captures the console output.
        try
        {
            auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            auto logger =
                std::make_shared<spdlog::logger>("qf", spdlog::sinks_init_list{console_sink});
            spdlog::set_default_logger(logger);
        }
        catch (const std::exception&)
        {
            // Keep the default stderr logger if setup fails
        }
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    });
}

inline spdlog::level::level_enum to_level(spdlog::level::level_enum level)
{
    init();
    return level;
}

template <typename... Args>
inline void write(spdlog::level::level_enum level, std::string_view action,
                  fmt::format_string<Args...> format, Args&&... args)
{
    const auto message = fmt::format(format, std::forward<Args>(args)...);
    // 用 fmt 而非 std::format：libc++ 的 std::format 浮点实现要求 macOS 14+，
    // 部署目标 13.0 下编译报 to_chars unavailable
    spdlog::log(level, "{:<22} {}", fmt::format("[{}]", action), message);
}

template <typename... Args>
inline void info(std::string_view action, fmt::format_string<Args...> format, Args&&... args)
{
    write(spdlog::level::info, action, format, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(std::string_view action, fmt::format_string<Args...> format, Args&&... args)
{
    write(spdlog::level::warn, action, format, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(std::string_view action, fmt::format_string<Args...> format, Args&&... args)
{
    write(spdlog::level::err, action, format, std::forward<Args>(args)...);
}

} // namespace qf::log
