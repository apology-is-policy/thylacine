; ModuleID = '/Users/northkillpd/projects/thylacine-corona/work/oct5-as-r9/astra-takeover/close-draft/geometry.c'
source_filename = "/Users/northkillpd/projects/thylacine-corona/work/oct5-as-r9/astra-takeover/close-draft/geometry.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64-unknown-none-elf"

@loom_bytes = dso_local local_unnamed_addr global i64 2968, align 8, !dbg !0
@engine_bytes = dso_local local_unnamed_addr global i64 1544, align 8, !dbg !5

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!9, !10, !11, !12, !13, !14, !15, !16, !17, !18}
!llvm.ident = !{!19}
!llvm.errno.tbaa = !{!20}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "loom_bytes", scope: !2, file: !7, line: 3, type: !8, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C99, file: !3, producer: "Homebrew clang version 22.1.4", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: None)
!3 = !DIFile(filename: "/Users/northkillpd/projects/thylacine-corona/work/oct5-as-r9/astra-takeover/close-draft/geometry.c", directory: "/Users/northkillpd/projects/thylacine-corona", checksumkind: CSK_MD5, checksum: "c1ec779d70fa1da488572f291f958ce2")
!4 = !{!0, !5}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "engine_bytes", scope: !2, file: !7, line: 4, type: !8, isLocal: false, isDefinition: true)
!7 = !DIFile(filename: "work/oct5-as-r9/astra-takeover/close-draft/geometry.c", directory: "/Users/northkillpd/projects/thylacine-corona", checksumkind: CSK_MD5, checksum: "c1ec779d70fa1da488572f291f958ce2")
!8 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!9 = !{i32 7, !"Dwarf Version", i32 5}
!10 = !{i32 2, !"Debug Info Version", i32 3}
!11 = !{i32 1, !"wchar_size", i32 4}
!12 = !{i32 8, !"branch-target-enforcement", i32 2}
!13 = !{i32 8, !"sign-return-address", i32 2}
!14 = !{i32 8, !"PIC Level", i32 1}
!15 = !{i32 7, !"PIE Level", i32 1}
!16 = !{i32 1, !"Code Model", i32 1}
!17 = !{i32 7, !"direct-access-external-data", i32 1}
!18 = !{i32 7, !"frame-pointer", i32 4}
!19 = !{!"Homebrew clang version 22.1.4"}
!20 = !{!21, !21, i64 0}
!21 = !{!"int", !22, i64 0}
!22 = !{!"omnipotent char", !23, i64 0}
!23 = !{!"Simple C/C++ TBAA"}
