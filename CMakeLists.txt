cmake_minimum_required(VERSION 3.10)
project(alsa_switch)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_library(asound_module_pcm_tssdk_alsa_switch SHARED AlsaSwitch.cpp)

target_include_directories(asound_module_pcm_tssdk_alsa_switch PRIVATE ${ALSA_INCLUDE_DIRS})
target_link_libraries(asound_module_pcm_tssdk_alsa_switch ${ALSA_LIBRARIES})

target_compile_options(asound_module_pcm_tssdk_alsa_switch PRIVATE -Wall -Werror)
target_compile_definitions(asound_module_pcm_tssdk_alsa_switch PRIVATE PIC)

set_target_properties(asound_module_pcm_tssdk_alsa_switch PROPERTIES
    PREFIX "lib"
    SUFFIX ".so"
)

add_library(asound_module_ctl_tssdk_alsa_switch SHARED AlsaSwitchCtl.cpp)

target_include_directories(asound_module_ctl_tssdk_alsa_switch PRIVATE ${ALSA_INCLUDE_DIRS})
target_link_libraries(asound_module_ctl_tssdk_alsa_switch ${ALSA_LIBRARIES})

target_compile_options(asound_module_ctl_tssdk_alsa_switch PRIVATE -Wall -Werror)
target_compile_definitions(asound_module_ctl_tssdk_alsa_switch PRIVATE PIC)

set_target_properties(asound_module_ctl_tssdk_alsa_switch PROPERTIES
    PREFIX "lib"
    SUFFIX ".so"
)

install(TARGETS asound_module_pcm_tssdk_alsa_switch asound_module_ctl_tssdk_alsa_switch
    LIBRARY DESTINATION /usr/lib/alsa-lib/
)