# nanopb-generated bindings for the aioesphomeapi subset we speak.
#
# Setup (one-time, on the build host):
#   cd components/api_proto && git clone --depth 1 \
#       https://github.com/nanopb/nanopb.git nanopb
#   pip install protobuf grpcio-tools
#
# CMake then runs nanopb_generator.py automatically when api_subset.proto
# or api_subset.options changes. The generated .pb.{c,h} are gitignored.

set(NANOPB_DIR "${CMAKE_CURRENT_LIST_DIR}/nanopb")

if(NOT EXISTS "${NANOPB_DIR}/generator/nanopb_generator.py")
    message(FATAL_ERROR
        "nanopb not vendored at ${NANOPB_DIR}.\n"
        "Run: cd components/api_proto && git clone --depth 1 "
        "https://github.com/nanopb/nanopb.git nanopb")
endif()

set(PROTO_SRC  "${CMAKE_CURRENT_LIST_DIR}/api_subset.proto")
set(PROTO_OPTS "${CMAKE_CURRENT_LIST_DIR}/api_subset.options")
set(GEN_C      "${CMAKE_CURRENT_LIST_DIR}/api_subset.pb.c")
set(GEN_H      "${CMAKE_CURRENT_LIST_DIR}/api_subset.pb.h")

idf_component_register(
    SRCS
        "${GEN_C}"
        "${NANOPB_DIR}/pb_common.c"
        "${NANOPB_DIR}/pb_encode.c"
        "${NANOPB_DIR}/pb_decode.c"
    INCLUDE_DIRS
        "."
        "${NANOPB_DIR}"
)

# ESP-IDF parses this file twice — once in script mode for requirements
# gathering (where add_custom_command/add_dependencies fail), then again
# for the real build. Guard the build-only commands.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # nanopb_generator wants the .proto path to be expressible relative
    # to its -I include dirs. Run it from the proto's directory and pass
    # the basename so protoc is happy.
    add_custom_command(
        OUTPUT  "${GEN_C}" "${GEN_H}"
        COMMAND python3 "${NANOPB_DIR}/generator/nanopb_generator.py"
                api_subset.proto
        DEPENDS "${PROTO_SRC}" "${PROTO_OPTS}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        COMMENT "[nanopb] generating api_subset.pb.{c,h}"
        VERBATIM
    )
    add_custom_target(api_subset_protoc DEPENDS "${GEN_C}" "${GEN_H}")
    add_dependencies(${COMPONENT_LIB} api_subset_protoc)
endif()
