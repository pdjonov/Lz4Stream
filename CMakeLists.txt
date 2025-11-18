cmake_minimum_required(VERSION 3.26)

project(lz4_stream, VERSION 1.1 DESCRIPTION "LZ4 streaming decompression library")

option(LZ4STREAM_DEBUG_OPT "Turn on optimization even in Debug configurations" OFF)
option(LZ4STREAM_WERROR "Treat warnings as errors" OFF)
option(LZ4STREAM_TESTS_EXE "Build the test runner" ON)

file(REAL_PATH ${CMAKE_CURRENT_LIST_DIR}/src/c LZ4STREAM_SOURCE_DIR)

set(LZ4STREAM_SOURCE_FILES
	${LZ4STREAM_SOURCE_DIR}/lz4_stream.c)
set(LZ4STREAM_INCLUDE_DIR
	${LZ4STREAM_SOURCE_DIR}/include)

add_library(lz4_stream-static STATIC
	${LZ4STREAM_SOURCE_FILES})
target_include_directories(lz4_stream-static PUBLIC
	${LZ4STREAM_INCLUDE_DIR})
set_target_properties(lz4_stream-static PROPERTIES
	C_STANDARD 11
	OUTPUT_NAME "lz4stream-static-$<CONFIG>")
if(LZ4STREAM_WERROR)
	set_target_properties(lz4_stream-static PROPERTIES
		COMPILE_WARNING_AS_ERROR ON)
endif()
target_compile_options(lz4_stream-static PRIVATE
	$<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall -Wextra -Wpedantic>)

if (LZ4STREAM_DEBUG_OPT)
	message("lz4_stream: forcing -O3 in Debug")
	target_compile_options(lz4_stream PRIVATE $<$<CONFIG:Debug>:-O3>)
endif()

if (LZ4STREAM_TESTS_EXE)
	include(FetchContent)
	FetchContent_Declare(
		Catch2
		GIT_SHALLOW TRUE
		GIT_REPOSITORY https://github.com/catchorg/Catch2.git
		GIT_TAG v3.4.0)
	FetchContent_MakeAvailable(Catch2)

	FetchContent_Declare(
		lz4
		GIT_REPOSITORY https://github.com/lz4/lz4.git
		GIT_TAG v1.9.3
	)
	FetchContent_MakeAvailable(lz4)

	add_executable(lz4_stream-tests
		${LZ4STREAM_SOURCE_DIR}/lz4_stream-tests.cpp
		${lz4_SOURCE_DIR}/lib/lz4.c)
	target_include_directories(lz4_stream-tests PRIVATE
		${lz4_SOURCE_DIR}/lib)
	set_target_properties(lz4_stream-tests PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED TRUE
		OUTPUT_NAME lz4_stream-tests)
	target_link_libraries(lz4_stream-tests PRIVATE
		lz4_stream-static
		Catch2::Catch2WithMain)
endif()