cmake_minimum_required(VERSION 3.20)
project(SavvyAttendanceCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(SAVVY_ATTENDANCE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
set(SQLITE_AMALGAMATION_DIR "" CACHE PATH "Path to a folder containing sqlite3.c and sqlite3.h")

find_package(OpenCV REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets Network Concurrent)

set(SQLITE_SOURCE_FILE "")
set(SQLITE_INCLUDE_DIR "")

if(SQLITE_AMALGAMATION_DIR)
    if(EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c" AND EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.h")
        set(SQLITE_SOURCE_FILE "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
        set(SQLITE_INCLUDE_DIR "${SQLITE_AMALGAMATION_DIR}")
    else()
        message(FATAL_ERROR "SQLITE_AMALGAMATION_DIR must contain sqlite3.c and sqlite3.h")
    endif()
else()
    find_package(SQLite3 REQUIRED)
    set(SQLITE_INCLUDE_DIR "${SQLite3_INCLUDE_DIR}")
endif()

add_executable(SavvyAttendanceQt
    src/qt_main.cpp
    ${SQLITE_SOURCE_FILE}
    app_icon.rc
)

target_include_directories(SavvyAttendanceQt PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${SQLITE_INCLUDE_DIR}
)

target_link_libraries(SavvyAttendanceQt PRIVATE ${OpenCV_LIBS} Qt6::Widgets Qt6::Network Qt6::Concurrent)

add_custom_command(TARGET SavvyAttendanceQt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/logo.png"
        "$<TARGET_FILE_DIR:SavvyAttendanceQt>/logo.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/app_icon.ico"
        "$<TARGET_FILE_DIR:SavvyAttendanceQt>/app_icon.ico"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/labels.txt"
        "$<TARGET_FILE_DIR:SavvyAttendanceQt>/labels.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/face_detection_yunet_2023mar.onnx"
        "$<TARGET_FILE_DIR:SavvyAttendanceQt>/face_detection_yunet_2023mar.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/face_embedding.onnx"
        "$<TARGET_FILE_DIR:SavvyAttendanceQt>/face_embedding.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/face_embeddings.yml.gz"
        "$<TARGET_FILE_DIR:SavvyAttendanceQt>/face_embeddings.yml.gz"
)

if(NOT SQLITE_SOURCE_FILE)
    target_link_libraries(SavvyAttendanceQt PRIVATE SQLite::SQLite3)
endif()

if(MSVC)
    target_compile_options(SavvyAttendanceQt PRIVATE /W4 /EHsc)
    target_link_libraries(SavvyAttendanceQt PRIVATE winmm)
    if(SAVVY_ATTENDANCE_WARNINGS_AS_ERRORS)
        target_compile_options(SavvyAttendanceQt PRIVATE /WX)
    endif()
else()
    target_compile_options(SavvyAttendanceQt PRIVATE -Wall -Wextra -Wpedantic)
    if(SAVVY_ATTENDANCE_WARNINGS_AS_ERRORS)
        target_compile_options(SavvyAttendanceQt PRIVATE -Werror)
    endif()
endif()

install(TARGETS SavvyAttendanceQt RUNTIME DESTINATION .)

install(FILES
    ${CMAKE_SOURCE_DIR}/README.md
    DESTINATION .
)
