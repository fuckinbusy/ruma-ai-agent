# Third-party libraries as DLLs with the static MSVC runtime inside (docs/02-LIBRARIES.md):
# the product links the runtime statically, so dependency DLLs must not import vcruntime140.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# glad: generate the core profile (the port defaults to compatibility). glad itself is
# static-only (vcpkg_check_linkage), which is fine: it is a small loader linked into the exe.
set(GLAD_PROFILE "core")
