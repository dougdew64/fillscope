// Dear ImGui's configuration for the viewer's UI tests: the Test Engine's hooks enabled, and
// ImGui's asserts reported through the Test Engine. Passed to ImGui with IMGUI_USER_CONFIG.
#pragma once

#define IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL 1
#define IM_ASSERT(_EXPR) IM_TEST_ENGINE_ASSERT(_EXPR)
#include "imgui_te_imconfig.h"
