#include <vlibc/signal.h>

/* Where a handler's `return` lands.
 *
 * The kernel cannot know this: it is the address inside the program's
 * own image that ends the handler and puts the interrupted context
 * back, and every program has its own copy of that code (the libc it
 * was linked against, in fact - a statically linked program brings its
 * own). sigaction() below installs this address for every handler it
 * sets, which is why a program using this header never has to know it
 * exists.
 *
 * Entered with the interrupted frame still on the stack below, and the
 * syscall below cannot touch it: the stack grows down, this code's own
 * frames go under the handler's, and the frame the kernel will pop is
 * above both.
 *
 * This is a naked stub on purpose: the handler's `return` leaves esp
 * just past the kernel-built return block, and this code pushes nothing
 * of its own, so the frame the int builds sits at a fixed, provable
 * distance under that block - the kernel relies on the exact offset
 * (rt_sigreturn finds the block 40 bytes above its own frame) to pick
 * the interrupted context and the mask to restore. The number must stay
 * in sync with VNU_SYS_rt_sigreturn in vnu/abi.h. 68 is that value. */
__attribute__((naked)) void __vnu_sigreturn(void)
{
    __asm__ volatile(
        "1:\n\t"
        "movl $68, %%eax\n\t"
        "int $0x80\n\t"
        "jmp 1b\n" ::: "eax");
}

int sigaction(int signo, const struct sigaction* act, struct sigaction* old)
{
    if (act) {
        /* Fill the restorer the kernel insists on, in the caller's copy
         * as well as ours: the kernel is told where the handler's
         * `return` goes, and the caller's struct is what it may read
         * back later through sigaction(signo, NULL, old). */
        struct sigaction a = *act;
        a.sa_restorer = __vnu_sigreturn;
        a.sa_flags |= VNU_SA_RESTORER;
        return (int)syscall(SYS_rt_sigaction, signo, &a, old);
    }
    return (int)syscall(SYS_rt_sigaction, signo, (void*)0, old);
}

__sighandler_t signal(int signo, __sighandler_t handler)
{
    struct sigaction act;
    struct sigaction old;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESETHAND;
    act.sa_restorer = 0; /* sigaction() fills this one */
    if (sigaction(signo, &act, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old)
{
    return (int)syscall(SYS_rt_sigprocmask, how, set, old);
}

int sigpending(sigset_t* set)
{
    return (int)syscall(SYS_sigpending, set);
}

int raise(int signo)
{
    return (int)syscall(SYS_sigraise, signo);
}

unsigned int alarm(unsigned int seconds)
{
    return (unsigned int)syscall(SYS_alarm, seconds);
}
