.section .note.GNU-stack,"",@progbits
.code64
.section .text

#define OFF_EXTRA_ARG   24
#define OFF_CURRENT     32
#define OFF_KSTACK_TOP  32

.global ap_trampoline
ap_trampoline:
    cli

    mov     OFF_EXTRA_ARG(%rdi), %rax
    test    %rax, %rax
    jnz     1f
    jmp     .Lhalt
1:
    mov     OFF_CURRENT(%rax), %rcx
    test    %rcx, %rcx
    jnz     2f
    jmp     .Lhalt
2:
    mov     OFF_KSTACK_TOP(%rcx), %rsp
    sub     $256, %rsp

    mov     %rax, %rdi
    call    ap_init_cpu

.Lhalt:
    cli
    hlt
    jmp     .Lhalt
