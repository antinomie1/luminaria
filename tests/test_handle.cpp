// Self-checks for UniqueFd — the fd half of core/handle.cppm.
// No framework: assert-based, returns non-zero on failure.
//
// "Is it closed?" is checked with fcntl(F_GETFD), which fails with EBADF on a
// descriptor that is no longer open. Every fd here comes from a pipe, so no
// filesystem is touched.
#include <cassert>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

import luminaria;
import std;

using luminaria::UniqueFd;

namespace {

/// True while `fd` is still an open descriptor in this process.
bool is_open(int fd) {
    return fd >= 0 && ::fcntl(fd, F_GETFD) != -1;
}

/// One end of a fresh pipe; the other is closed immediately.
int make_fd() {
    int p[2]{};
    assert(::pipe(p) == 0);
    ::close(p[1]);
    return p[0];
}

void empty_is_invalid() {
    UniqueFd fd;
    assert(!fd.valid());
    assert(!fd);
    assert(fd.get() == -1);
}

void closes_on_scope_exit() {
    const int raw = make_fd();
    {
        UniqueFd fd{raw};
        assert(fd.valid());
        assert(fd.get() == raw);
        assert(is_open(raw));
    }
    assert(!is_open(raw));
}

void move_transfers_ownership() {
    const int raw = make_fd();
    UniqueFd a{raw};
    UniqueFd b{std::move(a)};
    assert(!a.valid()); // NOLINT(bugprone-use-after-move) — that is the check
    assert(b.get() == raw);
    assert(is_open(raw));
}

void move_assign_closes_the_old_one() {
    const int old_fd = make_fd();
    const int new_fd = make_fd();
    UniqueFd a{old_fd};
    UniqueFd b{new_fd};
    a = std::move(b);
    assert(!is_open(old_fd));
    assert(is_open(new_fd));
    assert(a.get() == new_fd);
}

void self_move_assign_keeps_the_fd() {
    const int raw = make_fd();
    UniqueFd a{raw};
    auto& alias = a;
    a = std::move(alias); // NOLINT(clang-diagnostic-self-move) — that is the check
    assert(a.get() == raw);
    assert(is_open(raw));
}

void release_hands_ownership_out() {
    const int raw = make_fd();
    int taken = -1;
    {
        UniqueFd fd{raw};
        taken = fd.release();
        assert(!fd.valid());
    }
    assert(taken == raw);
    assert(is_open(raw)); // the destructor must not have closed it
    ::close(taken);
}

void reset_closes_and_replaces() {
    const int first = make_fd();
    const int second = make_fd();
    UniqueFd fd{first};
    fd.reset(second);
    assert(!is_open(first));
    assert(fd.get() == second);

    fd.reset();
    assert(!is_open(second));
    assert(!fd.valid());
}

void reset_to_the_same_fd_is_not_a_double_close() {
    const int raw = make_fd();
    UniqueFd fd{raw};
    fd.reset(raw);
    assert(fd.get() == raw);
    assert(is_open(raw));
}

void duplicate_is_independent() {
    const int raw = make_fd();
    int copy = -1;
    {
        UniqueFd fd{raw};
        UniqueFd dup = fd.duplicate();
        assert(dup.valid());
        copy = dup.get();
        assert(copy != raw);
        assert(is_open(copy));
    } // both close here
    assert(!is_open(raw));
    assert(!is_open(copy));
}

void duplicate_of_empty_is_empty() {
    const UniqueFd fd;
    assert(!fd.duplicate().valid());
}

void survives_living_in_a_vector() {
    // The move-only contract has to hold under reallocation, which is where a
    // wrong move constructor shows up as a double close.
    std::vector<int> raws;
    std::vector<UniqueFd> fds;
    for (int i = 0; i < 32; ++i) {
        const int raw = make_fd();
        raws.push_back(raw);
        fds.emplace_back(raw);
    }
    for (const int raw : raws) {
        assert(is_open(raw));
    }
    fds.clear();
    for (const int raw : raws) {
        assert(!is_open(raw));
    }
}

} // namespace

int main() {
    empty_is_invalid();
    closes_on_scope_exit();
    move_transfers_ownership();
    move_assign_closes_the_old_one();
    self_move_assign_keeps_the_fd();
    release_hands_ownership_out();
    reset_closes_and_replaces();
    reset_to_the_same_fd_is_not_a_double_close();
    duplicate_is_independent();
    duplicate_of_empty_is_empty();
    survives_living_in_a_vector();
    return 0;
}
