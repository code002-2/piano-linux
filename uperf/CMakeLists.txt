cmake_minimum_required(VERSION 3.20)
project(uperf-linux VERSION 0.1.0 LANGUAGES C CXX)

# This is a system daemon. Keep the default layout aligned with the Debian
# package while still allowing an explicit prefix override.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings as errors for production build
set(CMAKE_C_FLAGS_RELEASE "-O2 -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g -Wall -Wextra")

option(BUILD_GUI "Build GTK4 GUI" ON)
option(BUILD_TESTS "Build tests" ON)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-gobject-1)

# GLib for DBus integration (daemon side)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.56)

# Optional: libsystemd for journald integration
find_path(LIBSYSTEMD_INCLUDE_DIR NAMES systemd/sd-journal.h
          PATHS /usr/include /usr/local/include /usr/include/systemd)
find_library(LIBSYSTEMD_LIBRARY NAMES systemd
             PATHS /usr/lib /usr/local/lib)

# ---- Daemon sources ----
set(UPERF_SOURCES
    src/log.c
    src/config_parser.c
    src/sysfs_writer.c
    src/power_model.c
    src/frequency_controller.c
    src/frequency_state.c
    src/state_machine.c
    src/input_monitor.c
    src/task_scheduler.c
    src/cgroup_manager.c
    src/heavyload_detector.c
    src/game_scanner.c
    src/thermal.c
    src/perapp_config.c
    src/dbus_interface.c
    src/main.c
)

set(UPERF_HEADERS
    src/include/log.h
    src/include/config.h
    src/include/sysfs_writer.h
    src/include/power_model.h
    src/include/frequency_controller.h
    src/include/frequency_state.h
    src/include/state_machine.h
    src/include/input_monitor.h
    src/include/task_scheduler.h
    src/include/cgroup_manager.h
    src/include/heavyload_detector.h
    src/include/game_scanner.h
    src/include/thermal.h
    src/include/perapp_config.h
    src/include/dbus_interface.h
)

# ---- Main daemon binary ----
add_executable(uperf-linux ${UPERF_SOURCES} ${UPERF_HEADERS})

