include(${DONUT_DIR}/compileshaders.cmake)


project(triangle)


set(folder "sanbox/triangle")
file(GLOB shaders "shaders/*.hlsl")
file(GLOB sources "*.cpp" "*.h")

donut_compile_shaders_all_platforms(
    TARGET ${PROJECT_NAME}_shaders
    PROJECT_NAME ${PROJECT_NAME}
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg
    FOLDER ${folder}
    OUTPUT_BASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${PROJECT_NAME}
)

add_executable(${PROJECT_NAME} WIN32 ${sources})
target_link_libraries(${PROJECT_NAME} donut_app donut_engine)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_shaders)
set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER ${folder})
target_compile_definitions(${PROJECT_NAME} PRIVATE PROJECT_NAME=${PROJECT_NAME})

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()