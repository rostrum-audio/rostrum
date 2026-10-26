# AppImage runtime

`runtime-x86_64.gz` contains the unmodified x86_64 runtime from AppImage/type2-runtime
commit [`8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa`](https://github.com/AppImage/type2-runtime/tree/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa).
It was recovered from [upstream build 36463736478](https://github.com/AppImage/type2-runtime/actions/runs/36463736478),
artifact `artifacts x86_64` (ID `10989035536`). The uncompressed SHA-256 is
`156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074`,
the same checksum used for published Rostrum v0.1.0. Gzip compression omits the timestamp.

Packaging unpacks this file into its tools directory and checks the checksum in
[`sources.json`](../licenses/runtime/sources.json). It does not download the moving
upstream `continuous` asset. The packaged launcher must also pass the existing ELF
build-ID and code-hash checks.

Upstream licences and component notices are retained in
[`../licenses/runtime/`](../licenses/runtime/). The runtime contains statically linked
libraries; their exact versions, source locations and rebuild instructions are
documented in [AppImage dependencies](../../../docs/appimage-dependencies.md).
Keep the binary, corresponding source information and notices together when updating
this copy. Do not update just the checksum to accept a different upstream runtime.
