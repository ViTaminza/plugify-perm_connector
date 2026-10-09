# perm_connector — Plugify C++ plugin linking CS2 players (s2sdk) to the official permissions plugin.
#
#   plugify-perm_connector  the plugin shared library (entry "bin/perm_connector" -> bin/libperm_connector.so)

set(PERM_CONNECTOR_VERSION "1.0.0")

add_library(plugify-perm_connector SHARED
        src/plugin.cpp
        # Binding globals (__s2sdk_* / __permissions_*) filled by the plugify module from the
        # plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(plugify-perm_connector PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/permissions/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-perm_connector SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-perm_connector PRIVATE
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-perm_connector PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-perm_connector PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="perm_connector"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-perm_connector PROPERTIES OUTPUT_NAME perm_connector)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-perm_connector PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/perm_connector.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/perm_connector.pplugin"
)
