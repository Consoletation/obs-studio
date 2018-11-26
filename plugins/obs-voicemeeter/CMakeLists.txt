cmake_minimum_required(VERSION 3.5)

project(obs-voicemeeter)

########################################
# find libobs                                                          #
########################################
#include(external/ObsPluginHelpers.cmake)
#include(external/FindLibObs.cmake)
#find_package(LibObs REQUIRED)

include_directories(
	/
#	"${LIBOBS_INCLUDE_DIR}/../UI/obs-frontend-api"
)

#set(OBS_FRONTEND_LIB "OBS_FRONTEND_LIB-NOTFOUND" CACHE FILEPATH "OBS frontend library")
#if(OBS_FRONTEND_LIB EQUAL "OBS_FRONTEND_LIB-NOTFOUND")
#	message(FATAL_ERROR "Could not find OBS Frontend API\'s library !")
#endif()

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
	"${OBS_FRONTEND_LIB}"
)

install_obs_plugin_with_data(obs-voicemeeter data)
#install_external_plugin_with_data(obs-voicemeeter data)
