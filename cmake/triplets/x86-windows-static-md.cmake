set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# The pinned curl port exposes SSPI but leaves NTLM disabled without this option.
if(PORT STREQUAL "curl")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCURL_ENABLE_NTLM=ON")
endif()

# Match the static protobuf package instead of importing protobuf DLL symbols.
if(PORT STREQUAL "gamenetworkingsockets")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DProtobuf_USE_STATIC_LIBS=ON")
endif()
