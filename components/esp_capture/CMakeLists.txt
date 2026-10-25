# Gather common sources
set(COMPONENT_SRC_DIRS
    "src"
    "src/utils"
    "impl/capture_gmf_path/src"
    "impl/capture_gmf_path/src/elements"
)

set(COMPONENT_INCLUDE
  "./include" ./include/impl "./interface"
  "impl/capture_gmf_path/include"
)

set(COMPONENT_PRIV_INCLUDE
    "private_inc"
    "impl/capture_gmf_path/include/elements"
)

if(CONFIG_ESP_CAPTURE_ENABLE_AUDIO)
    list(APPEND COMPONENT_SOURCES "impl/capture_audio_src/capture_audio_dev_src.c")
endif()

if(CONFIG_ESP_CAPTURE_ENABLE_VIDEO)
    list(APPEND COMPONENT_SOURCES "impl/capture_video_src/capture_video_v4l2_src.c")
endif()

# ESP-IDF ignores SRC_DIRS if SRCS is also provided. Collect common sources
# explicitly before adding the two board-specific source implementations.
foreach(source_dir ${COMPONENT_SRC_DIRS})
    file(GLOB dir_sources "${CMAKE_CURRENT_LIST_DIR}/${source_dir}/*.c")
    list(APPEND COMPONENT_SOURCES ${dir_sources})
endforeach()

idf_component_register(
    SRCS ${COMPONENT_SOURCES}
    INCLUDE_DIRS ${COMPONENT_INCLUDE}
    PRIV_INCLUDE_DIRS ${COMPONENT_PRIV_INCLUDE}
    REQUIRES esp_timer
)

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
