cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

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
  find_package(Qt6 COMPONENTS Widgets Core)
  if(TARGET WrapOpenGL::WrapOpenGL)
    # Qt's OBS dependency bundle still advertises the removed macOS AGL framework
    # through WrapOpenGL. Xcode 26 no longer ships AGL, while QtGui already links
    # OpenGL directly, so keep the stale compatibility target empty.
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "")
  endif()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/camera-tally-filter.cpp src/plugin-main.cpp src/rec-trigger-dock.cpp
)

target_add_resource(${CMAKE_PROJECT_NAME} data/kofi_logo.png)

if(APPLE)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/macos-ocr.mm)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework Foundation" "-framework CoreGraphics" "-framework Vision")
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/macos-ocr-stub.cpp)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(APPLE)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/data/kofi_logo.png"
      "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Resources/kofi_logo.png"
    COMMENT "Copy Ko-fi logo into plugin resources"
    VERBATIM
  )
else()
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/data/kofi_logo.png"
      "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/kofi_logo.png"
    COMMENT "Copy Ko-fi logo into plugin resources"
    VERBATIM
  )
endif()
