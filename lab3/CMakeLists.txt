cmake_minimum_required(VERSION 3.15)
project(LocationFinder)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Находим зависимости
find_package(cpr REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# Создаем исполняемый файл
add_executable(LocationFinder
    src/main.cpp
    src/ApiClient.cpp
    src/Config.cpp
)

# Подключаем заголовочные файлы
target_include_directories(LocationFinder PRIVATE include)

# Линкуем зависимости
target_link_libraries(LocationFinder PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
    Threads::Threads
)

# Опции для отладки
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(LocationFinder PRIVATE DEBUG_MODE)
endif()