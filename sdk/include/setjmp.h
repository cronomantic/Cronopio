#ifndef _CVM_SETJMP_H
#define _CVM_SETJMP_H

#include <stdint.h>

/* jmp_buf for CronoVM. The translator lowers setjmp/longjmp to the CVM_OP_SETJMP
 * / CVM_OP_LONGJMP opcodes, which store {resume pc, SP, dest reg} here — see
 * cvm.h. An array type so it decays to a pointer when passed (matching the
 * standard prototypes). 4 words = 16 bytes (3 used + 1 reserved). */
typedef int32_t jmp_buf[4];

/* Returns 0 on the direct call, and the (nonzero) value passed to longjmp when
 * returning via a longjmp. Recognised by name in the translator (returns_twice
 * is implied by clang for `setjmp`). */
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

#endif /* _CVM_SETJMP_H */
