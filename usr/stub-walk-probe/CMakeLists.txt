# /stub-walk-probe — drives t_attach_9p + t_walk_open + t_read against
# the stratumd-stub synthetic FS. P5-stratumd-stub-bringup-e1.
#
# Where /stub-fs-probe used a raw-9P client over pipes to test the stub's
# wire handlers, /stub-walk-probe goes through the kernel 9P client +
# dev9p Dev vtable + the new SYS_WALK_OPEN syscall to test the walk-
# through-mount primitive end-to-end. Used by kernel/test/test_stratumd_
# stub.c's stub_walk_round_trip test case.

add_executable(stub-walk-probe stub-walk-probe.c)
target_link_libraries(stub-walk-probe PRIVATE t)

set_target_properties(stub-walk-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME stub-walk-probe
)
