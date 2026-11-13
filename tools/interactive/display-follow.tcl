# tools/interactive/display-follow.tcl -- what the scenarios about a display
# that follows its host share (ls-display-follow.exp, ls-display-stall.exp,
# ls-display-follow-declared.exp, ls-display-follow-edid.exp,
# ls-graphical-sak-follow.exp, ls-graphical-sak-unmade.exp, ls-graphical-sak-blank.exp).
#
# Not a scenario: tools/test-interactive.sh runs every *.exp in this directory,
# and this is not one. tools/check-expect-forms.py reads it with them. Sourced
# after lib.exp, with these globals set by the scenario: `repo` (the tree),
# `qmp` (the monitor's socket) and `ppm` (where a dump goes).

# The scanout's geometry from the PPM header ("P6\nW H\n255"), how many
# colours a sample of the picture has (a screen with nothing drawn on it has
# the right geometry too), and a digest of all of it. The stale-file guard is
# load-bearing: a dump that failed must never be answered from the one before.
# Why a dump gave nothing is kept for the failure that follows it. The length
# is checked against the header: tools/screendump.sh hands out a true P6 (QEMU
# pads each row to four bytes, which a width that is no multiple of four shows).
set dims_why ""
proc dims {} {
    global repo qmp ppm
    set ::dims_why ""
    file delete -force $ppm
    if {[catch {exec $repo/tools/screendump.sh -s $qmp -P $ppm} out]} {
        set ::dims_why "the dump failed: [lindex [split [string trim $out] "\n"] end]"
        return ""
    }
    if {![file exists $ppm]} { set ::dims_why "the dump wrote no file"; return "" }
    if {[catch {exec python3 -I -c {
import hashlib, re, sys
d = open(sys.argv[1], 'rb').read()
m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', d)
w, h = int(m.group(1)), int(m.group(2))
px = d[m.end():]
assert len(px) == w * h * 3, (w, h, len(px))
print(w, h, len({px[i:i + 3] for i in range(0, len(px), 3 * 101)}), hashlib.md5(px).hexdigest())
} $ppm} out]} {
        set ::dims_why "the dump could not be read: [lindex [split [string trim $out] "\n"] end]"
        return ""
    }
    return $out
}
proc wait_dims {want label} {
    set d "(no dump)"
    for {set i 0} {$i < 20} {incr i} {
        set d [dims]
        if {[lrange $d 0 1] eq $want && [lindex $d 2] > 1} {
            lc_step "scanout $want, [lindex $d 2] colours in the sample ($label)"
            return
        }
        after 700
    }
    lc_fail "the scanout never became $want with a picture on it ($label; last: width height colours = $d; $::dims_why)"
}
# Tell QEMU a size as a VNC viewer that resized its window does: the path a
# host window takes, without a window.
proc tell {args} {
    global repo
    if {[catch {exec python3 -I $repo/tools/interactive/rfb_resize.py $::env(THYLACINE_VNC_SOCKET) {*}$args} out]} {
        lc_fail "rfb_resize.py $args: $out"
    }
    lc_step "told: [string map [list "\n" "; "] $out]"
}
# The console's size in cells, read in the shell. The renderer reports it after
# it has laid itself out for the mode, which is after the compositor's line, so
# the question is put again until the answer is the one wanted. The newline
# after the rows keeps a number that is still arriving from being read short.
proc wait_cells {cols rows label} {
    set ::what "reading the console's size ($label)"
    set got "(nothing read)"
    for {set i 0} {$i < 8} {incr i} {
        lc_send "cat /dev/winsize"
        lc_within 10 {
            -re {winsize (\d+) (\d+)[\r\n]} { set got "$expect_out(1,string) $expect_out(2,string)" }
            timeout { set got "(no answer in 10s)" }
            eof     { lc_fail "qemu exited while reading the console's size ($label)" }
        }
        if {$got eq "$cols $rows"} {
            lc_step "the console is $cols by $rows cells ($label)"
            return
        }
        after 700
    }
    lc_fail "the console never became $cols by $rows cells ($label; last: $got)"
}
# The picture on the scanout changes when something is typed at this mode. A
# display whose device scans a screen that nobody composes into has the right
# size and a picture on it, and never another. The command's own output is
# waited for first: a line that must not be said, and was said by now, then
# fails here under this label and not in the next leg's first wait. One token
# a call, so that no wait is answered by the output of the one before.
set redraw_n 0
proc wait_redraw {label} {
    global timeout
    set ::what "the redraw ($label)"
    set before [lindex [dims] 3]
    if {$before eq ""} { lc_fail "no dump to tell a redraw by ($label; $::dims_why)" }
    set tok "display-redraw-[incr ::redraw_n]"
    lc_send "echo $tok"
    set saved $timeout
    set timeout 10
    expect \
        -re "\[\r\n\]$tok\[\r\n\]" { } \
        timeout { lc_fail "the shell did not answer '$tok' ($label)" } \
        eof     { lc_fail "qemu exited during the redraw ($label)" }
    set timeout $saved
    for {set i 0} {$i < 12} {incr i} {
        after 700
        set now [lindex [dims] 3]
        if {$now ne "" && $now ne $before} {
            lc_step "the scanout's picture changed after a command was typed ($label)"
            return
        }
    }
    lc_fail "the scanout's picture did not change after a command was typed ($label): the device shows a screen nothing is drawn into"
}
# Wait for a line of the compositor's. A follow that is put off because the
# device is stalled before it is asked is noted and waited out. One pattern
# that is either line, which matches the one said first: as two patterns the
# stalled line would be taken wherever it stood in what was read, and the
# line waited for, said before it, would go with it.
proc follow_expect {re what} {
    global timeout
    set ::what $what
    set stalled {tapestryd: display event \d+: the device is stalled; to be asked when it answers}
    while {1} {
        set got ""
        set timeout 30
        expect {
            -re "($stalled|$re)" {
                set got $expect_out(1,string)
            }
            timeout {
                lc_fail "timeout (30s) waiting for \[$re\] during '$what'"
            }
            eof {
                lc_fail "qemu exited during '$what'"
            }
        }
        if {![regexp -- $stalled $got]} {
            break
        }
        lc_step "note: the device was stalled when '$what' was due; waited for"
    }
    lc_step "saw: $what"
}
# The compositor's lines for a change: begun (said before it, with the state
# the scanout is in then), up (said after it), and followed (said last, once
# the bind of the screen and the scale are settled too). The scale is 100 at
# every mode here: a VNC viewer tells no millimetres.
proc begun {from to state} { return "tapestryd: mode $from -> $to \\(scanout $state\\)" }
proc up {mode} { return "tapestryd: mode $mode is up \\(scanout composed\\)" }
proc followed {mode} { return "tapestryd: display event \\d+: followed, $mode at 100%" }
