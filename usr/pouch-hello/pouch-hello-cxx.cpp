// /pouch-hello-cxx -- the CL-2 C++ runtime prover (Clade arc,
// docs/LLVM-DESIGN.md workstream CL-2). Exercises every seam of the C++
// runtime stack (libunwind + libc++abi + libc++, static, over the pouch
// musl sysroot) END TO END, so a regression in any runtime wire surfaces
// as a labelled FAIL.
//
// Runs POST-pivot from a WRITABLE root (spawned by joey after the Stratum
// pivot), like pouch-hello-fs -- the std::filesystem leg needs a writable
// cwd.
//
// The seams proven (C++ runtime feature -> the substrate it rides):
//   throw / catch          -> libunwind (.eh_frame unwind) + libc++abi
//                             (__gxx_personality_v0 + __cxa_throw)
//   RTTI (typeid/dynamic_cast) -> libc++abi type_info
//   std::thread + join     -> musl pthread via _LIBCPP_HAS_THREAD_API_PTHREAD
//   thread_local dtor      -> __cxa_thread_atexit (musl provides the _impl)
//   std::string/vector/map/sort -> libc++ containers + <algorithm>
//   iostreams              -> libc++ <iostream> over musl stdio (fd 1)
//   std::filesystem        -> the CL-1a dirent/stat/mkdir/rename wires +
//                             the getcwd fix (current_path)
//
// On success: "pouch-hello-cxx: ALL C++ WIRES PASS" + exit 0. Any seam
// failing prints "pouch-hello-cxx: <seam> FAIL ..." and exits non-zero so
// joey's reap sees it. fd 1 is a pipe joey relays to the boot-log UART.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <typeinfo>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

static int fail(const char *seam, const char *why) {
    std::printf("pouch-hello-cxx: %s FAIL (%s)\n", seam, why);
    return 1;
}

// --- RTTI: a small polymorphic hierarchy for dynamic_cast + typeid ---
struct Animal { virtual ~Animal() = default; virtual const char *sound() const = 0; };
struct Thylacine : Animal { const char *sound() const override { return "yip-bark"; } };
struct Joey      : Animal { const char *sound() const override { return "squeak"; } };

// --- thread_local with a non-trivial destructor (drives __cxa_thread_atexit) ---
static std::atomic<int> g_tls_dtor_runs{0};
struct TlsGuard {
    bool armed = false;
    ~TlsGuard() { if (armed) g_tls_dtor_runs.fetch_add(1, std::memory_order_relaxed); }
};
static thread_local TlsGuard t_guard;

// --- concurrent Meyers-singleton race (drives libc++abi __cxa_guard directly) ---
// On pouch, cxa_guard's recursion check keys on syscall(SYS_gettid), which routes
// to the ENOSYS sentinel -> every thread gets the same bogus id -> a concurrent
// FIRST init of a function-local static false-aborts ("recursive initialization").
// This wire fires that deterministically: NRACE threads barrier-sync, then race
// the SAME static's first init whose ctor spins to widen the guard's PENDING
// window, so the waiters pile into __cxa_guard_acquire's recursion check together.
// The fix (libcxxabi PlatformThreadID -> pthread_self, a real per-thread id) makes
// the waiter's id != the initializer's id -> it waits correctly instead of aborting.
static constexpr int NRACE = 8;
static std::atomic<int> g_race_arrived{0};
struct SlowSingleton {
    int v;
    SlowSingleton() {
        volatile long x = 0;
        for (long i = 0; i < 800000; i++) x += i;   // widen the PENDING window
        v = 7;
    }
};
static SlowSingleton &race_singleton() { static SlowSingleton s; return s; }

