// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <vector>

namespace pdfz {

// The PDF without the objects nothing refers to (from its trailer on), for a file PDFium wrote:
// one cross-reference table, no object streams, no incremental updates. Anything else, or
// anything it can't read, comes back as it is.
std::vector<std::uint8_t> withoutUnusedObjects(const std::vector<std::uint8_t>& pdf);

}  // namespace pdfz
