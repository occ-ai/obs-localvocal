# build the CTranslate2 library from source https://github.com/OpenNMT/CTranslate2.git

include(ExternalProject)
include(FetchContent)

if(APPLE)

  FetchContent_Declare(
    ctranslate2_fetch
    URL https://github.com/occ-ai/obs-ai-ctranslate2-dep/releases/download/1.2.0/libctranslate2-macos-Release-1.2.0.tar.gz
    URL_HASH SHA256=9029F19B0F50E5EDC14473479EDF0A983F7D6FA00BE61DC1B01BF8AA7F1CDB1B)
  FetchContent_MakeAvailable(ctranslate2_fetch)

  add_library(ct2 INTERFACE)
  target_link_libraries(ct2 INTERFACE "-framework Accelerate" ${ctranslate2_fetch_SOURCE_DIR}/lib/libctranslate2.a
                                      ${ctranslate2_fetch_SOURCE_DIR}/lib/libcpu_features.a)
  set_target_properties(ct2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${ctranslate2_fetch_SOURCE_DIR}/include)
  target_compile_options(ct2 INTERFACE -Wno-shorten-64-to-32 -Wno-comma)

elseif(WIN32)

  # check ACCELERATION environment variable
  if(NOT DEFINED ACCELERATION)
    message(FATAL_ERROR "Please set ACCELERATION to either `cpu`, `hipblas`, or `cuda`")
  endif()

  if(${ACCELERATION} STREQUAL "cpu" OR ${ACCELERATION} STREQUAL "hipblas")
    FetchContent_Declare(
      ctranslate2_fetch
      URL https://github.com/occ-ai/obs-ai-ctranslate2-dep/releases/download/1.2.0/libctranslate2-windows-4.1.1-Release-cpu.zip
      URL_HASH SHA256=30ff8b2499b8d3b5a6c4d6f7f8ddbc89e745ff06e0050b645e3b7c9b369451a3)
  else()
    # add compile definitions for CUDA
    add_compile_definitions(POLYGLOT_WITH_CUDA)
    add_compile_definitions(POLYGLOT_CUDA_VERSION="12.2.0")

    FetchContent_Declare(
      ctranslate2_fetch
      URL https://github.com/occ-ai/obs-ai-ctranslate2-dep/releases/download/1.2.0/libctranslate2-windows-4.1.1-Release-cuda12.2.0.zip
      URL_HASH SHA256=131724d510f9f2829970953a1bc9e4e8fb7b4cbc8218e32270dcfe6172a51558)
  endif()

  FetchContent_MakeAvailable(ctranslate2_fetch)

  add_library(ct2 INTERFACE)
  target_link_libraries(ct2 INTERFACE ${ctranslate2_fetch_SOURCE_DIR}/lib/ctranslate2.lib)
  set_target_properties(ct2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${ctranslate2_fetch_SOURCE_DIR}/include)
  target_compile_options(ct2 INTERFACE /wd4267 /wd4244 /wd4305 /wd4996 /wd4099)

  file(GLOB CT2_DLLS ${ctranslate2_fetch_SOURCE_DIR}/bin/*.dll)
  install(FILES ${CT2_DLLS} DESTINATION "obs-plugins/64bit")
else()
  # build cpu_features from source
  set(CPU_FEATURES_VERSION "0.9.0")
  set(CPU_FEATURES_URL "https://github.com/google/cpu_features.git")
  set(CPU_FEATURES_CMAKE_ARGS -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF)
  ExternalProject_Add(
    cpu_features_build
    GIT_REPOSITORY ${CPU_FEATURES_URL}
    GIT_TAG v${CPU_FEATURES_VERSION}
    GIT_PROGRESS 1
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
    CMAKE_GENERATOR ${CMAKE_GENERATOR}
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cpu_features${CMAKE_STATIC_LIBRARY_SUFFIX}
    CMAKE_ARGS -DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM} -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${CPU_FEATURES_CMAKE_ARGS}
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON)
  ExternalProject_Get_Property(cpu_features_build INSTALL_DIR)

  add_library(cpu_features STATIC IMPORTED GLOBAL)
  add_dependencies(cpu_features cpu_features_build)
  set_target_properties(
    cpu_features PROPERTIES IMPORTED_LOCATION
                            ${INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cpu_features${CMAKE_STATIC_LIBRARY_SUFFIX})
  set_target_properties(cpu_features PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_DIR}/include)

  # build CTranslate2 from source
  set(CT2_VERSION "4.1.1")
  set(CT2_URL "https://github.com/OpenNMT/CTranslate2.git")
  set(CT2_OPENBLAS_CMAKE_ARGS -DWITH_OPENBLAS=OFF)

  set(CT2_CMAKE_PLATFORM_OPTIONS -DBUILD_SHARED_LIBS=OFF -DOPENMP_RUNTIME=NONE -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
  set(CT2_LIB_INSTALL_LOCATION lib/${CMAKE_SHARED_LIBRARY_PREFIX}ctranslate2${CMAKE_STATIC_LIBRARY_SUFFIX})

  ExternalProject_Add(
    ct2_build
    GIT_REPOSITORY ${CT2_URL}
    GIT_TAG v${CT2_VERSION}
    GIT_PROGRESS 1
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
    CMAKE_GENERATOR ${CMAKE_GENERATOR}
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
    BUILD_BYPRODUCTS <INSTALL_DIR>/${CT2_LIB_INSTALL_LOCATION}
    CMAKE_ARGS -DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
               -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
               -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
               -DWITH_CUDA=OFF
               -DWITH_MKL=OFF
               -DWITH_TESTS=OFF
               -DWITH_EXAMPLES=OFF
               -DWITH_TFLITE=OFF
               -DWITH_TRT=OFF
               -DWITH_PYTHON=OFF
               -DWITH_SERVER=OFF
               -DWITH_COVERAGE=OFF
               -DWITH_PROFILING=OFF
               -DBUILD_CLI=OFF
               ${CT2_OPENBLAS_CMAKE_ARGS}
               ${CT2_CMAKE_PLATFORM_OPTIONS}
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON)

  ExternalProject_Get_Property(ct2_build INSTALL_DIR)

  add_library(ct2::ct2 STATIC IMPORTED GLOBAL)
  add_dependencies(ct2::ct2 ct2_build cpu_features_build)
  set_target_properties(ct2::ct2 PROPERTIES IMPORTED_LOCATION ${INSTALL_DIR}/${CT2_LIB_INSTALL_LOCATION})
  set_target_properties(ct2::ct2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_DIR}/include)

  add_library(ct2 INTERFACE)
  target_link_libraries(ct2 INTERFACE ct2::ct2 cpu_features)

endif()
