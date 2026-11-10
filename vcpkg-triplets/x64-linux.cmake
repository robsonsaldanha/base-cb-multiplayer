# x64-linux do vcpkg (estático, CRT dinâmico), com UMA exceção: libmariadb (MariaDB Connector/C, LGPL-2.1) vem como
# biblioteca compartilhada (libmariadb.so), para o cb-server ligar com ela dinamicamente, como no Windows (x64-windows
# já é todo dinâmico). Ativado por VCPKG_OVERLAY_TRIPLETS no preset base (CMakePresets.json). Ver vcpkg-triplets/README.md.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(PORT STREQUAL "libmariadb")
  set(VCPKG_LIBRARY_LINKAGE dynamic)
  set(VCPKG_FIXUP_ELF_RPATH ON)
endif()
