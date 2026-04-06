cmake_minimum_required(VERSION 3.16)
project(research_phys LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

add_executable(phys_load
  src/load.cpp
  src/main.cpp
)
target_include_directories(phys_load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(phys_load PRIVATE nlohmann_json::nlohmann_json)
