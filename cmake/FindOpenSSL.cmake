if(WIN32)
  set(OPENSSL_ROOT_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows"
      CACHE STRING "Path to OpenSSL")
elseif(APPLE)
  include(FetchContent)

  FetchContent_Declare(
    openssl-macos-fetch
    URL "https://github.com/occ-ai/occ-ai-dep-openssl/releases/download/0.0.1/openssl-3.3.1-macos.tar.gz"
    URL_HASH SHA256=d578921b7168e21451f0b6e4ac4cb989c17abc6829c8c43f32136c1c2544ffde)

  FetchContent_MakeAvailable(openssl-macos-fetch)

  set(OPENSSL_ROOT_DIR
      "${openssl-macos-fetch_SOURCE_DIR}"
      CACHE STRING "Path to OpenSSL")
endif()

find_package(OpenSSL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OpenSSL::SSL)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OpenSSL::Crypto)

# copy the openssl dlls to the release directory
if(WIN32)
  set(OpenSS_LIB_NAMES "libcrypto-3-x64" "libssl-3-x64")
  foreach(lib_name IN LISTS OpenSS_LIB_NAMES)
    install(FILES ${OPENSSL_ROOT_DIR}/bin/${lib_name}.dll DESTINATION "obs-plugins/64bit")
  endforeach()
endif()
