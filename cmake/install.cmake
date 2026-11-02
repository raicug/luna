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
