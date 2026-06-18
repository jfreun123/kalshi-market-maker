find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(NOT DOXYGEN_FOUND)
  message(STATUS "Doxygen not found — 'docs' target unavailable")
  return()
endif()

set(DOXYGEN_IN  "${PROJECT_SOURCE_DIR}/docs/Doxyfile")
set(DOXYGEN_OUT "${PROJECT_SOURCE_DIR}/docs/Doxyfile.configured")

configure_file("${DOXYGEN_IN}" "${DOXYGEN_OUT}" @ONLY)

add_custom_target(docs
  COMMAND "${DOXYGEN_EXECUTABLE}" "${DOXYGEN_OUT}"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/docs"
  COMMENT "Generating API documentation with Doxygen"
  VERBATIM
)
