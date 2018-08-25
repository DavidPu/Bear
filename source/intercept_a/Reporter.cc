/*  Copyright (C) 2012-2017 by László Nagy
    This file is part of Bear.

    Bear is a tool to generate compilation database for clang tooling.

    Bear is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Bear is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "intercept_a/Reporter.h"
#include "intercept_a/Result.h"
#include "intercept_a/SystemCalls.h"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace {

    pear::Result<std::wstring> to_wstring(const char *value) {
        std::mbstate_t state = std::mbstate_t();
        const std::size_t wsrc_length = std::mbsrtowcs(nullptr, &value, 0, &state);

        wchar_t wsrc[wsrc_length + 1];
        return (std::mbsrtowcs((wchar_t *)&wsrc, &value, wsrc_length + 1, &state) != wsrc_length)
                ? pear::Err<std::wstring>("mbstrtowcs")
                : pear::Ok(std::wstring(wsrc));
    }

    std::string to_json_string(const std::wstring &value) {
        std::string result;

        wchar_t const *wsrc_it = value.c_str();
        wchar_t const *const wsrc_end = wsrc_it + value.length();

        for (; wsrc_it != wsrc_end; ++wsrc_it) {
            // Insert an escape character before control characters.
            switch (*wsrc_it) {
                case L'\b':
                    result += "\\b";
                    break;
                case L'\f':
                    result += "\\f";
                    break;
                case L'\n':
                    result += "\\n";
                    break;
                case L'\r':
                    result += "\\r";
                    break;
                case L'\t':
                    result += "\\t";
                    break;
                case L'"':
                    result += "\\\"";
                    break;
                case L'\\':
                    result += "\\\\";
                    break;
                default:
                    if ((*wsrc_it < L' ') || (*wsrc_it > 127)) {
                        char buffer[7];
                        snprintf(buffer, 7, "\\u%04x", (unsigned int)*wsrc_it);
                        result += buffer;
                    } else {
                        result += char(*wsrc_it);
                    }
                    break;
            }
        }
        return result;
    }

    void json_string(std::ostream &os, const char *value) {
        to_wstring(value)
                .map<std::string>(to_json_string)
                .map<std::string>([&os](auto string) {
                    os << '"' << string << '"';
                    return string;
                })
                .handle_with([](auto error) {
                    std::cerr << error.what() << std::endl;
                });
    }

    void json_attribute(std::ostream &os, const char *key, const char *value) {
        os << '"' << key << '"' << ':';
        json_string(os, value);
    }

    void json_attribute(std::ostream &os, const char *key, const char **value) {
        os << '"' << key << '"' << ':';
        os << '[';
        for (const char **it = value; *it != nullptr; ++it) {
            if (it != value)
                os << ',';
            json_string(os, *it);
        }
        os << ']';
    }

    void json_attribute(std::ostream &os, const char *key, const int value) {
        os << '"' << key << '"' << ':'<< value;
    }


    class TimedEvent : public pear::Event {
    private:
        std::chrono::system_clock::time_point const when_;

    public:
        TimedEvent() noexcept
                : when_(std::chrono::system_clock::now())
        { }

        std::chrono::system_clock::time_point const &when() const noexcept {
            return when_;
        }
    };

    struct ProcessStartEvent : public TimedEvent {
        pid_t child_;
        pid_t supervisor_;
        pid_t parent_;
        std::string cwd_;
        const char **cmd_;

        ProcessStartEvent(pid_t child,
                          pid_t supervisor,
                          pid_t parent,
                          std::string cwd,
                          const char **cmd) noexcept
                : TimedEvent()
                , child_(child)
                , supervisor_(supervisor)
                , parent_(parent)
                , cwd_(std::move(cwd))
                , cmd_(cmd)
        { }

        const char *name() const override {
            return "process_start";
        }

        std::ostream &to_json(std::ostream &os) const override {
            os << '{';
            json_attribute(os, "pid", child_);
            os << ',';
            json_attribute(os, "ppid", supervisor_);
            os << ',';
            json_attribute(os, "pppid", parent_);
            os << ',';
            json_attribute(os, "cwd", cwd_.c_str());
            os << ',';
            json_attribute(os, "cmd", cmd_);
            os << '}';

            return os;
        }
    };

    struct ProcessStopEvent : public TimedEvent {
        pid_t child_;
        pid_t supervisor_;
        int exit_;

        ProcessStopEvent(pid_t child,
                         pid_t supervisor,
                         int exit) noexcept
                : TimedEvent()
                , child_(child)
                , supervisor_(supervisor)
                , exit_(exit)
        { }

        const char *name() const override {
            return "process_stop";
        }

        std::ostream &to_json(std::ostream &os) const override {
            os << '{';
            json_attribute(os, "pid", child_);
            os << ',';
            json_attribute(os, "ppid", supervisor_);
            os << ',';
            json_attribute(os, "exit", exit_);
            os << '}';

            return os;
        }
    };


    class ReporterImpl : public pear::Reporter {
    public:
        explicit ReporterImpl(const char *target) noexcept;

        pear::Result<int> send(const pear::EventPtr &event) noexcept override;

    private:
        pear::Result<std::shared_ptr<std::ostream>> create_stream(const std::string &) const;

        std::filesystem::path const target_;
    };

    ReporterImpl::ReporterImpl(const char *target) noexcept
            : pear::Reporter()
            , target_(target)
    { }

    pear::Result<int> ReporterImpl::send(const pear::EventPtr &event) noexcept {
        return create_stream(event->name())
                .map<int>([&event](auto stream) {
                    event->to_json(*stream);
                    return 0;
                });
    }

    pear::Result<std::shared_ptr<std::ostream>> ReporterImpl::create_stream(const std::string &prefix) const {
        return pear::temp_file(target_.c_str(), ("." + prefix + ".json").c_str());
    }
}


namespace pear {

    Result<EventPtr> Event::start(pid_t pid, const char **cmd) noexcept {
        const Result<pid_t> current_pid = get_pid();
        const Result<pid_t> parent_pid = get_ppid();
        const Result<std::string> working_dir = get_cwd();
        return merge(current_pid, parent_pid, working_dir)
                .map<EventPtr>([&pid, &cmd](auto tuple) {
                    const auto& [ current, parent, cwd ] = tuple;
                    return EventPtr(new ProcessStartEvent(pid, current, parent, cwd, cmd));
                });
    };

    Result<EventPtr> Event::stop(pid_t pid, int exit) noexcept {
        return get_pid()
                .map<EventPtr>([&pid, &exit](auto current) {
                    return EventPtr(new ProcessStopEvent(pid, current, exit));
                });
    }

    Result<ReporterPtr> Reporter::tempfile(char const *dir_name) noexcept {
        if (std::filesystem::is_directory(dir_name)) {
            ReporterPtr result = std::make_unique<ReporterImpl>(dir_name);
            return Ok(std::move(result));
        } else {
            const std::string message = std::string("Directory does not exists: ") + dir_name;
            return Err(std::runtime_error(message));
        }
    }
}
