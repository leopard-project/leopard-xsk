// kmod/ime/ime.cpp — OpenXJ380 IME 兼容模块
//
// IME skeleton.
//
// The OpenXJ380 XAPI_IME_* / X3TP personality ABI is not exported while the
// syscall-hook and input-router design is still being finalized. The real IME
// business will be a user-space X3TP consumer (or a provider registered through
// a future narrow ABI), not a module that reaches into X3TP internals.

extern "C"
__attribute__((used)) __attribute__((visibility("default"))) int dlmain(void)
{
    /* OpenXJ380 personality/IME ABI is intentionally not exported yet.
       The real IME/X3TP compatibility layer will be designed with network-kmod. */
    return 0;
}

extern "C"
__attribute__((used)) __attribute__((visibility("default"))) int dlstart(void)
{
    return 0;
}
