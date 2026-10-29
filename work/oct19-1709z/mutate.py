#!/usr/bin/env python3
# Sabotage edits for the 17:09Z ctl-identity witnesses. Each mutant is a list of
# exact (file, old, new) replacements; every `old` must occur EXACTLY once in the
# committed tree or the mutant refuses (a stale anchor must never apply nothing).
# Usage: python3 -I work/oct19-1709z/mutate.py <root> <id>   |   --list   |   --check <root>
import sys

SC = "kernel/syscall.c"
VC = "kernel/vivarium.c"

MUTANTS = {
    # The pin is trusted without the identity check.
    "M1": ("vivsock.ctl_verbs_need_their_ctl", [
        (SC, "    if (s && viv_socktab_ctl_is(tab, (s32)(s64)fd_raw, e->epoch, s)) return s;",
             "    if (s) return s;"),
    ]),
    # connect's dial pins by number again.
    "M2a": ("vivsock.ctl_verbs_need_their_ctl", [
        (SC, "    struct Spoor *ctl = viv_sock_pin_ctl(p, tab, fd_raw, e, want, &connected, &err);",
             "    struct Spoor *ctl = sys_lookup_rw_handle(p, (hidx_t)fd_raw, want);\n"
             "    connected = false;\n"
             "    err = -(s64)T_E_BADF;"),
    ]),
    # listen's announce pins by number again.
    "M2b": ("vivsock.ctl_verbs_need_their_ctl", [
        (SC, "    struct Spoor *ctl = viv_sock_pin_ctl(p, tab, fd_raw, &e, RIGHT_WRITE, &connected, &perr);",
             "    struct Spoor *ctl = sys_lookup_rw_handle(p, (hidx_t)fd_raw, RIGHT_WRITE);\n"
             "    connected = false;\n"
             "    perr = -(s64)T_E_BADF;"),
    ]),
    # the datagram sendto's re-dial pins by number again.
    "M2c": ("vivsock.ctl_verbs_need_their_ctl", [
        (SC, "    struct Spoor *ctl = viv_sock_pin_ctl(p, tab, fd_raw, e, RIGHT_WRITE, &connected, &rc);",
             "    struct Spoor *ctl = sys_lookup_rw_handle(p, (hidx_t)fd_raw, RIGHT_WRITE);\n"
             "    connected = false;\n"
             "    rc = -(s64)T_E_BADF;"),
    ]),
    # fork's snapshot drops the identity (treats it as a cache, like ready[]).
    "M3": ("vivsock.ctl_identity_travels_with_the_fd", [
        (VC, "        if (ctl) spoor_ref(ctl);\n        f->dst->ctl[i] = ctl;",
             "        (void)ctl;\n        f->dst->ctl[i] = NULL;"),
    ]),
    # dup's alias drops the identity.
    "M4": ("vivsock.ctl_identity_travels_with_the_fd", [
        (VC, "            if (ctl) spoor_ref(ctl);\n            tab->ctl[i] = ctl;",
             "            (void)ctl;\n            tab->ctl[i] = NULL;"),
    ]),
    # connect's commit keeps the row's ctl (one extra netd fid per connection).
    "M5": ("vivsock.ctl_verbs_need_their_ctl", [
        (VC, "        c->detached    = c->tab->ctl[e - c->tab->s];\n        c->tab->ctl[e - c->tab->s] = NULL;",
             "        c->detached    = NULL;"),
    ]),
    # a refused swap is EIO even when a peer finished the same connect.
    "M7": ("vivsock.connect_finished_by_a_peer", [
        (SC, "        struct viv_sock now;\n"
             "        if (viv_socktab_get(tab, (s32)(s64)fd_raw, &now) && now.epoch == e->epoch &&\n"
             "            now.state == VIV_SOCK_CONNECTED)\n"
             "            return 0;\n"
             "        return -(s64)T_E_IO;",
             "        return -(s64)T_E_IO;"),
    ]),
    # the pin never answers as the connected socket.
    "M8": ("vivsock.ctl_verbs_meet_a_racing_peer", [
        (SC, "        now.state == VIV_SOCK_CONNECTED) {\n        *connected = true;\n        return NULL;\n    }",
             "        now.state == VIV_SOCK_CONNECTED) {\n        (void)0;\n    }"),
    ]),
    # the row keeps a BARE pointer: no reference taken, none released (the
    # ABA-prone design the reference rules out).
    "M9": ("vivsock.ctl_verbs_need_their_ctl vivsock.ctl_identity_travels_with_the_fd", [
        (VC, "            if (ctl) spoor_ref(ctl);     // the row's own; a FREE slot's was NULL\n",
             ""),
        (VC, "        if (ctl) spoor_ref(ctl);\n        f->dst->ctl[i] = ctl;",
             "        f->dst->ctl[i] = ctl;"),
        (VC, "            if (ctl) spoor_ref(ctl);\n            tab->ctl[i] = ctl;",
             "            tab->ctl[i] = ctl;"),
        (VC, "    spoor_clunk(d.ctl);\n", ""),
        (VC, "        spoor_clunk(tab->ctl[i]);\n", ""),
        (VC, "        c->detached    = c->tab->ctl[e - c->tab->s];\n",
             "        c->detached    = NULL;\n"),
    ]),
    # connect's commit keys on the snapshot's epoch alone (audit r1 F1): a row a
    # peer re-made with the same ctl identity stays CONNECTING over data.
    "M11": ("vivsock.connect_commits_a_realiased_row", [
        (VC, "    if (e && (e->epoch == c->expect_epoch ||\n"
             "              (c->ctl && c->tab->ctl[e - c->tab->s] == c->ctl))) {",
             "    if (e && e->epoch == c->expect_epoch) {"),
    ]),
    # the refusal never tells a closed number from a non-socket.
    "M10": ("vivsock.ctl_verbs_meet_a_racing_peer", [
        (SC, "    *err = (handle_get_cloexec(p, (hidx_t)(u32)fd_raw) < 0) ? -(s64)T_E_BADF\n"
             "                                                            : -(s64)T_E_NOTSOCK;",
             "    *err = -(s64)T_E_NOTSOCK;"),
    ]),
}


def plan(root, mid):
    _, edits = MUTANTS[mid]
    texts = {}
    for path, old, new in edits:
        full = root + "/" + path
        if full not in texts:
            with open(full, encoding="utf-8") as fh:
                texts[full] = fh.read()
        n = texts[full].count(old)
        if n != 1:
            raise SystemExit("%s: anchor occurs %d times in %s: %r" % (mid, n, path, old[:70]))
        texts[full] = texts[full].replace(old, new, 1)
    return texts


def main(argv):
    if len(argv) == 2 and argv[1] == "--list":
        for mid, (tests, _) in MUTANTS.items():
            print(mid, tests)
        return 0
    if len(argv) == 3 and argv[1] == "--check":
        for mid in MUTANTS:
            plan(argv[2], mid)
        print("all %d mutants anchor exactly once" % len(MUTANTS))
        return 0
    if len(argv) != 3 or argv[2] not in MUTANTS:
        raise SystemExit("usage: mutate.py <root> <id> | --list | --check <root>")
    for full, text in plan(argv[1], argv[2]).items():
        with open(full, "w", encoding="utf-8") as fh:
            fh.write(text)
        print("mutated", full)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
