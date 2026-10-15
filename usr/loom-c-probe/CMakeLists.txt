# /loom-c-probe -- the C native Loom adapter driven from EL0 against
# /loom-fixture (docs/ASYNC-SERVICE-ABI.md, NA-4d). Ships in every image; the
# kernel test loom.native_probe_c spawns both and fixes the expected verdicts
# by the kernel's own qualification lever.

add_executable(loom-c-probe loom-c-probe.c)
target_link_libraries(loom-c-probe PRIVATE t)

set_target_properties(loom-c-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME loom-c-probe
)
