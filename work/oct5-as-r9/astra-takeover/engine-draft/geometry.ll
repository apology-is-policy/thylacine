; ModuleID = 'work/oct5-as-r9/astra-takeover/engine-draft/geometry.c'
source_filename = "work/oct5-as-r9/astra-takeover/engine-draft/geometry.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-none-elf"

@astra_loom_size = dso_local global [4480 x i8] zeroinitializer, align 1
@astra_index_size = dso_local global [1544 x i8] zeroinitializer, align 1
@astra_protocol_size = dso_local global [42264 x i8] zeroinitializer, align 1

!llvm.module.flags = !{!0, !1}
!llvm.ident = !{!2}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"frame-pointer", i32 4}
!2 = !{!"Homebrew clang version 22.1.4"}
