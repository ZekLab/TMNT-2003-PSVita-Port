/* setjmp.h matching the Vita newlib: jmp_buf is int[23] (92 bytes, 4-byte aligned). */
#ifndef SO_SETJMP_H
#define SO_SETJMP_H
typedef int jmp_buf[23];
int setjmp(jmp_buf) __attribute__((returns_twice));
void longjmp(jmp_buf, int) __attribute__((noreturn));
#endif
