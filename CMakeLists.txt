cmake_minimum_required(VERSION 3.16...3.26)

# 基于 obs-plugintemplate 脚手架(与 DistroAV / obs-capcast 相同构建体系)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  # DistroAV 标准: 直接用 find_package(Qt6)(模板的 find_qt 宏会因新版 Qt6
  # 的 ALIAS 目标报 set_property 错误)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
endif()

# Windows: WASAPI COM (CLSID_MMDeviceEnumerator 等 GUID 本地定义 + uuid.lib 双保险;
# CoInitializeEx/CoTaskMemFree 在 ole32.lib)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE uuid.lib ole32.lib)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/plugin-main.hpp
    src/mirror-core.cpp
    src/mirror-core.hpp
    src/mirror-window.cpp
    src/mirror-window.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
