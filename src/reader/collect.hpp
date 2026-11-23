#pragma once

// What agent-reader.exe reads (phase 13a § 6): the boot log, Defender's history, the tasks run as
// SYSTEM, every process's image, the sizes of folders closed to the user and Defender's exclusions.
// Reading only; what cannot be read goes into not_read in words.

#include "tools/winsys/reader_data.hpp"

namespace reader {

[[nodiscard]] auto collect() -> tools::winsys::reader::Data;

} // namespace reader
