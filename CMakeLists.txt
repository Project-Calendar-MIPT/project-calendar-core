cmake_minimum_required(VERSION 3.10)
project(project-calendar)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# Find required packages
find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(BCRYPT_LIBRARY NAMES bcrypt PATHS /usr/local/lib /opt/homebrew/lib)

# Collect model source files (.cpp instead of .cc)
file(GLOB MODEL_SOURCES
    ${CMAKE_SOURCE_DIR}/src/models/*.cpp
)

# Collect API source files
file(GLOB API_SOURCES
    ${CMAKE_SOURCE_DIR}/src/API/*.cpp
)

# Main source
set(MAIN_SOURCE ${CMAKE_SOURCE_DIR}/src/main.cpp)

# Create library for models
add_library(models_lib STATIC ${MODEL_SOURCES})
target_link_libraries(models_lib PUBLIC Drogon::Drogon)
target_include_directories(models_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Main executable
add_executable(${PROJECT_NAME} 
    ${MAIN_SOURCE}
    ${API_SOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    Drogon::Drogon
    models_lib
    OpenSSL::Crypto
)

if(BCRYPT_LIBRARY)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${BCRYPT_LIBRARY})
else()
    message(WARNING "bcrypt library not found; local build may fail outside Docker")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
