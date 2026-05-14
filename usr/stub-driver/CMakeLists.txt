# /stub-driver — production-shape orchestrator for the stratumd-stub
# round-trip; P5-stratumd-stub-bringup-b.
#
# Drives the full pipeline from userspace: pipe + spawn-with-fds +
# attach + mount + unmount + close + wait. No kernel-supervised handle
# pre-installation.

add_executable(stub-driver stub-driver.c)
target_link_libraries(stub-driver PRIVATE t)

set_target_properties(stub-driver PROPERTIES
    SUFFIX ""
    OUTPUT_NAME stub-driver
)
