#include "cli/console.hpp"

#include "core/utf8.hpp"
#include "core/win.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace cli {

void say(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

auto utf8(std::wstring_view w) -> std::string {
    auto s = core::utf8::to_utf8(w);
    return s ? std::move(*s) : std::string{};
}

auto data_dir() -> std::filesystem::path {
    if (const auto* own = _wgetenv(L"WA_AGENT_DATA"); own != nullptr && *own != L'\0') {
        return std::filesystem::path{own};
    }
    return core::win::agent_data_dir(); // data\ beside the program in a portable build, as agent.exe
}

auto ask_yes(std::string_view question, bool yes) -> bool {
    if (yes) {
        say(std::string{question} + "  -> yes (--yes)");
        return true;
    }
    std::fwrite(question.data(), 1, question.size(), stderr);
    std::fputs(" [y/N] ", stderr);
    std::fflush(stderr);
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        say("\n  -> no (no answer)");
        return false;
    }
    while (!answer.empty() && (answer.back() == '\r' || answer.back() == ' ')) {
        answer.pop_back();
    }
    return answer == "y" || answer == "Y" || answer == "yes";
}

auto confirm(bool yes) -> tools::Confirm {
    return [yes](const security::Ruling& r, std::string_view preview, const tools::Prepared*) -> tools::Consent {
        say("[ask] " + r.reason + (r.warning ? "  (the agent runs as administrator)" : ""));
        for (const auto& p : r.paths) {
            say("  " + utf8(p.path));
        }
        if (!preview.empty()) {
            say(std::string{preview});
        }
        return ask_yes("Allow?", yes);
    };
}

} // namespace cli
