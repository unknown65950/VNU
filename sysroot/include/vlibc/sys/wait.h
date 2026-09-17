#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int waitpid(int pid, int* status, int options);
int wait(int* status);
#ifdef __cplusplus
}
#endif