int main() {
    // ---------------------------------------------------------------
    // 1. Exceptions: throw across a frame, catch by base, read what().
    // ---------------------------------------------------------------
    {
        bool caught = false;
        try {
            throw std::runtime_error("clade-eh");
        } catch (const std::exception &e) {
            caught = (std::strcmp(e.what(), "clade-eh") == 0);
        } catch (...) {
            return fail("exceptions", "caught wrong type");
        }
        if (!caught) return fail("exceptions", "throw/catch did not unwind");

        // A second throw through an intervening non-catching frame, to
        // exercise the unwinder walking >1 frame + running a local dtor.
        static std::atomic<int> unwind_dtor{0};
        struct Marker { ~Marker() { unwind_dtor.fetch_add(1, std::memory_order_relaxed); } };
        auto deep = []() { Marker m; throw 42; };
        bool caught_int = false;
        try { deep(); } catch (int v) { caught_int = (v == 42); }
        if (!caught_int) return fail("exceptions", "nested throw not caught");
        if (unwind_dtor.load() != 1) return fail("exceptions", "unwind did not run local dtor");
    }

    // ---------------------------------------------------------------
    // 2. RTTI: dynamic_cast down the hierarchy + typeid names.
    // ---------------------------------------------------------------
    {
        std::unique_ptr<Animal> a = std::make_unique<Thylacine>();
        if (std::strcmp(a->sound(), "yip-bark") != 0) return fail("rtti", "vcall wrong");
        auto *t = dynamic_cast<Thylacine *>(a.get());
        if (!t) return fail("rtti", "dynamic_cast to actual type failed");
        auto *j = dynamic_cast<Joey *>(a.get());
        if (j) return fail("rtti", "dynamic_cast to wrong type succeeded");
        if (typeid(*a) != typeid(Thylacine)) return fail("rtti", "typeid mismatch");
    }

    // ---------------------------------------------------------------
    // 3. Containers + <algorithm>: the STL data path.
    // ---------------------------------------------------------------
    {
        std::vector<int> v{5, 3, 9, 1, 7, 2};
        std::sort(v.begin(), v.end());
        if (!std::is_sorted(v.begin(), v.end())) return fail("stl", "sort");
        std::map<std::string, int> m;
        for (int x : v) m[std::to_string(x)] = x;
        if (m.size() != v.size()) return fail("stl", "map size");
        if (m["9"] != 9) return fail("stl", "map lookup");
        std::string s = "thyla" + std::string("cine");
        if (s != "thylacine") return fail("stl", "string concat");
    }

    // ---------------------------------------------------------------
    // 4. Threads + TLS destructors: spawn workers that each touch a
    //    thread_local with a dtor; join; assert every worker's TLS dtor
    //    ran (the __cxa_thread_atexit wire).
    // ---------------------------------------------------------------
    {
        constexpr int NWORK = 4;
        // Pre-init the __cxa_thread_atexit machinery on the MAIN thread (its
        // libc++abi function-local `manager` static) so that static's guard
        // reaches COMPLETE **uncontended**, BEFORE the workers race to register.
        // A concurrent FIRST init of it would hit libc++abi's __cxa_guard
        // recursion check, which keys on syscall(SYS_gettid) -- pouch routes it to
        // ENOSYS so every thread shares one bogus id -> a false "recursive
        // initialization" abort (the tracked gettid seam,
        // memory/bug_cxa_guard_gettid.md). This main-thread ODR-use of the
        // thread_local emits the process's first __cxa_thread_atexit, so the
        // workers below find `manager` already-COMPLETE and only their TLS-dtor
        // REGISTRATION (not the static's init) is exercised concurrently.
        t_guard.armed = false;   // main-thread touch -> inits manager uncontended

        std::atomic<int> sum{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < NWORK; i++) {
            ts.emplace_back([i, &sum]() {
                t_guard.armed = true;               // arm this thread's TLS dtor
                sum.fetch_add(i + 1, std::memory_order_relaxed);
            });
        }
        for (auto &th : ts) th.join();
        if (sum.load() != (1 + 2 + 3 + 4)) return fail("threads", "worker sum wrong");
        // Each of the NWORK worker threads exited -> its thread_local
        // TlsGuard dtor must have run via __cxa_thread_atexit.
        if (g_tls_dtor_runs.load() != NWORK)
            return fail("tls-dtor", "not every worker ran its thread_local dtor");
    }

    // ---------------------------------------------------------------
    // 5. iostreams: write through libc++ <iostream> to fd 1.
    // ---------------------------------------------------------------
    {
        std::cout << "pouch-hello-cxx: iostream ok (" << 6 * 7 << ")\n";
        std::cout.flush();
        if (!std::cout.good()) return fail("iostream", "stream bad after write");
    }

    // ---------------------------------------------------------------
    // 6. std::filesystem: current_path (getcwd) + create/iterate/remove
    //    (the CL-1a dirent/stat/mkdir wires). Uses an absolute working
    //    dir so it does not depend on the spawn cwd.
    // ---------------------------------------------------------------
    {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (ec) return fail("filesystem", "current_path (getcwd)");
        if (cwd.empty() || cwd.native()[0] != '/') return fail("filesystem", "cwd not absolute");

        fs::path dir = "/pouch-cxx-probe";
        // Best-effort pre-clean via the AT_FDCWD-safe wire -- NOT fs::remove_all,
        // which recurses through dirfd-relative unlinkat(dirfd,name) -> the v1.0
        // AT_FDCWD-only ENOTSUP seam (the CL-4 lift noted below), so on a
        // surviving populated dir it is DEAD (files remain). If a prior boot's
        // prover failed between create and the cleanup loop and the pool is
        // reused (PRESERVE=1), a dead pre-clean would leave create_directory
        // returning false-with-cleared-ec -> a boot-fatal "create_directory
        // FAIL" that MASKS the true prior cause. The probe creates only flat
        // files, so one directory_iterator level + fs::remove(abspath) empties
        // it (the same AT_FDCWD path the cleanup loop below uses).
        if (fs::exists(dir)) {
            std::error_code ec2;
            for (const auto &e : fs::directory_iterator(dir, ec2)) fs::remove(e.path(), ec2);
            fs::remove(dir, ec2);
        }
        if (!fs::create_directory(dir, ec) || ec) return fail("filesystem", "create_directory");

        for (const char *name : {"a.txt", "b.txt", "c.txt"}) {
            std::FILE *f = std::fopen((dir / name).c_str(), "w");
            if (!f) return fail("filesystem", "fopen in probe dir");
            std::fputs("clade\n", f);
            std::fclose(f);
        }
        int count = 0;
        for (const auto &ent : fs::directory_iterator(dir, ec)) {
            if (ec) return fail("filesystem", "directory_iterator");
            if (ent.is_regular_file()) count++;
        }
        if (count != 3) return fail("filesystem", "directory_iterator count wrong");

        fs::rename(dir / "a.txt", dir / "renamed.txt", ec);
        if (ec) return fail("filesystem", "rename");
        if (fs::exists(dir / "a.txt")) return fail("filesystem", "old name survived rename");
        if (!fs::exists(dir / "renamed.txt")) return fail("filesystem", "new name absent");
        if (fs::file_size(dir / "renamed.txt", ec) != 6 || ec)
            return fail("filesystem", "file_size (stat) wrong");

        // fs::remove each file (absolute path), then rmdir the now-empty dir.
        // fs::remove -> ::remove(3) -> the pouch-wired unlink()/rmdir() (0027).
        // std::filesystem::remove_all + recursive_directory_iterator are NOT used:
        // they walk via dirfd-relative openat(fd,name)/unlinkat(fd,name), and the
        // pouch FS wire is AT_FDCWD-only at v1.0 (a real dirfd -> ENOTSUP; a CL-4
        // seam). fs::remove(abspath) rides the working AT_FDCWD wire.
        for (const char *name : {"renamed.txt", "b.txt", "c.txt"}) {
            if (!fs::remove(dir / name, ec) || ec)
                return fail("filesystem", "remove file");
        }
        if (!fs::remove(dir, ec) || ec) return fail("filesystem", "remove (rmdir) dir");
        if (fs::exists(dir)) return fail("filesystem", "probe dir survived remove");
    }

    // ---------------------------------------------------------------
    // 7. Concurrent Meyers-singleton init: NRACE threads race the SAME
    //    function-local static's FIRST init (the libc++abi __cxa_guard
    //    path). A barrier makes them hit __cxa_guard_acquire together;
    //    the initializer spins to keep the guard PENDING while the
    //    waiters run their recursion check. Pre-fix (gettid==ENOSYS ->
    //    shared bogus id) this false-aborts; post-fix (pthread_self id)
    //    every waiter waits correctly and reads the singleton.
    // ---------------------------------------------------------------
    {
        std::atomic<int> got{0};
        std::vector<std::thread> rs;
        for (int i = 0; i < NRACE; i++) {
            rs.emplace_back([&got]() {
                g_race_arrived.fetch_add(1, std::memory_order_relaxed);
                while (g_race_arrived.load(std::memory_order_relaxed) < NRACE)
                    std::this_thread::yield();
                if (race_singleton().v == 7)
                    got.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto &t : rs) t.join();
        if (got.load() != NRACE)
            return fail("cxa-guard-race", "concurrent singleton init lost a racer");
    }

    std::printf("pouch-hello-cxx: ALL C++ WIRES PASS\n");
    return 0;
}
