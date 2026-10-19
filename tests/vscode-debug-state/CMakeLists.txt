# The VS Code extension that publishes the debugger's state (tools/vscode-debug-state), tested with
# Node.js: its unit tests, and a type check of its JavaScript by the TypeScript compiler.
find_program(NODE node)
find_program(NPM npm)
if(NOT NODE OR NOT NPM)
  message(FATAL_ERROR "The extension's tests need Node.js and npm (Fedora: dnf install nodejs npm). "
                      "To build without any tests, configure with -DFILLSCOPE_TESTS=OFF.")
endif()

set(EXTENSION ${PROJECT_SOURCE_DIR}/tools/vscode-debug-state)

# The development tools (the compiler and type definitions), installed exactly as locked whenever
# the package files change. node_modules is git-ignored.
# Both builds share node_modules, so the install holds an exclusive lock on the extension's
# directory, and the tests and type check a shared one: npm ci never deletes the packages while
# the other build's check is reading them.
find_program(FLOCK flock REQUIRED)  # util-linux
add_custom_command(
  OUTPUT ${EXTENSION}/node_modules/.package-lock.json
  COMMAND ${FLOCK} ${EXTENSION} ${NPM} ci --no-audit --no-fund
  DEPENDS ${EXTENSION}/package.json ${EXTENSION}/package-lock.json
  WORKING_DIRECTORY ${EXTENSION}
  COMMENT "Installing the extension's development tools (npm ci)")
add_custom_target(vscode_debug_state_tools ALL DEPENDS ${EXTENSION}/node_modules/.package-lock.json)

# Every file the type check reads (tsconfig.json's "include") is a run-time file of both, so the
# two can't cover different files. Node runs only the *.test.js files.
file(GLOB_RECURSE extension_sources CONFIGURE_DEPENDS ${EXTENSION}/src/*.js)
file(GLOB_RECURSE extension_test_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.js)
file(GLOB extension_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.test.js)
set(extension_files ${extension_sources} ${extension_test_files}
  ${EXTENSION}/package.json ${EXTENSION}/package-lock.json ${EXTENSION}/tsconfig.json)

fillscope_test_program(vscode_debug_state_tests unit
  COMMAND ${FLOCK} --shared ${EXTENSION} ${NODE} --test ${extension_tests}
  RUNTIME_DEPS ${extension_files})
fillscope_test_program(vscode_debug_state_typecheck check
  COMMAND ${FLOCK} --shared ${EXTENSION} ${NODE} ${EXTENSION}/node_modules/typescript/bin/tsc -p ${EXTENSION}
  RUNTIME_DEPS ${extension_files})
