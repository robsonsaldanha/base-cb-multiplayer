# Painel admin (cbadmin): HTTP próprio + UI embutida (web/* → header gerado no build).
set(CBADMIN_WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(CBADMIN_WEB_FILES ${CBADMIN_WEB_DIR}/index.html ${CBADMIN_WEB_DIR}/app.js ${CBADMIN_WEB_DIR}/app.css)
set(CBADMIN_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(CBADMIN_GEN ${CBADMIN_GEN_DIR}/cbadmin_web.hpp)
add_custom_command(OUTPUT ${CBADMIN_GEN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CBADMIN_GEN_DIR}
  COMMAND ${CMAKE_COMMAND} -DWEB_DIR=${CBADMIN_WEB_DIR} -DOUT=${CBADMIN_GEN} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS ${CBADMIN_WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  COMMENT "Embutindo a UI do painel admin"
  VERBATIM)

add_library(cbadmin STATIC src/admin_server.cpp ${CBADMIN_GEN})
target_include_directories(cbadmin PUBLIC include PRIVATE ${CBADMIN_GEN_DIR})
target_link_libraries(cbadmin PUBLIC cbserver_core cbserver_script)

add_executable(cbadmin_tests tests/test_admin.cpp)
target_include_directories(cbadmin_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
target_link_libraries(cbadmin_tests PRIVATE cbadmin Catch2::Catch2WithMain)
cbmp_lua_stack(cbadmin_tests)
target_compile_definitions(cbadmin_tests PRIVATE CBMP_EXAMPLE_RESOURCES="${CMAKE_SOURCE_DIR}/resources.example")
catch_discover_tests(cbadmin_tests PROPERTIES TIMEOUT 60)
