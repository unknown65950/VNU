// Точка входа для программ (x86_64, Linux).
//
// Важно: _start НЕ может быть обычной C-функцией. Компилятор
// вставляет в неё пролог (push %rbp; mov %rsp,%rbp; sub $N,%rsp
// под локальные переменные), и к моменту выполнения нашего кода
// %rsp уже не указывает на то, что реально положило ядро при
// запуске процесса. Раньше здесь была именно такая ошибка: argc
// читался из собственного стек-фрейма _start, а не из данных ядра —
// в лучшем случае мусор, в худшем сегфолт.
//
// Правильная схема (как в musl/glibc): голая asm-заглушка забирает
// "сырой" %rsp ДО какого-либо пролога и передаёт его как обычный
// аргумент в маленькую C-функцию.
#include <stddef.h>
#include <vlibc/unistd.h>

// Внешняя функция main
extern int main(int argc, char** argv, char** envp);

// При старте процесса на x86_64 Linux стек выглядит так:
//   rsp -> [argc]
//          [argv[0]]
//          ...
//          [argv[argc-1]]
//          [NULL]
//          [envp[0]]
//          ...
//          [NULL]
//          [auxv...]
void _start_c(long* stack) {
    int argc = (int)stack[0];
    char** argv = (char**)&stack[1];
    char** envp = argv + argc + 1;

    int result = main(argc, argv, envp);

    exit(result);
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "   xor %ebp, %ebp\n"        // обнуляем rbp — конец цепочки стека для отладчиков
    "   mov %rsp, %rdi\n"        // rdi = указатель на argc (первый аргумент _start_c)
    "   and $-16, %rsp\n"        // выравниваем стек на 16 байт под SysV ABI
    "   call _start_c\n"
    "   hlt\n"                   // никогда не должны сюда попасть (_start_c зовёт exit)
);
