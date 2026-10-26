// The viewer's window size on this screen (window_size, viewer_view.h), for the programs that open
// a window through GLFW: the viewer, and the demo when watched. GLFW must be initialized.
#pragma once

#include "viewer/viewer_view.h"

#include <GLFW/glfw3.h>

namespace fillscope {

[[nodiscard]] inline WindowSize primary_window_size() {
    int x = 0;
    int y = 0;
    int area_width = 0;
    int area_height = 0;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorWorkarea(monitor, &x, &y, &area_width, &area_height);
    }
    return window_size(area_width, area_height);
}

}  // namespace fillscope
