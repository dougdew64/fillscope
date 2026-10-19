// Lets SIGINT (Ctrl-C) and SIGTERM (what VS Code's Stop and `kill` send) end the viewer's loop
// rather than the process, so the viewer can write its last context file, marked as no longer
// running, on the ordinary ways of stopping it.
#pragma once

#include <expected>
#include <system_error>

namespace fillscope {

// Installs the handlers. Each signal's handler then reverts to the default, so a second Ctrl-C
// ends a viewer that is stuck instead of being asked to stop again.
[[nodiscard]] std::expected<void, std::error_code> install_stop_signals();

// Whether SIGINT or SIGTERM has arrived since install_stop_signals().
[[nodiscard]] bool stop_requested() noexcept;

}  // namespace fillscope
