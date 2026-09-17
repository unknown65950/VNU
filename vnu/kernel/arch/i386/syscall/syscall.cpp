#include <cstdint>
#include <vnu/abi.h>
#include <vnu/vfs.h>
#include <vnu/process.h>
#include <vnu/kbd.h>
#include <vnu/tty.h>
#include <vnu/pipe.h>
#include <vnu/utsname.h>
#include <vnu/gui.h>
#include <vnu/wintask.h>
#include <vnu/install.h>

struct TrapFrame {
    std::uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    std::uint32_t eip, cs, eflags;
};

static void serial_putc(char c)
{
    auto inb = [](std::uint16_t p) {
        std::uint8_t v;
        asm volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
        return v;
    };
    while (!(inb(0x3F8 + 5) & 0x20)) {
    }
    asm volatile("outb %0,%1" : : "a"((std::uint8_t)c), "Nd"((std::uint16_t)0x3F8));
}

extern "C" void vnu_return_to_console();
extern "C" std::uint32_t vnu_pending_user_esp;
extern "C" std::uint8_t vnu_pending_user_esp_flag;
extern "C" std::uint32_t vnu_pending_user_eip;
extern "C" std::uint8_t vnu_pending_jump_flag;

extern "C" std::uint32_t vnu_syscall_dispatch(TrapFrame* tf)
{
    switch (tf->eax) {
    case VNU_SYS_write: {
        /* Gfx surface fd 3: when running as a windowed task, writes
         * to fd 3 go into the pixel framebuffer (not the VFS) — as
         * long as that descriptor isn't a real file the task opened
         * (the shell's history/redirection files land on fd 3 and must
         * NOT be intercepted). The app opens /dev/fb or just
         * lseek+writes to fd 3 directly. */
        if (tf->ebx == 3 && vnu::wintask::current_is_task() &&
            !vnu::vfs::fd_is_open(3) && !vnu::pipe::is_pipe_fd(3)) {
            return static_cast<std::uint32_t>(
                vnu::wintask::task_gfx_write(
                    reinterpret_cast<const char*>(tf->ecx), tf->edx));
        }

        if (vnu::pipe::is_pipe_fd(static_cast<int>(tf->ebx))) {
            return static_cast<std::uint32_t>(vnu::pipe::write_fd(
                static_cast<int>(tf->ebx), reinterpret_cast<const void*>(tf->ecx), tf->edx));
        }
        if (((tf->ebx == 1 || tf->ebx == 2) &&
             !vnu::vfs::fd_is_redirected(static_cast<int>(tf->ebx))) ||
            vnu::vfs::fd_dev_kind(static_cast<int>(tf->ebx)) == vnu::vfs::DevKind::Tty) {
            const char* p = reinterpret_cast<const char*>(tf->ecx);
            std::uint32_t n = tf->edx;
            if (vnu::wintask::current_is_task()) {
                /* Windowed task: its stdout/stderr go into its own
                 * console window instead of the real VGA text plane. */
                vnu::wintask::task_write(p, n);
                return n;
            }
            /* VGA once per batch (flush cursor at end) — avoids CRTC lag */
            vnu::tty::write(p, n);
            for (std::uint32_t i = 0; i < n; ++i)
                serial_putc(p[i]);
            return n;
        }
        return static_cast<std::uint32_t>(
            vnu::vfs::write(static_cast<int>(tf->ebx),
                            reinterpret_cast<const void*>(tf->ecx), tf->edx));
    }
    case VNU_SYS_read: {
        if (vnu::pipe::is_pipe_fd(static_cast<int>(tf->ebx))) {
            /* Blocking pipe read for coro processes: a reader with a
             * live writer but no data yet parks on the scheduler until
             * the writer (or another coro process) gets its slice and
             * fills the pipe. Legacy/windowed callers keep the old
             * EAGAIN behavior (no scheduler to wait on). */
            int rc = 0;
            for (;;) {
                rc = vnu::pipe::read_fd(static_cast<int>(tf->ebx),
                                        reinterpret_cast<void*>(tf->ecx), tf->edx);
                if (rc != -VNU_EAGAIN || !vnu::proc::can_yield())
                    break;
                vnu::proc::yield_current();
            }
            return static_cast<std::uint32_t>(rc);
        }
        if ((tf->ebx == 0 && !vnu::vfs::fd_is_redirected(0)) ||
            vnu::vfs::fd_dev_kind(static_cast<int>(tf->ebx)) == vnu::vfs::DevKind::Tty) {
            char* buf = reinterpret_cast<char*>(tf->ecx);
            std::uint32_t n = tf->edx;
            if (!buf || n == 0)
                return 0;
            bool windowed = vnu::wintask::current_is_task();
            for (std::uint32_t i = 0; i < n; ++i) {
                char ch = 0;
                if (windowed) {
                    ch = vnu::wintask::task_getch_blocking();
                } else if (vnu::proc::can_yield()) {
                    /* Coro-scheduled process: don't spin the whole
                     * machine waiting for a key — yield to the scheduler
                     * (which earlier yielded to us) until a scancode
                     * decodes into a character. */
                    for (;;) {
                        int c = vnu::kbd::poll_char();
                        if (c >= 0) {
                            ch = static_cast<char>(c);
                            break;
                        }
                        vnu::proc::yield_current();
                    }
                } else {
                    ch = vnu::kbd::getch_blocking();
                }
                buf[i] = ch;
                if (ch == '\n')
                    return i + 1;
            }
            return n;
        }
        return static_cast<std::uint32_t>(
            vnu::vfs::read(static_cast<int>(tf->ebx),
                           reinterpret_cast<void*>(tf->ecx), tf->edx));
    }
    case VNU_SYS_open:
        return static_cast<std::uint32_t>(
            vnu::vfs::open(reinterpret_cast<const char*>(tf->ebx), tf->ecx));
    case VNU_SYS_close: {
        int fd = static_cast<int>(tf->ebx);
        if (vnu::pipe::is_pipe_fd(fd))
            return static_cast<std::uint32_t>(vnu::pipe::close_fd(fd));
        return static_cast<std::uint32_t>(vnu::vfs::close(fd));
    }
    case VNU_SYS_lseek: {
        /* Gfx surface fd 3: reposition the cursor into the pixel
         * framebuffer before the next task_gfx_write, exactly like
         * lseek() on /dev/fb.  Only intercepted for windowed tasks
         * — normal processes go straight to the VFS. */
        if (tf->ebx == 3 && vnu::wintask::current_is_task() &&
            !vnu::vfs::fd_is_open(3) && !vnu::pipe::is_pipe_fd(3)) {
            return static_cast<std::uint32_t>(
                vnu::wintask::task_gfx_seek(
                    static_cast<std::int32_t>(tf->ecx),
                    static_cast<int>(tf->edx)));
        }
        return static_cast<std::uint32_t>(vnu::vfs::lseek(
            static_cast<int>(tf->ebx), static_cast<int32_t>(tf->ecx),
            static_cast<int>(tf->edx)));
    }
    case VNU_SYS_stat:
        return static_cast<std::uint32_t>(vnu::vfs::stat(
            reinterpret_cast<const char*>(tf->ebx),
            reinterpret_cast<vnu::posix::Stat*>(tf->ecx)));
    case VNU_SYS_fstat:
        return static_cast<std::uint32_t>(vnu::vfs::fstat(
            static_cast<int>(tf->ebx), reinterpret_cast<vnu::posix::Stat*>(tf->ecx)));
    case VNU_SYS_exit: {
        int status = static_cast<int>(tf->ebx);
        if (vnu::wintask::current_is_task()) {
            /* If this was a command run from a windowed shell, go back
             * to that shell rather than killing the window. */
            uint32_t new_eip = 0, new_esp = 0;
            if (vnu::wintask::exit_respawns_root(new_eip, new_esp)) {
                vnu_pending_user_eip = new_eip;
                vnu_pending_user_esp = new_esp;
                vnu_pending_jump_flag = 1;
                return 0;
            }
            vnu::wintask::task_exit(); /* noreturn: switches back to the GUI */
        }
        if (vnu::proc::can_yield()) {
            /* Coro-scheduled process (init or a spawned service): sys_exit
             * converts it to a zombie and hands control straight back to
             * the scheduler coroutine on its own stack — never touch the
             * legacy single-slot console continuation below. */
            vnu::proc::sys_exit(status);
            return 0; /* not reached */
        }
        bool back = (vnu::proc::current() && vnu::proc::current()->ppid == 0);
        vnu::proc::sys_exit(status);
        if (back) {
            vnu_return_to_console();
        }
        auto* p = vnu::proc::current();
        if (p && p->pid != 0) {
            tf->eip = p->regs.eip;
            tf->esp = p->regs.esp;
            tf->eflags = p->regs.eflags ? p->regs.eflags : 0x202;
            tf->eax = p->regs.eax;
            tf->ebx = p->regs.ebx;
            tf->ecx = p->regs.ecx;
            tf->edx = p->regs.edx;
            tf->esi = p->regs.esi;
            tf->edi = p->regs.edi;
            tf->ebp = p->regs.ebp;
            return tf->eax;
        }
        vnu_return_to_console();
        return 0;
    }
    case VNU_SYS_getpid:
        return static_cast<std::uint32_t>(vnu::proc::sys_getpid());
    case VNU_SYS_fork: {
        vnu::proc::Registers trap{};
        trap.edi = tf->edi;
        trap.esi = tf->esi;
        trap.ebp = tf->ebp;
        trap.esp = tf->esp;
        trap.ebx = tf->ebx;
        trap.edx = tf->edx;
        trap.ecx = tf->ecx;
        trap.eax = tf->eax;
        trap.eip = tf->eip;
        trap.eflags = tf->eflags;
        return static_cast<std::uint32_t>(vnu::proc::sys_fork(&trap));
    }
    case VNU_SYS_execve: {
        if (vnu::wintask::current_is_task()) {
            /* Running inside a windowed task (e.g. `term`/vash exec'ing
             * the next command the user typed) — this can't go through
             * the classic sys_execve() path below, which knows nothing
             * about wintask's separate stack/scheduling model. Reload
             * the new binary the same way, on this task's own stack,
             * via vnu::wintask::exec_current(), and hand off through
             * the exact same "pending jump" mechanism normal execve()
             * uses (see syscall.s) — it doesn't care which subsystem
             * set eip/esp, just that they're valid. */
            uint32_t new_eip = 0, new_esp = 0;
            bool ok = vnu::wintask::exec_current(
                reinterpret_cast<const char*>(tf->ebx),
                reinterpret_cast<char* const*>(tf->ecx), new_eip, new_esp);
            if (!ok)
                return static_cast<std::uint32_t>(-VNU_ENOENT);
            vnu_pending_user_eip = new_eip;
            vnu_pending_user_esp = new_esp;
            vnu_pending_jump_flag = 1;
            return 0;
        }

        /* "gui" is not a real ELF on disk (there is no /apps/ launcher
         * or userspace toolkit yet) — vash still resolves it through
         * the normal PATH lookup to "/bin/gui" and calls execve() like
         * any other command, so we intercept that one path here and
         * run the VibeGraphics event loop directly in the kernel.
         * When it exits (Esc), behave exactly like any other external
         * command finishing: end this vash session the same way
         * VNU_SYS_exit does, so the shell restarts cleanly. */
        const char* gui_path = reinterpret_cast<const char*>(tf->ebx);
        bool is_gui = gui_path && gui_path[0] == '/' && gui_path[1] == 'b' &&
                      gui_path[2] == 'i' && gui_path[3] == 'n' && gui_path[4] == '/' &&
                      gui_path[5] == 'g' && gui_path[6] == 'u' && gui_path[7] == 'i' &&
                      gui_path[8] == '\0';
        if (is_gui) {
            vnu::gui::run();
            bool back = (vnu::proc::current() && vnu::proc::current()->ppid == 0);
            vnu::proc::sys_exit(0);
            if (back) {
                vnu_return_to_console();
            }
            auto* p = vnu::proc::current();
            if (p && p->pid != 0) {
                tf->eip = p->regs.eip;
                tf->esp = p->regs.esp;
                tf->eflags = p->regs.eflags ? p->regs.eflags : 0x202;
                tf->eax = 0;
                tf->ebx = tf->ecx = tf->edx = 0;
                tf->esi = tf->edi = tf->ebp = 0;
                return 0;
            }
            vnu_return_to_console();
            return 0;
        }

        vnu::proc::Registers trap{};
        trap.edi = tf->edi;
        trap.esi = tf->esi;
        trap.ebp = tf->ebp;
        trap.esp = tf->esp;
        trap.ebx = tf->ebx;
        trap.edx = tf->edx;
        trap.ecx = tf->ecx;
        trap.eax = tf->eax;
        trap.eip = tf->eip;
        trap.eflags = tf->eflags;
        int rc = vnu::proc::sys_execve(&trap, reinterpret_cast<const char*>(tf->ebx),
                                       reinterpret_cast<char* const*>(tf->ecx));
        if (rc == 0) {
            /* Must not iretd onto user stack — jump cleanly to new entry. */
            vnu_pending_user_eip = trap.eip;
            vnu_pending_user_esp = trap.esp;
            vnu_pending_jump_flag = 1;
            return 0;
        }
        return static_cast<std::uint32_t>(rc);
    }
    case VNU_SYS_mkdir:
        return static_cast<std::uint32_t>(
            vnu::vfs::mkdir(reinterpret_cast<const char*>(tf->ebx)));
    case VNU_SYS_rmdir:
        return static_cast<std::uint32_t>(
            vnu::vfs::rmdir(reinterpret_cast<const char*>(tf->ebx)));
    case VNU_SYS_unlink:
        return static_cast<std::uint32_t>(
            vnu::vfs::unlink(reinterpret_cast<const char*>(tf->ebx)));
    case VNU_SYS_getdents:
        return static_cast<std::uint32_t>(vnu::vfs::getdents(
            static_cast<int>(tf->ebx), reinterpret_cast<void*>(tf->ecx), tf->edx));
    case VNU_SYS_chdir:
        return static_cast<std::uint32_t>(
            vnu::vfs::chdir(reinterpret_cast<const char*>(tf->ebx)));
    case VNU_SYS_getcwd:
        return static_cast<std::uint32_t>(vnu::vfs::getcwd(
            reinterpret_cast<char*>(tf->ebx), tf->ecx));
    case VNU_SYS_getuid:
        return vnu::proc::sys_getuid();
    case VNU_SYS_getgid:
        return vnu::proc::sys_getgid();
    case VNU_SYS_setuid:
        return static_cast<std::uint32_t>(vnu::proc::sys_setuid(tf->ebx));
    case VNU_SYS_setgid:
        return static_cast<std::uint32_t>(vnu::proc::sys_setgid(tf->ebx));
    case VNU_SYS_chmod:
        return static_cast<std::uint32_t>(
            vnu::vfs::chmod(reinterpret_cast<const char*>(tf->ebx), tf->ecx));
    case VNU_SYS_chown:
        return static_cast<std::uint32_t>(
            vnu::vfs::chown(reinterpret_cast<const char*>(tf->ebx), tf->ecx, tf->edx));
    case VNU_SYS_brk: {
        std::uint32_t req = tf->ebx;
        /* Windowed tasks get their own pre-mapped heap slice
         * (BRK_MIN..BRK_MAX, see wintask.cpp's spawn ranges), so track
         * the break per task instead of through the shared process
         * heap — otherwise every windowed app would stomp the single
         * g_brk that classic processes share. */
        if (vnu::wintask::current_is_task())
            return vnu::wintask::task_brk(req);
        if (req == 0)
            return vnu::proc::brk_current();
        if (req < vnu::proc::BRK_MIN || req > vnu::proc::BRK_MAX)
            return static_cast<std::uint32_t>(-VNU_ENOMEM);
        return vnu::proc::brk_set(req);
    }

    case VNU_SYS_waitpid:
        return static_cast<std::uint32_t>(vnu::proc::sys_waitpid(
            static_cast<int>(tf->ebx), reinterpret_cast<int*>(tf->ecx), static_cast<int>(tf->edx)));
    case VNU_SYS_pipe: {
        int fds[2];
        int rc = vnu::pipe::create(fds);
        if (rc < 0)
            return static_cast<std::uint32_t>(rc);
        auto* out = reinterpret_cast<int*>(tf->ebx);
        if (!out)
            return static_cast<std::uint32_t>(-VNU_EFAULT);
        out[0] = fds[0];
        out[1] = fds[1];
        return 0;
    }
    case VNU_SYS_dup: {
        /* Pipes keep their own fd table, so try that first and fall
         * back to the VFS for ordinary files and /dev nodes. */
        int fd = static_cast<int>(tf->ebx);
        if (vnu::pipe::is_pipe_fd(fd))
            return static_cast<std::uint32_t>(vnu::pipe::dup_fd(fd));
        return static_cast<std::uint32_t>(vnu::vfs::dup_fd(fd));
    }
    case VNU_SYS_dup2: {
        int oldfd = static_cast<int>(tf->ebx);
        int newfd = static_cast<int>(tf->ecx);
        if (vnu::pipe::is_pipe_fd(oldfd))
            return static_cast<std::uint32_t>(vnu::pipe::dup2_fd(oldfd, newfd));
        return static_cast<std::uint32_t>(vnu::vfs::dup2_fd(oldfd, newfd));
    }
    case VNU_SYS_kill:
        return static_cast<std::uint32_t>(
            vnu::proc::sys_kill(static_cast<int>(tf->ebx), static_cast<int>(tf->ecx)));
    case VNU_SYS_uname: {
        auto* u = reinterpret_cast<vnu_utsname*>(tf->ebx);
        if (!u)
            return static_cast<std::uint32_t>(-VNU_EFAULT);
        auto cpy = [](char* d, const char* s) {
            int i = 0;
            for (; s[i] && i < 63; ++i)
                d[i] = s[i];
            d[i] = 0;
        };
        cpy(u->sysname, "VNU");
        cpy(u->nodename, "vnu");
        cpy(u->release, "0.2");
        cpy(u->version, "vibe");
        cpy(u->machine, "i386");
        cpy(u->processor, "i386");
        cpy(u->hardware_platform, "pc");
        cpy(u->operating_system, "VNU");
        return 0;
    }

    case VNU_SYS_getppid: {
        auto* p = vnu::proc::current();
        return static_cast<std::uint32_t>(p ? p->ppid : 0);
    }
    case VNU_SYS_isatty: {
        int fd = static_cast<int>(tf->ebx);
        if (fd == 0 || fd == 1 || fd == 2) {
            /* Only a terminal while still pointing at the console —
             * once the shell has redirected it to a file, it isn't. */
            return vnu::vfs::fd_is_redirected(fd) ? 0 : 1;
        }
        if (vnu::vfs::fd_dev_kind(fd) == vnu::vfs::DevKind::Tty)
            return 1;
        return 0;
    }

    case VNU_SYS_spawn:
        /* Launch a service as a new coro process in its own address
         * space, without replacing the caller (init keeps supervising).
         * The scheduler runs it once the caller yields/blocks. */
        return static_cast<std::uint32_t>(
            vnu::proc::sys_spawn(reinterpret_cast<const char*>(tf->ebx)));

    case VNU_SYS_blkcount:
        return static_cast<std::uint32_t>(vnu::install::disk_count());

    case VNU_SYS_install:
        return static_cast<std::uint32_t>(
            vnu::install::install(static_cast<int>(tf->ebx)));

    default:
        return static_cast<std::uint32_t>(-VNU_ENOSYS);
    }
}

extern "C" {
std::uint32_t vnu_pending_user_esp = 0;
std::uint8_t vnu_pending_user_esp_flag = 0;
std::uint32_t vnu_pending_user_eip = 0;
std::uint8_t vnu_pending_jump_flag = 0;
}
