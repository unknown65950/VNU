/* seq — print a sequence of numbers (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    long first = 1, step = 1, last = 0;
    if (argc == 2) {
        last = atol(argv[1]);
    } else if (argc == 3) {
        first = atol(argv[1]);
        last = atol(argv[2]);
    } else if (argc >= 4) {
        first = atol(argv[1]);
        step = atol(argv[2]);
        last = atol(argv[3]);
    } else {
        we("seq: usage: seq [FIRST [STEP]] LAST\n");
        return 1;
    }
    if (step == 0)
        step = 1;
    if (step > 0) {
        for (long v = first; v <= last; v += step) {
            put_uint((unsigned long)v);
            w("\n");
        }
    } else {
        for (long v = first; v >= last; v += step) {
            put_uint((unsigned long)v);
            w("\n");
        }
    }
    return 0;
}