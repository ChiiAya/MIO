cmake_minimum_required(VERSION 3.20)
project(MIORegressionTests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
find_package(Threads REQUIRED)
find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
set(MIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(MIO_SRC "${MIO_ROOT}/src")
set(COMMON_SOURCES
    ${MIO_SRC}/core/message/Message.cpp
    ${MIO_SRC}/core/conversation/Conversation.cpp
    ${MIO_SRC}/log/Log.cpp)
set(CONTEXT_SOURCES
    ${MIO_SRC}/context/achieve/Achieve.cpp
    ${MIO_SRC}/context/conversationFusion/FusionContext.cpp
    ${MIO_SRC}/context/contextBuilder/ContextBuilder.cpp
    ${MIO_SRC}/context/summarizor/SummaryManager.cpp)
add_library(regression_core STATIC ${COMMON_SOURCES} ${CONTEXT_SOURCES}
    ${MIO_SRC}/providers/llm/tool/ToolLoop.cpp
    ${MIO_SRC}/providers/llm/tool/ToolRegistry.cpp
    ${MIO_SRC}/mind/graph/RelationshipGraph.cpp
    ${MIO_SRC}/context/conversationFusion/FusionRouter.cpp
    ${MIO_SRC}/adapters/napcat/OneBotParser.cpp
    ${MIO_SRC}/runtime/ContactTools.cpp
    ${MIO_SRC}/runtime/MessageTools.cpp
    ${MIO_SRC}/mind/facts/Facts.cpp
    ${MIO_SRC}/context/inputBuffer/inputBuffer.cpp)
target_include_directories(regression_core PUBLIC ${MIO_SRC} ${NLOHMANN_INCLUDE_DIR})
target_link_libraries(regression_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(regression_core PUBLIC /utf-8)
else()
    target_compile_options(regression_core PRIVATE -Wall -Wextra)
endif()
foreach(test lifecycle contacts mentions delivery)
    add_executable(${test}_test ${test}_test.cpp)
    target_link_libraries(${test}_test PRIVATE regression_core)
    if(MSVC)
        target_compile_options(${test}_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(${test}_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${test} COMMAND ${test}_test)
endforeach()

# Optional full compilation and Runtime integration tests, using existing app
# dependencies. Sources can be supplied offline through FETCHCONTENT_SOURCE_DIR_*.
option(MIO_BUILD_APP "Build the complete application and Runtime integration tests" OFF)
if(MIO_BUILD_APP)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG da40186618909b1a7363d4e4495aa899c6e0eb75)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(USE_TLS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ixwebsocket GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG 64fae7676bd8fe31f7cb4bcde7a6841892dad65e)
    FetchContent_MakeAvailable(cpr ixwebsocket)
    find_package(SQLite3 REQUIRED)
    file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${MIO_SRC}/*.cpp")
    list(REMOVE_ITEM APP_SOURCES "${MIO_SRC}/main.cpp")
    add_library(mio_app STATIC ${APP_SOURCES})
    target_include_directories(mio_app PUBLIC ${MIO_SRC} ${NLOHMANN_INCLUDE_DIR})
    target_link_libraries(mio_app PUBLIC cpr::cpr ixwebsocket SQLite::SQLite3 Threads::Threads)
    if(MSVC)
        target_compile_options(mio_app PUBLIC /utf-8)
    endif()
    add_executable(MIO ${MIO_SRC}/main.cpp)
    target_link_libraries(MIO PRIVATE mio_app)
    add_executable(runtime_test runtime_test.cpp)
    target_link_libraries(runtime_test PRIVATE mio_app)
    add_test(NAME runtime COMMAND runtime_test)
    add_executable(transport_test transport_test.cpp)
    target_link_libraries(transport_test PRIVATE mio_app)
    add_test(NAME transport COMMAND transport_test)
    set_tests_properties(runtime transport PROPERTIES TIMEOUT 30)
endif()
