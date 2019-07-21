if (NOT APPLE)
    option (USE_INTERNAL_LIBCXX_LIBRARY "Set to FALSE to use system libcxx and libcxxabi libraries instead of bundled" ${NOT_UNBUNDLED})
endif ()

if (USE_INTERNAL_LIBCXX_LIBRARY AND NOT EXISTS "${ClickHouse_SOURCE_DIR}/contrib/libcxx/include/vector")
    message (WARNING "submodule contrib/libcxx is missing. to fix try run: \n git submodule update --init --recursive")
    set (USE_INTERNAL_LIBCXX_LIBRARY 0)
endif ()

if (USE_INTERNAL_LIBCXX_LIBRARY AND NOT EXISTS "${ClickHouse_SOURCE_DIR}/contrib/libcxxabi/src")
    message (WARNING "submodule contrib/libcxxabi is missing. to fix try run: \n git submodule update --init --recursive")
    set (USE_INTERNAL_LIBCXXABI_LIBRARY 0)
endif ()

if (NOT USE_INTERNAL_LIBCXX_LIBRARY)
    find_library (LIBCXX_LIBRARY c++)
    find_library (LIBCXXABI_LIBRARY c++abi)
else ()
    set (LIBCXX_INCLUDE_DIR ${ClickHouse_SOURCE_DIR}/contrib/libcxx/include)
    set (LIBCXXABI_INCLUDE_DIR ${ClickHouse_SOURCE_DIR}/contrib/libcxxabi/include)
    set (LIBCXX_LIBRARY cxx_static)
    set (LIBCXXABI_LIBRARY cxxabi_static)
endif ()

message (STATUS "Using libcxx: ${LIBCXX_LIBRARY}")
message (STATUS "Using libcxxabi: ${LIBCXXABI_LIBRARY}")