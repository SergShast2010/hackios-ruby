/* Bare EL1 harness entry. Only sets SP and calls C; no sysreg hooks into the PE. */
.section .text.boot,"ax"
.global _start
_start:
    ldr x0, =0x40400000
    mov sp, x0
    bl harness_main
1:  wfe
    b 1b
