; install/blobs.s — GRUB stage blobs for the in-system disk installer.
;
; The actual bytes are generated at build time (tools/gen_install_blobs.sh)
; into vnu_install_blobs.inc and embedded verbatim, declaring the
; begin/end symbols that install.cpp reads.

section .rodata
%include "vnu_install_blobs.inc"
