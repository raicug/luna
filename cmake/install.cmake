include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(LUNA_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/luna")

install(TARGETS luna
    EXPORT lunaTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/luna DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT lunaTargets
    NAMESPACE luna::
    DESTINATION ${LUNA_CMAKE_DIR}
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lunaConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lunaConfig.cmake"
    INSTALL_DESTINATION ${LUNA_CMAKE_DIR}
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/lunaConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/lunaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/lunaConfigVersion.cmake"
    DESTINATION ${LUNA_CMAKE_DIR}
)

# The self-contained flavour: luna_bundled plus the Luau headers luna's public
# headers include, in the same directory, so no Luau setup is needed.
if(LUNA_BUNDLE_LUAU)
    install(FILES "${LUNA_BUNDLE_FILE}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    foreach(_dir ${LUNA_LUAU_INCLUDE_DIRS})
        foreach(_header lua.h lualib.h luaconf.h)
            if(EXISTS "${_dir}/${_header}")
                install(FILES "${_dir}/${_header}" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
            endif()
        endforeach()
    endforeach()
endif()
