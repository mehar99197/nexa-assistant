# The contract between Nexa and its root helper. Both sides generate their D-Bus code from
# org.nexa.Helper.xml, so the proxy and the adaptor cannot drift apart: the proxy is built here
# (HelperProxy, used by system/helper/HelperClient) and the adaptor in helper/.
add_library(nexa_proto STATIC
    HelperTypes.h
    KeyMap.h KeyMap.cc
)

if(UNIX AND NOT ANDROID AND NOT APPLE)
    set(NEXA_HELPER_XML "${CMAKE_CURRENT_SOURCE_DIR}/org.nexa.Helper.xml"
        CACHE INTERNAL "D-Bus contract of nexa-helper")
    set_source_files_properties("${NEXA_HELPER_XML}" PROPERTIES INCLUDE "proto/HelperTypes.h" CLASSNAME HelperProxy)
    qt_add_dbus_interface(nexa_proto_generated "${NEXA_HELPER_XML}" HelperProxy)
    target_sources(nexa_proto PRIVATE ${nexa_proto_generated})
endif()

# Includes are rooted at the project directory; the generated header lands in the binary directory.
target_include_directories(nexa_proto PUBLIC "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(nexa_proto
    PUBLIC
        nexa_core
        $<$<AND:$<BOOL:${UNIX}>,$<NOT:$<BOOL:${ANDROID}>>,$<NOT:$<BOOL:${APPLE}>>>:Qt6::DBus>
    PRIVATE
        nexa_options
)
