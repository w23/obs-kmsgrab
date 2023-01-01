cmake_minimum_required(VERSION 3.10)

project(linux-kmsgrab VERSION 0.1.0)

# Replace `Your Name Here` with the name (yours or your organization's) you want
# to see as the author of the plugin (in the plugin's metadata itself and in the installers)
set(PLUGIN_AUTHOR "Ivan Avdeev")

# Replace `me@contoso.com` with the maintainer email address you want to put in Linux packages
set(LINUX_MAINTAINER_EMAIL "me@w23.ru")

if(NOT UNIX OR APPLE)
	message(FATAL_ERROR "This plugin is Linux-only")
endif()

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# In case you need C++
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(libobs REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core Widgets)

# LOL TODO for this we need cmake >= 3.19 w/ cmake policy CMP0109 set to NEW
#find_program(POLKIT NAMES pkexec)
option(ENABLE_POLKIT "Use pkexec for elevated drmsend privileges" ON)

find_package(PkgConfig)
pkg_check_modules(DRM libdrm)

if(NOT DRM_FOUND)
	message(FATAL_ERROR "libdrm(-dev) not found")
	return()
endif()

include(GNUInstallDirs)

set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")

configure_file(
	src/plugin-macros.h.in
	../src/plugin-macros.generated.h
)

configure_file(
	ci/ci_includes.sh.in
	../ci/ci_includes.generated.sh
)

set(PLUGIN_SOURCES
	src/dmabuf.c
	src/drm-helpers.c
	src/xcursor-xcb.c)

set(PLUGIN_HEADERS
	src/plugin-macros.generated.h)

add_library(${CMAKE_PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

if (ENABLE_POLKIT)
	message(STATUS "Using Polkit/pkexec for elevating drmsend privileges")
	target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE USE_PKEXEC)
else()
	message(WARNING "Polkit support is disabled. You'll need to manually run `sudo setcap cap_sys_admin+ep \"${OBS_PLUGIN_DESTINATION}/obs-plugins/linux-kmsgrab-send\"` to allow it to grab framebuffers")
endif()

include_directories(
	"${CMAKE_SOURCE_DIR}/src"
	"${LIBOBS_INCLUDE_DIR}/../UI/obs-frontend-api"
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
	obs
	xcb
	xcb-xfixes
	Qt5::Core
	Qt5::Widgets
)

add_executable(linux-kmsgrab-send src/drmsend.c)
target_include_directories(linux-kmsgrab-send PRIVATE ${DRM_INCLUDE_DIRS})
target_link_libraries(linux-kmsgrab-send PRIVATE ${DRM_LIBRARIES})

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
target_link_libraries(${CMAKE_PROJECT_NAME} obs-frontend-api)

file(GLOB locale_files data/locale/*.ini)

install(TARGETS ${CMAKE_PROJECT_NAME} linux-kmsgrab-send
	LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}"
	RUNTIME DESTINATION "${OBS_PLUGIN_DESTINATION}")

install(FILES ${locale_files}
	DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/obs/obs-plugins/${CMAKE_PROJECT_NAME}/locale")

