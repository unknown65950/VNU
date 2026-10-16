// getenv.c - look a name up in the process environment.
//
// The environment arrives with the process and crt0 has already stored it
// in `environ` before main() runs, so all this has to do is walk the
// NULL-terminated array of "NAME=VALUE" strings and find one whose name
// matches.
//
// The match is on the '=' and not just on the characters before it, which
// is the whole of the function's edge cases: without it, getenv("PATH")
// would happily return the value of "PATH_EXTRA=1". An empty name is
// answered with NULL, which falls out of the same rule - nothing in the
// environment has a zero-length name, so no entry can match.
//
// `name` is not checked against NULL: POSIX declares the parameter
// nonnull, and a caller that passes NULL has asked for a value that does
// not exist in any meaning of the word.
//
// The value handed back points into the environment block, so a caller
// must not free it or write to it — which is what POSIX says, and why
// this does not copy.
#include <stdlib.h>
#include <string.h>
#include <vlibc/unistd.h>

char* getenv(const char* name)
{
    if (!*name)
        return (char*)0;
    unsigned long n = strlen(name);
    char** e = environ;
    if (!e)
        return (char*)0;
    for (; *e; ++e) {
        const char* entry = *e;
        unsigned long i = 0;
        while (i < n && entry[i] && entry[i] == name[i])
            ++i;
        if (i == n && entry[i] == '=')
            return (char*)(entry + n + 1);
    }
    return (char*)0;
}