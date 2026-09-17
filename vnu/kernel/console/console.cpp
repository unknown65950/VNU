#include <stdint.h>
#include <stddef.h>
#include <vnu/vfs.h>
#include <vnu/process.h>
#include <vnu/tty.h>
#include <vnu/kbd.h>

namespace {

void print(const char* s) { vnu::tty::write_cstr(s); }

bool eq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}

bool starts_with(const char* s, const char* prefix, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (s[i] != prefix[i])
            return false;
        if (prefix[i] == 0)
            return true;
    }
    return true;
}

void prompt()
{
    print("VNU> ");
}

void cmd_help()
{
    print("Builtins: help ls cat pwd clear touch mkdir rm echo\n");
    print("          vcc vld run <path>\n");
    print("Prefer userspace: boot runs /bin/vash\n");
}

void cmd_run(const char* path)
{
    if (!path || !*path) {
        print("usage: run <path>\n");
        return;
    }
    print("running ");
    print(path);
    print("\n");
    int rc = vnu::proc::run_program(path);
    if (rc < 0)
        print("run failed\n");
    else
        print("[exited]\n");
}

} // namespace

extern "C" void vnu_console_start()
{
    char line[128];
    vnu::tty::clear();
    print("VNU native console (fallback)\n");
    print("type 'help'\n\n");

    for (;;) {
        prompt();
        size_t n = 0;
        for (;;) {
            char ch = vnu::kbd::getch_blocking();
            if (ch == '\n') {
                vnu::tty::putc('\n');
                vnu::tty::flush_cursor();
                break;
            }
            if (ch == '\b') {
                if (n) {
                    --n;
                    vnu::tty::putc('\b');
                    vnu::tty::flush_cursor();
                }
                continue;
            }
            if (ch && n < 127) {
                line[n++] = ch;
                vnu::tty::putc(ch);
                vnu::tty::flush_cursor();
            }
        }
        line[n] = 0;

        if (eq(line, "help"))
            cmd_help();
        else if (eq(line, "ls")) {
            char out[512];
            vnu::vfs::list(out, sizeof(out));
            print(out);
        } else if (eq(line, "pwd")) {
            char buf[64];
            vnu::vfs::getcwd(buf, sizeof(buf));
            print(buf);
            print("\n");
        } else if (eq(line, "clear"))
            vnu::tty::clear();
        else if (eq(line, "cat /etc/motd") || eq(line, "cat etc/motd")) {
            char b[512];
            int z = vnu::vfs::read_path("/etc/motd", b, sizeof(b) - 1);
            if (z >= 0) {
                b[z] = 0;
                print(b);
            } else
                print("not found\n");
        } else if (starts_with(line, "echo ", 5)) {
            print(line + 5);
            print("\n");
        } else if (starts_with(line, "touch ", 6)) {
            print(vnu::vfs::touch(line + 6) == 0 ? "ok\n" : "fail\n");
        } else if (starts_with(line, "mkdir ", 6)) {
            print(vnu::vfs::mkdir(line + 6) == 0 ? "ok\n" : "fail\n");
        } else if (starts_with(line, "rm ", 3)) {
            print(vnu::vfs::remove(line + 3) == 0 ? "ok\n" : "fail\n");
        } else if (eq(line, "vcc") || starts_with(line, "vcc ", 4))
            print("vcc: host tool ./tools/vcc\n");
        else if (eq(line, "vld") || starts_with(line, "vld ", 4))
            print("vld: host tool ./tools/vld\n");
        else if (eq(line, "run") || starts_with(line, "run ", 4))
            cmd_run(line[3] == ' ' ? line + 4 : "");
        else if (line[0])
            print("command not found\n");
    }
}
