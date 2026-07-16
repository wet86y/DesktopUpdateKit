#pragma once

#include "DesktopUpdateKit/UpdateKit.h"

#include <cstddef>
#include <span>

namespace desktop_update_kit::detail {

std::size_t next_accelerated_node_index(
    std::span<const DownloadNode> nodes, std::size_t current_index) noexcept;

} // namespace desktop_update_kit::detail
