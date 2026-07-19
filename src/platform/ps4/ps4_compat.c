#include <setjmp.h>

/* The OpenOrbis SDK in the pinned toolchain exposes _setjmp/_longjmp in
 * libc_setjmp.so but does not provide setjmp/longjmp symbols. mGBA and
 * libpng rely on the standard names, so provide thin wrappers here.
 */
int setjmp(jmp_buf env) {
	return _setjmp(env);
}

_Noreturn void longjmp(jmp_buf env, int val) {
	_longjmp(env, val);
}
