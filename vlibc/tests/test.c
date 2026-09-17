#include <vlibc/stdio.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/unistd.h>

int main(int argc, char** argv) {
    printf("Hello from vlibc!\n");
    printf("argc = %d\n", argc);
    
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }
    
    const char* test_str = "Testing vlibc";
    char buffer[100];
    strcpy(buffer, test_str);
    printf("strcpy: %s\n", buffer);
    printf("strlen: %lu\n", strlen(buffer));
    
    int num = 42;
    printf("Number: %d, Hex: %x\n", num, num);
    
    char* ptr = malloc(100);
    if (ptr) {
        strcpy(ptr, "Allocated memory!");
        printf("%s\n", ptr);
        free(ptr);
    }
    
    printf("PID: %lu\n", getpid());
    printf("UID: %lu\n", getuid());
    
    return 0;
}
