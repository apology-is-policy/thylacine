# /loom-bridge-probe -- the EL0 leg of the private Loom syscall bridge
# (docs/ASYNC-SERVICE-ABI.md, "The EL0 probe"). Ships in every image; the
# kernel test loom.bridge_probe spawns it and fixes the expected verdict by
# the kernel's own qualification lever.

add_executable(loom-bridge-probe loom-bridge-probe.c)
target_link_libraries(loom-bridge-probe PRIVATE t)

set_target_properties(loom-bridge-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME loom-bridge-probe
)
