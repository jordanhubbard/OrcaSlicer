# OrcaSlicer Config Codegen CMake Module
#
# Generates C++ source files from protobuf schema definitions.
# Generated files live in src/slic3r/GUI/generated/ and are gitignored.
# Run 'python tools/run_codegen.py' (requires grpcio-tools or protoc) to regenerate.
#
# Targets:
#   codegen_config  - Custom target to regenerate C++ from .proto files
#   validate_config - Custom target to validate generated vs original
#
# Usage in parent CMakeLists.txt:
#   include(cmake/modules/ConfigCodegen.cmake)

find_program(PROTOC_EXECUTABLE protoc)
find_package(Python3 COMPONENTS Interpreter QUIET)

# If generated files are missing (fresh clone), run codegen immediately at configure time.
# This allows cmake configure + build to work without a separate pre-build step.
set(_generated_marker "${CMAKE_SOURCE_DIR}/src/slic3r/GUI/generated/PrintConfigDef_generated.cpp")
if(Python3_EXECUTABLE AND NOT EXISTS "${_generated_marker}")
    message(STATUS "Config codegen: generated files missing — running codegen now...")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/run_codegen.py" --no-validate
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _codegen_result
    )
    if(NOT _codegen_result EQUAL 0)
        message(FATAL_ERROR "Config codegen failed. Install grpcio-tools: pip install grpcio-tools")
    endif()
    message(STATUS "Config codegen: generated files created successfully")
elseif(NOT Python3_EXECUTABLE AND NOT EXISTS "${_generated_marker}")
    message(FATAL_ERROR "Config codegen: generated files missing and Python3 not found.\n"
        "Install Python and grpcio-tools: pip install grpcio-tools\n"
        "Then run: python tools/run_codegen.py")
endif()

set(CONFIG_PROTO_DIR   "${CMAKE_SOURCE_DIR}/src/PrintConfigs")
set(CONFIG_CODEGEN_DIR "${CMAKE_SOURCE_DIR}/src/slic3r/GUI/generated")
set(CONFIG_LAYOUT_YAML "${CMAKE_SOURCE_DIR}/src/PrintConfigs/layout.yaml")
set(CONFIG_DESC_FILE   "${CMAKE_BINARY_DIR}/config.desc")

set(CODEGEN_TOOL     "${CMAKE_SOURCE_DIR}/tools/config_codegen.py")
set(VALIDATE_TOOL    "${CMAKE_SOURCE_DIR}/tools/validate_codegen.py")
set(RUN_CODEGEN_TOOL "${CMAKE_SOURCE_DIR}/tools/run_codegen.py")

# Generated output files (TabLayout_generated.cpp is also generated from layout.yaml)
set(CONFIG_GENERATED_SOURCES
    "${CONFIG_CODEGEN_DIR}/PrintConfigDef_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/Preset_options_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/Invalidation_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/OptionKeys_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/TabLayout_generated.cpp"
)

# Collect all .proto source files (flat in src/PrintConfigs/, excluding config_metadata.proto)
file(GLOB CONFIG_PROTO_FILES
    "${CONFIG_PROTO_DIR}/filament.proto"
    "${CONFIG_PROTO_DIR}/print.proto"
    "${CONFIG_PROTO_DIR}/printer.proto"
)
set(CONFIG_PROTO_FILES
    "${CONFIG_PROTO_DIR}/config_metadata.proto"
    ${CONFIG_PROTO_FILES}
)

if(Python3_EXECUTABLE)
    # Single command: run_codegen.py handles protoc/grpcio-tools detection internally.
    # Proto files → generated .cpp files. Runs automatically when any .proto changes.
    add_custom_command(
        OUTPUT  ${CONFIG_GENERATED_SOURCES}
        COMMAND ${Python3_EXECUTABLE} ${RUN_CODEGEN_TOOL} --no-validate
        DEPENDS ${CONFIG_PROTO_FILES} ${CONFIG_LAYOUT_YAML} ${CODEGEN_TOOL}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Re-generating config C++ from changed .proto files"
        VERBATIM
    )

    # codegen_config is part of ALL — runs before every build, checks if protos changed.
    add_custom_target(codegen_config ALL
        DEPENDS ${CONFIG_GENERATED_SOURCES}
        COMMENT "Config codegen up to date"
    )

    # Validation target: cmake --build . --target validate_config
    add_custom_target(validate_config
        COMMAND ${Python3_EXECUTABLE} ${VALIDATE_TOOL}
        DEPENDS ${CONFIG_GENERATED_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Validating generated config code against PrintConfig.cpp"
        VERBATIM
    )

    # Export for use by subdirectories (libslic3r, etc.)
    set(CONFIG_GENERATED_SOURCES "${CONFIG_GENERATED_SOURCES}" CACHE INTERNAL "Generated config cpp files")

    message(STATUS "Config codegen: enabled — proto changes auto-regenerate on next build")
else()
    message(STATUS "Config codegen: Python3 not found — run: pip install grpcio-tools && python tools/run_codegen.py")
endif()
