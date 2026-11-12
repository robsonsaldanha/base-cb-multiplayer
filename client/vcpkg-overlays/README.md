# client/vcpkg-overlays

Ports locais só do build do mod (`tools/build.ps1 -Target mod` passa `VCPKG_OVERLAY_PORTS` para esta pasta). O build
da raiz usa `vcpkg-overlays/` na raiz e não vê estes.

## gamenetworkingsockets (1.6.1)

Cópia do port oficial (árvore `222bf41b02632da7559a9a9620fe2e8f1721adc3` do microsoft/vcpkg, a da baseline
`ec48c11e…`) com uma opção a mais: `-DProtobuf_USE_STATIC_LIBS=${BUILD_STATIC_LIB}`. O CMake do GNS 1.6.1 tem essa
opção desligada por padrão e, com MSVC, define `PROTOBUF_USE_DLLS` quando ela está desligada; no triplet
`x64-windows-static-md` (protobuf estático) o `GameNetworkingSockets_s.lib` saía pedindo `__imp_…` do protobuf e o
`cb-mp.dll` não ligava. `port-version` 100 só marca que é o port local.
