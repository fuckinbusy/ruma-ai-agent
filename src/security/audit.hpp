#pragma once

// security::audit — audit.jsonl (03-SECURITY § Secrets and logs): one JSON line per tool call that
// reached the policy, chained by SHA-256 so a changed or removed record shows.
//
// A line is {"ts",…,"prev_hash":"<previous hash>","hash":"<hash>"}: `hash` is SHA-256 of the
// line's text up to `,"hash":"`, `prev_hash` the previous line's hash (64 zeros for the first).
// The check reads the text as written, so it needs no canonical JSON.

#include "core/error.hpp"
#include "core/win.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace security {

struct AuditRecord {
    std::string ts;          // UTC "2026-10-11T12:00:00.000Z"; filled in by append() when empty
    std::uint64_t task_id = 0;
    std::string tool;
    std::string args;        // the call's arguments as JSON text
    std::string zone;
    std::string decision;    // auto / ask / deny / hard-deny
    std::string decided_by;  // policy / user / session (a command whose start the task allowed)
    double duration_ms = 0;
    int result_code = 0;
};

class AuditLog {
public:
    // Appends to `file` (created with its folder). Refuses a file whose last line is not a whole
    // record: a chain is never continued from a damaged end.
    static auto open(const std::filesystem::path& file) -> core::Result<AuditLog>;
    // One line, written and flushed before it returns; tasks on several threads may call it at once.
    auto append(AuditRecord record) -> core::Result<void>;

private:
    AuditLog(core::win::FileHandle file, std::string prev)
        : file_(std::move(file)), prev_(std::move(prev)), mutex_(std::make_unique<std::mutex>()) {}
    core::win::FileHandle file_;
    std::string prev_; // the last line's hash
    std::unique_ptr<std::mutex> mutex_; // one record at a time: the chain is a sequence
};

// The number of records whose chain holds; an error names the first line where it breaks. A
// missing file holds 0 records.
auto verify_audit(const std::filesystem::path& file) -> core::Result<std::size_t>;
// The newest `max` records, oldest first (the audit screen); the chain is not checked here.
auto read_audit(const std::filesystem::path& file, std::size_t max) -> core::Result<std::vector<AuditRecord>>;

} // namespace security
