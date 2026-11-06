cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXECUTABLE}"
    DIRECTORIES "${DEPENDENCY_DIRECTORY}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\].*")
if(unresolved)
    message(FATAL_ERROR "Unresolved tool runtime dependencies: ${unresolved}")
endif()
get_filename_component(destination "${EXECUTABLE}" DIRECTORY)
foreach(library IN LISTS resolved)
    get_filename_component(name "${library}" NAME)
    file(REAL_PATH "${library}" original)
    set(target "${destination}/${name}")
    if(EXISTS "${target}")
        file(REAL_PATH "${target}" target)
    endif()
    if(NOT original STREQUAL target)
        file(COPY_FILE "${library}" "${destination}/${name}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
