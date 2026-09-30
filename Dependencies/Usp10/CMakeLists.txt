set(USP10_SRC
    Usp10Loader.cpp
    Usp10Loader.h
    usp10_adapter.h
    usp10_subset.h
)

add_library(deps_usp10 STATIC)

target_sources(deps_usp10 PRIVATE ${USP10_SRC})

target_include_directories(deps_usp10 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# For WWLib/stringex.h, which includes bittype.h from the same directory.
target_include_directories(deps_usp10 PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWLib
)

target_link_libraries(deps_usp10 PRIVATE
    core_utility
)
