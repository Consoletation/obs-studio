cmake_minimum_required(VERSION 3.5)

project(obs-voicemeeter)

include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/UI/obs-frontend-api")
include_directories(
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
)

set(obs-voicemeeter_HEADERS
	VoicemeeterRemote.h
	circle-buffer.h
)

set(obs-voicemeeter_SOURCES
	obs-voicemeeter.cpp
)

add_library(obs-voicemeeter MODULE
	${obs-voicemeeter_HEADERS}
	${obs-voicemeeter_SOURCES}
)

target_link_libraries(obs-voicemeeter
	libobs
        obs-frontend-api
        Qt5::Core
        Qt5::Widgets
)

install_obs_plugin_with_data(obs-voicemeeter data)