target_include_directories(uperf-linux PRIVATE
    ${JSONC_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${LIBSYSTEMD_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_libraries(uperf-linux PRIVATE m gio-2.0 dbus-1 gobject-2.0
    PkgConfig::JSONC
    PkgConfig::POLKIT
    ${GLIB2_LIBRARIES}
    pthread
    rt
)

if(LIBSYSTEMD_LIBRARY AND LIBSYSTEMD_INCLUDE_DIR)
    target_link_libraries(uperf-linux PRIVATE ${LIBSYSTEMD_LIBRARY})
    target_compile_definitions(uperf-linux PRIVATE HAVE_LIBSYSTEMD=1)
endif()

# ---- CLI tool ----
add_executable(uperfctl src/cli.c)
target_include_directories(uperfctl PRIVATE
    ${GLIB2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)
target_link_libraries(uperfctl PRIVATE
    gio-2.0 gobject-2.0 ${GLIB2_LIBRARIES}
)

# ---- Config wizard ----
add_executable(uperf-wizard src/config_wizard.c src/log.c)
target_include_directories(uperf-wizard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)
target_link_libraries(uperf-wizard PRIVATE
    pthread rt
)

# ---- Tests ----
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_config_parser
        tests/test_config_parser.c
        src/config_parser.c
        src/log.c
    )
    target_include_directories(test_config_parser PRIVATE
        ${JSONC_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_config_parser PRIVATE PkgConfig::JSONC pthread)

    add_executable(test_state_machine tests/test_state_machine.c src/log.c src/state_machine.c)
    target_include_directories(test_state_machine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_state_machine PRIVATE pthread)

    add_executable(test_power_model tests/test_power_model.c src/log.c src/power_model.c)
    target_include_directories(test_power_model PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_power_model PRIVATE pthread)

    add_executable(test_frequency_controller
        tests/test_frequency_controller.c
        src/frequency_controller.c
        src/power_model.c
        src/log.c
    )
    target_include_directories(test_frequency_controller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_frequency_controller PRIVATE m pthread)

    add_executable(test_sysfs_writer tests/test_sysfs_writer.c src/log.c src/sysfs_writer.c)
    target_include_directories(test_sysfs_writer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_sysfs_writer PRIVATE pthread)

    add_executable(test_game_scanner tests/test_game_scanner.c src/log.c src/game_scanner.c src/perapp_config.c)
    target_include_directories(test_game_scanner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_game_scanner PRIVATE pthread)

    add_executable(test_perapp_config tests/test_perapp_config.c src/log.c src/perapp_config.c)
    target_include_directories(test_perapp_config PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_perapp_config PRIVATE pthread)

    add_executable(test_thermal tests/test_thermal.c src/log.c src/thermal.c)
    target_include_directories(test_thermal PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_thermal PRIVATE pthread)

    add_executable(test_input_monitor
        tests/test_input_monitor.c
        src/log.c
        src/input_monitor.c
    )
    target_include_directories(test_input_monitor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_input_monitor PRIVATE pthread m)

    add_executable(test_heavyload_detector
        tests/test_heavyload_detector.c
        src/log.c
        src/heavyload_detector.c
    )
    target_include_directories(test_heavyload_detector PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_heavyload_detector PRIVATE pthread rt)

    add_executable(test_task_scheduler
        tests/test_task_scheduler.c
        src/task_scheduler.c
        src/log.c
    )
    target_include_directories(test_task_scheduler PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_task_scheduler PRIVATE pthread rt)

    if(LIBSYSTEMD_LIBRARY AND LIBSYSTEMD_INCLUDE_DIR)
        add_executable(test_cgroup_manager
            tests/test_cgroup_manager.c
            src/cgroup_manager.c
            src/log.c
        )
        target_include_directories(test_cgroup_manager PRIVATE
            ${LIBSYSTEMD_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        )
        target_compile_definitions(test_cgroup_manager PRIVATE
            HAVE_LIBSYSTEMD=1)
        target_link_libraries(test_cgroup_manager PRIVATE
            ${LIBSYSTEMD_LIBRARY} pthread)
    endif()

    add_executable(test_dbus_interface
        tests/test_dbus_interface.c
        src/log.c
        src/dbus_interface.c
    )
    target_include_directories(test_dbus_interface PRIVATE
        ${GLIB2_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_dbus_interface PRIVATE
        gio-2.0 gobject-2.0 ${GLIB2_LIBRARIES} PkgConfig::POLKIT pthread
    )

    add_executable(test_config_file
        tests/test_config_file.c
        src/config_parser.c
        src/log.c
    )
    target_include_directories(test_config_file PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(test_config_file PRIVATE PkgConfig::JSONC pthread)

    add_test(NAME config_parser COMMAND test_config_parser)
    add_test(NAME state_machine COMMAND test_state_machine)
    add_test(NAME power_model COMMAND test_power_model)
    add_test(NAME frequency_controller COMMAND test_frequency_controller)

    add_executable(test_frequency_state
        tests/test_frequency_state.c
        src/frequency_state.c
    )
    target_include_directories(test_frequency_state PRIVATE src/include)
    target_compile_options(test_frequency_state PRIVATE -Wall -Wextra -Werror)
    add_test(NAME frequency_state COMMAND test_frequency_state)
    add_test(NAME sysfs_writer COMMAND test_sysfs_writer)
    add_test(NAME game_scanner COMMAND test_game_scanner)
    add_test(NAME perapp_config COMMAND test_perapp_config)
    add_test(NAME thermal COMMAND test_thermal)
    add_test(NAME input_monitor COMMAND test_input_monitor)
    add_test(NAME heavyload_detector COMMAND test_heavyload_detector)
    add_test(NAME task_scheduler COMMAND test_task_scheduler)
    if(TARGET test_cgroup_manager)
        add_test(NAME cgroup_manager COMMAND test_cgroup_manager)
        set_tests_properties(cgroup_manager PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME dbus_interface COMMAND test_dbus_interface)
    add_test(NAME wizard_detect COMMAND uperf-wizard detect --output
        ${CMAKE_CURRENT_BINARY_DIR}/wizard-detected.json)
    add_test(NAME wizard_config COMMAND test_config_file
        ${CMAKE_CURRENT_BINARY_DIR}/wizard-detected.json)
    set_tests_properties(wizard_config PROPERTIES DEPENDS wizard_detect)
    add_test(NAME wizard_stdout COMMAND sh -c
        "$<TARGET_FILE:uperf-wizard> detect > '${CMAKE_CURRENT_BINARY_DIR}/wizard-stdout.json'")
    add_test(NAME wizard_stdout_config COMMAND test_config_file
        ${CMAKE_CURRENT_BINARY_DIR}/wizard-stdout.json)
    set_tests_properties(wizard_stdout_config PROPERTIES DEPENDS wizard_stdout)

    set_tests_properties(config_parser PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

# ---- GTK4/libadwaita GUI ----
if(BUILD_GUI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED gtk4>=4.6)
    pkg_check_modules(LIBADWAITA REQUIRED libadwaita-1>=1.2)
    add_subdirectory(gui)
endif()

# ---- Install targets ----
install(TARGETS uperf-linux uperfctl uperf-wizard
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES
    config/perapp_powermode
    DESTINATION /etc/uperf-linux
)
install(FILES
    config/sm8550.json
    config/dbus-interface.xml
    DESTINATION share/uperf-linux
)
install(FILES config/sm8550.json
    DESTINATION /etc/uperf-linux
    RENAME config.json
)
install(FILES systemd/uperf-linux.service DESTINATION lib/systemd/system)
install(FILES config/org.uperflinux.Daemon.conf
    DESTINATION share/dbus-1/system.d
)
install(FILES config/org.uperflinux.policy
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions
)

# ---- Deb packaging ----
set(CPACK_PACKAGE_NAME "uperf-linux")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Userspace performance scheduler for Linux ARM64")
set(CPACK_PACKAGE_VENDOR "code002-2")
set(CPACK_PACKAGE_CONTACT "hiez832670@gmail.com")
set(CPACK_PACKAGE_FILE_NAME "uperf-linux-${CPACK_PACKAGE_VERSION}-arm64")
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libglib2.0-0 (>= 2.0), libdbus-1-3 (>= 1.0), libjson-c5 (>= 0.12), libsystemd0 (>= 209), libpolkit-gobject-1-0 (>= 0.105), polkitd, libgtk-4-1 (>= 4.6), libadwaita-1-0 (>= 1.2)")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/conffiles"
)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)
# Runtime dependencies are declared explicitly above for predictable ARM64 packages.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
include(CPack)
