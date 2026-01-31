cmake_minimum_required(VERSION 3.16)
project(CorneaController VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt Configuration
set(CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64")
find_package(Qt5 REQUIRED COMPONENTS Core Widgets Gui Network)

# Python Configuration
set(Python3_ROOT_DIR "C:/Python312")
find_package(Python3 REQUIRED COMPONENTS Interpreter Development NumPy)

# Source files
set(SOURCES
    src/main.cpp
    src/corneawidget.cpp
    src/corneaconfig.cpp
    src/pythonbridge.cpp
    src/corneacontroller.cpp
    src/imageloader.cpp
    src/devicecontrolpanel.cpp
    src/tcpserver.cpp
)

set(HEADERS
    src/corneawidget.h
    src/corneaconfig.h
    src/pythonbridge.h
    src/corneacontroller.h
    src/imageloader.h
    src/devicecontrolpanel.h
    src/tcpserver.h
)

set(UI_FILES
    src/corneawidget.ui
)

set(RESOURCES
    resources/resources.qrc
)

# Create executable
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    ${UI_FILES}
    ${RESOURCES}
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Python3_INCLUDE_DIRS}
    ${Python3_NumPy_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Gui
    Qt5::Network
    ${Python3_LIBRARIES}
)

# Copy Python DLL to output directory (Windows)
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Python3_RUNTIME_LIBRARY_DIRS}/python312.dll"
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endif()

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Install target
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
