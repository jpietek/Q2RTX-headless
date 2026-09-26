cmake_minimum_required(VERSION 3.15)
project(vkpt_device_selection_tests C)
enable_testing()
add_executable(device_selection_test
    device_selection_test.c
    ../src/refresh/vkpt/device_selection.c)
target_include_directories(device_selection_test PRIVATE
    ../src/refresh/vkpt ../extern/Vulkan-Headers/include)
set_target_properties(device_selection_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(device_selection_test PRIVATE /W4 /WX)
else()
    target_compile_options(device_selection_test PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME device_selection COMMAND device_selection_test)
