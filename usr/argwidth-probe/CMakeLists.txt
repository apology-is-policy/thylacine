# /argwidth-probe -- the EL0 leg of ERRORS.md principle 6 (an argument that
# does not fit its declared width is refused, never truncated). Ships in every
# image; the kernel test syscall.argwidth_probe spawns it.

add_executable(argwidth-probe argwidth-probe.c)
target_link_libraries(argwidth-probe PRIVATE t)

set_target_properties(argwidth-probe PROPERTIES
    SUFFIX ""
    OUTPUT_NAME argwidth-probe
)
