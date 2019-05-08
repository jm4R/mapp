cmake_minimum_required(VERSION 3.8)
project(mapp LANGUAGES CXX VERSION 0.1)

include_directories(
    ${PROJECT_SOURCE_DIR}/../src
)

add_executable(${PROJECT_NAME}
    ../src/mapp.hpp #just to see in project tree
    mapp_demo.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_11)

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME}
        -pthread
        -ldl
    )
endif()
