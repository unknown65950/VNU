# VNU (Vibe's Not UNIX!) — kernel skeleton

Минимальный, но по-настоящему рабочий скелет ядра: Multiboot2-заголовок,
загрузка через GRUB, переход в 32-битный protected mode и код ядра на
freestanding C++20, который печатает приветствие в VGA text mode и в
serial-порт (COM1).

Это **проверено вживую** — собрано и загружено в QEMU в процессе
подготовки этого скелета:

```
VNU (Vibe's Not UNIX!) booted.
multiboot2 magic OK
kernel is alive. halting.
```

## Структура

```
vnu/
├── kernel/
│   ├── CMakeLists.txt          # сборка kernel.elf
│   ├── linker.ld               # размещение ядра по адресу 1 MiB
│   ├── arch/i386/boot/boot.s   # Multiboot2-заголовок + _start (NASM)
│   └── kernel/kernel.cpp       # kernel_main: VGA + serial вывод
├── iso/boot/grub/grub.cfg      # конфиг GRUB для ISO
├── build_iso.sh                # cmake build + grub-mkrescue -> vnu.iso
├── run.sh                      # запуск vnu.iso в QEMU
└── README.md
```

## Зависимости (Ubuntu/Debian)

```bash
apt-get install -y build-essential cmake nasm \
    qemu-system-x86 grub-pc-bin grub-common xorriso mtools \
    gcc-multilib g++-multilib
```

`gcc-multilib`/`g++-multilib` нужны, чтобы обычный хостовый g++ на
64-битной машине умел собирать 32-битный код (`-m32`). Это **не**
настоящий кросс-компилятор — для x86-ядра на x86-хосте этого достаточно,
пока код ядра не трогает ничего из host libc/ABI (см. предупреждение в
`kernel/CMakeLists.txt`).

## Сборка и запуск

```bash
./build_iso.sh        # соберёт kernel.elf и упакует в vnu.iso
./run.sh               # запустит в QEMU с окном
./run.sh --headless    # запустит без графики, вывод через serial в терминал
```

Ctrl+C или закрытие окна QEMU останавливает эмуляцию (ядро уходит в `hlt`
сразу после вывода приветствия — планировщика и обработки прерываний
пока нет).

## Как это работает

1. GRUB находит Multiboot2-заголовок в `boot.s` (магическое число,
   контрольная сумма, теги), переводит CPU в 32-битный protected mode
   и прыгает на `_start` с `eax` = multiboot2 magic, `ebx` = указатель
   на multiboot info.
2. `_start` ставит стек и зовёт `kernel_main(magic, mbi)`.
3. `kernel_main` инициализирует serial (UART 16550 на 0x3F8) и пишет
   текст напрямую в VGA text buffer по адресу `0xB8000` — никакого
   драйвера экрана, никакой libc, только memory-mapped I/O и `outb`/`inb`.

## Дорожная карта (куда двигаться дальше)

Разумный порядок наращивания — GDT/IDT сильно проще делать сразу после
скелета, пока код маленький и всё под контролем:

1. **GDT** — своя глобальная таблица дескрипторов вместо той, что
   поставил GRUB (нужна для дальнейшего перехода в user mode).
2. **IDT + обработчики исключений** — хотя бы page fault, GPF, double
   fault с внятным сообщением вместо мгновенного reboot/triple fault.
3. **PIC/PIT** — разрешить и обработать первое настоящее прерывание
   (таймер), убедиться что `EOI` отправляется правильно.
4. **Клавиатура через IRQ1** — первый ввод с реального железа/эмулятора.
5. **Physical memory manager** (bitmap/free-list по данным Multiboot2
   memory map) → **paging** → `kmalloc`.
6. **Процессы**: переключение контекста, простой round-robin
   планировщик, переход Ring 0 → Ring 3, syscalls.
7. Только после этого имеет смысл портировать `vibecoreutils` —
   `std::filesystem`/`std::thread` из текущей версии потребуют
   реализовать под них системные вызовы (или проще: на первое время
   переписать утилиты на голые syscalls типа `open`/`read`/`write`
   без STL-тяжеловесов, будет проще натянуть на своё ядро).

Каждый из этих шагов хорошо документирован на wiki.osdev.org — начать
стоит со страниц "GDT Tutorial", "Interrupts Tutorial" и "Meaty Skeleton".

## POSIX v0.1 + v0.2 foundation
VNU now reserves a POSIX-oriented userspace boundary. ABI v1 (source of truth: `kernel/include/vnu/abi.h`, mirrored in `vlibc` and `abi/ABI.md`) currently exposes:

| #  | call   |
|----|--------|
| 0  | read   |
| 1  | write  |
| 2  | open   |
| 3  | close  |
| 4  | exit   |
| 5  | lseek  |
| 6  | stat   |
| 7  | fstat  |
| 8  | brk    |
| 9  | getpid |
| 10 | chdir  |
| 11 | getcwd |

Unsupported calls return `-VNU_ENOSYS`. Numbers are append-only.

The kernel contains an initial VFS/file-descriptor table with stdin/stdout/stderr reserved and dynamic descriptors starting at 3. `initialD` is the designated PID 1 init system. Its boot scripts live under `/etc/initialD/`; execution will be enabled with directory traversal and process syscalls.

## Сборка
```bash
cd vnu          # или куда вы кладёте дерево
rm -rf kernel/build
./build_iso.sh
./run.sh        # или ./run.sh --headless
```

## Host toolchain (vcc / vld)

From the repository root (parent of `tools/` and `vlibc/`):

```bash
./tools/vcc vnu/userspace/examples/hello/hello.c -o hello
file hello   # ELF 32-bit LSB executable
```

Inside the native console the names are reserved:

```
VNU> help
VNU> vcc
VNU> vld
VNU> run /bin/hello
```

Full in-OS compile/run waits on process support and an ELF loader.

## Unified system (kernel + vlibc userspace)

Boot no longer stays in the native console by default. After GDT/IDT init the
kernel starts **`/bin/vash`** (embedded ELF built with `vcc`/`vld`).

```bash
./tools/build_userspace.sh   # regenerates embedded_*.h
cd vnu && ./build_iso.sh && ./run.sh
```

Inside `vash`: `help`, `echo`, `ls`, `hello`, `run /bin/hello`, `exit`.

## Мануалы

У каждой команды системы (builtins vash, отдельные coreutils-бинарники,
самостоятельные программы) есть справочная страница. Просмотр — через `man`:

```bash
man              # список всех документированных команд
man ls           # страница команды ls
man vash man su  # несколько страниц сразу
```

Правило обязательности мануалов для новых команд описано в `AGENTS.md`
в корне репозитория (и продублировано в `vnu/userspace/man/man.c`).
