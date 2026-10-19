# The repository's Markdown documents: those at the top level, and any under docs/, src/, tools/ or
# tests/. Not CLAUDE.local.md, which is untracked and per-machine, and nothing under node_modules.
# Each document is a run-time file of docs_tests. Its result also depends on every file a link
# names, which could be any file in the repository, so it is ALWAYS run: it takes milliseconds.
file(GLOB documents CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/*.md)
file(GLOB_RECURSE nested_documents CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/docs/*.md ${PROJECT_SOURCE_DIR}/src/*.md
  ${PROJECT_SOURCE_DIR}/tools/*.md ${PROJECT_SOURCE_DIR}/tests/*.md)
list(APPEND documents ${nested_documents})
list(FILTER documents EXCLUDE REGEX "/CLAUDE\\.local\\.md$")
list(FILTER documents EXCLUDE REGEX "/node_modules/")  # installed packages' documents aren't ours
list(SORT documents)
list(JOIN documents "\n" document_list)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/documents.txt CONTENT "${document_list}\n")

fillscope_gtest(docs_tests unit ALWAYS SOURCES doc_links_test.cpp RUNTIME_DEPS ${documents})
target_compile_definitions(docs_tests PRIVATE FILLSCOPE_DOC_LIST="${CMAKE_CURRENT_BINARY_DIR}/documents.txt")
