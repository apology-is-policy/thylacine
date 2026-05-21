# /stub-fs-probe — raw 9P client driving Twalk + Tlopen + Tread against
# the stratumd-stub synthetic FS. P5-stratumd-stub-bringup-d.
#
# Mirror-image of /stratumd-stub (a 9P SERVER): this is a 9P CLIENT
# wired to the same byte-pipe transport. Used by
# kernel/test/test_stratumd_stub.c's stub_fs_round_trip test case.

add_executable(stub-fs-probe stub-fs-probe.c)
target_link_libraries(stub-fs-probe PRIVATE t)

set_target_properties(stub-fs-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME stub-fs-probe
)
