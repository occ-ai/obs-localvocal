cmake_minimum_required(VERSION 3.16...3.26)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_TESTS "Enable tests" OFF)

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
  find_qt(COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                  -Wno-comma>)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
endif()

set(USE_SYSTEM_CURL
    OFF
    CACHE STRING "Use system cURL")

if(USE_SYSTEM_CURL)
  find_package(CURL REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${CURL_LIBRARIES}")
  target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PUBLIC "${CURL_INCLUDE_DIRS}")
else()
  include(cmake/BuildMyCurl.cmake)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE libcurl)
endif()

include(cmake/BuildWhispercpp.cmake)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Whispercpp)

include(cmake/BuildCTranslate2.cmake)
include(cmake/BuildSentencepiece.cmake)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ct2 sentencepiece)

include(cmake/FindOpenSSL.cmake)

set(USE_SYSTEM_ONNXRUNTIME
    OFF
    CACHE STRING "Use system ONNX Runtime")

set(DISABLE_ONNXRUNTIME_GPU
    OFF
    CACHE STRING "Disables GPU support of ONNX Runtime (Only valid on Linux)")

if(DISABLE_ONNXRUNTIME_GPU)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE DISABLE_ONNXRUNTIME_GPU)
endif()

if(USE_SYSTEM_ONNXRUNTIME)
  if(OS_LINUX)
    find_package(Onnxruntime 1.16.3 REQUIRED)
    set(Onnxruntime_INCLUDE_PATH
        ${Onnxruntime_INCLUDE_DIR} ${Onnxruntime_INCLUDE_DIR}/onnxruntime
        ${Onnxruntime_INCLUDE_DIR}/onnxruntime/core/session ${Onnxruntime_INCLUDE_DIR}/onnxruntime/core/providers/cpu)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${Onnxruntime_LIBRARIES}")
    target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PUBLIC "${Onnxruntime_INCLUDE_PATH}")
  else()
    message(FATAL_ERROR "System ONNX Runtime is only supported on Linux!")
  endif()
else()
  include(cmake/FetchOnnxruntime.cmake)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.c
          src/transcription-filter.cpp
          src/transcription-filter.c
          src/transcription-filter-callbacks.cpp
          src/transcription-filter-properties.cpp
          src/transcription-filter-utils.cpp
          src/transcription-utils.cpp
          src/model-utils/model-downloader.cpp
          src/model-utils/model-downloader-ui.cpp
          src/model-utils/model-infos.cpp
          src/model-utils/model-find-utils.cpp
          src/whisper-utils/whisper-processing.cpp
          src/whisper-utils/whisper-utils.cpp
          src/whisper-utils/whisper-model-utils.cpp
          src/whisper-utils/silero-vad-onnx.cpp
          src/whisper-utils/token-buffer-thread.cpp
          src/timed-metadata/timed-metadata-utils.cpp
          src/translation/language_codes.cpp
          src/translation/translation.cpp
          src/translation/translation-utils.cpp
          src/ui/filter-replace-dialog.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(ENABLE_TESTS)
  add_executable(${CMAKE_PROJECT_NAME}-tests)

  include(cmake/FindLibAvObs.cmake)

  target_sources(
    ${CMAKE_PROJECT_NAME}-tests
    PRIVATE src/tests/localvocal-offline-test.cpp
            src/tests/audio-file-utils.cpp
            src/transcription-utils.cpp
            src/model-utils/model-infos.cpp
            src/model-utils/model-find-utils.cpp
            src/whisper-utils/whisper-processing.cpp
            src/whisper-utils/whisper-utils.cpp
            src/whisper-utils/silero-vad-onnx.cpp
            src/whisper-utils/token-buffer-thread.cpp
            src/translation/language_codes.cpp
            src/translation/translation.cpp)

  find_libav(${CMAKE_PROJECT_NAME}-tests)

  target_link_libraries(${CMAKE_PROJECT_NAME}-tests PRIVATE ct2 sentencepiece Whispercpp Ort OBS::libobs)
  target_include_directories(${CMAKE_PROJECT_NAME}-tests PRIVATE src)

  # install the tests to the release/test directory
  install(TARGETS ${CMAKE_PROJECT_NAME}-tests DESTINATION test)
endif()
