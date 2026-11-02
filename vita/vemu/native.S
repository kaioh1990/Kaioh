.syntax unified
.arm
.global n_memcpy, n_memmove, n_memset, n_memcmp, n_strcmp
@ all position independent, offsets known from symbol table
n_memcpy:
n_memmove:
    mov r12, r0
    cmp r0, r1
    bhi 2f
    @ forward
1:  subs r2, r2, #1
    ldrbpl r3, [r1], #1
    strbpl r3, [r12], #1
    bpl 1b
    bx lr
2:  add r1, r1, r2
    add r12, r12, r2
3:  subs r2, r2, #1
    ldrbpl r3, [r1, #-1]!
    strbpl r3, [r12, #-1]!
    bpl 3b
    bx lr
n_memset:
    mov r12, r0
1:  subs r2, r2, #1
    strbpl r1, [r12], #1
    bpl 1b
    bx lr
n_memcmp:
    push {r4}
1:  subs r2, r2, #1
    bmi 2f
    ldrb r3, [r0], #1
    ldrb r4, [r1], #1
    subs r3, r3, r4
    beq 1b
    mov r0, r3
    pop {r4}
    bx lr
2:  mov r0, #0
    pop {r4}
    bx lr
n_strcmp:
1:  ldrb r2, [r0], #1
    ldrb r3, [r1], #1
    cmp r2, #1
    cmpcs r2, r3
    beq 1b
    sub r0, r2, r3
    bx lr
.global n_ret0, n_tls
n_ret0:
    mov r0, #0
    bx lr
n_tls:
    and r0, r0, #0xff
    ldr r1, 1f
    add r0, r1, r0, lsl #2
    bx lr
1:  .word 0
