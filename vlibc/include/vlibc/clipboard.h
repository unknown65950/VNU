#ifndef VLIB_CLIPBOARD_H_
#define VLIB_CLIPBOARD_H_

/*
 * The system clipboard, as a library: tiny helpers over the
 * /dev/clipboard character device.
 *
 * The device is the interface - writing it replaces the clipboard,
 * reading it walks the text from the start to the end - so these are
 * just the open/read/write/close dance in one place. They are static
 * inline because they are a convenience, not a new ABI: a program that
 * prefers to open(2) the device itself is doing exactly the same thing.
 *
 * vclip_get() copies at most `cap` bytes and returns the count; it does
 * not NUL-terminate (the clipboard is bytes, not a C string), so a
 * caller printing it should add its own terminator.
 */

#include <vlibc/fcntl.h>
#include <vlibc/unistd.h>

/* Where the device lives, and the kernel's maximum clipboard size. */
#define VCLIP_DEV "/dev/clipboard"
#define VCLIP_CAP (64u * 1024u)

/* Replace the clipboard with `len` bytes from `buf`. Returns the number
 * of bytes stored, or -1 if the device could not be opened. */
static inline long vclip_set(const void* buf, unsigned long len)
{
    int fd = open(VCLIP_DEV, O_WRONLY);
    if (fd < 0)
        return -1;
    const char* p = (const char*)buf;
    unsigned long done = 0;
    while (done < len) {
        long n = write(fd, p + done, len - done);
        if (n <= 0)
            break;
        done += (unsigned long)n;
    }
    close(fd);
    return (long)done;
}

/* Copy the clipboard into `buf` (at most `cap` bytes) and return how
 * many bytes were copied, or -1 if the device could not be opened. */
static inline long vclip_get(void* buf, unsigned long cap)
{
    int fd = open(VCLIP_DEV, O_RDONLY);
    if (fd < 0)
        return -1;
    char* p = (char*)buf;
    unsigned long done = 0;
    while (done < cap) {
        long n = read(fd, p + done, cap - done);
        if (n <= 0)
            break;
        done += (unsigned long)n;
    }
    close(fd);
    return (long)done;
}

/* Empty the clipboard: opening it for writing clears it. */
static inline void vclip_clear(void)
{
    int fd = open(VCLIP_DEV, O_WRONLY);
    if (fd >= 0)
        close(fd);
}

#endif /* VLIB_CLIPBOARD_H_ */
