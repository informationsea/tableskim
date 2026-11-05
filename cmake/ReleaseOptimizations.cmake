# Single-config Debug builds compile dependencies as Release internally. Keep
# those builds out of this policy; multi-config generators select IPO per config.
string(TOUPPER "${CMAKE_BUILD_TYPE};${CMAKE_CONFIGURATION_TYPES}" tableskim_configurations)
if(NOT "RELEASE" IN_LIST tableskim_configurations)
    return()
endif()

include(CheckIPOSupported)
function(tableskim_check_release_ipo)
    # CheckIPOSupported uses a Debug try-compile with CMP0138 NEW. Check the
    # Release flags instead: MSVC's Debug /RTC1 is incompatible with /GL.
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_RELEASE}")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_RELEASE}")
    check_ipo_supported(RESULT supported OUTPUT error LANGUAGES C CXX)
    if(NOT supported)
        message(FATAL_ERROR "Release builds require link-time optimization: ${error}")
    endif()
endfunction()
tableskim_check_release_ipo()

# Older dependencies (miniz) predate CMP0069 and would otherwise ignore IPO.
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Keep the compiler's Release speed optimizations, and discard unused functions
# and data at link time. Each platform's linker uses its own spelling.
if(MSVC)
    add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gy>"
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gw>")
    add_link_options("$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
elseif(APPLE)
    # Mach-O already supports stripping individual functions and data symbols.
    add_link_options("$<$<CONFIG:Release>:-Wl,-dead_strip>")
else()
    add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-ffunction-sections>"
        "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:-fdata-sections>")
    add_link_options("$<$<CONFIG:Release>:-Wl,--gc-sections>")
endif()

message(STATUS "Release builds: link-time optimization and unused-code elimination enabled")
