cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
project(research_phys LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# URL + hash (not git): avoids git-populate rm -rf of json-src on NFS.
# Explicit SOURCE_DIR avoids collisions with a stuck legacy build/_deps/json-src.
set(_nlohmann_json_src "${CMAKE_BINARY_DIR}/fc-nlohmann-json-src")
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
  URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
  SOURCE_DIR "${_nlohmann_json_src}"
)
FetchContent_MakeAvailable(json)

add_executable(phys_load
  src/chip.cpp
  src/chip_loader.cpp
  src/pdn_extract.cpp
  src/pdn_model.cpp
  src/pdn_tcl_physical.cpp
  src/pdn_ir.cpp
  src/main.cpp
)
target_include_directories(phys_load PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(phys_load PRIVATE nlohmann_json::nlohmann_json)

