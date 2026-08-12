cmake_minimum_required(VERSION 3.16)
project(pixelate VERSION 1.0.0 LANGUAGES C)

if(WIN32)
    enable_language(RC)
endif()

add_executable(pixelate pixelate.c)
target_compile_features(pixelate PRIVATE c_std_99)

if(WIN32)
    target_sources(pixelate PRIVATE pixelate.rc)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(pixelate PRIVATE m)
endif()

if(MSVC)
    target_compile_options(pixelate PRIVATE /W4)
else()
    target_compile_options(pixelate PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS pixelate RUNTIME DESTINATION bin)
