// legobatman - hooks for the game's Direct3D busy-waits.
//
// sub_82396360 is one iteration of the D3D spin-wait used while waiting for the GPU to consume the command ring
// (caller sub_823AAAE0 loops on it, re-reading the GPU read pointer at ring object +10896). On the Xbox 360 each
// iteration spins 32x db16cyc; recompiled, that is a tight loop that keeps a whole core busy. On the Switch the
// profile showed the main game thread at 100% of a core here while NVK's shader compiler threads, which the GPU
// is actually waiting for, were short of CPU.
//
// The hook runs the original iteration (so the game's own checks and timeout logic are unchanged) and then sleeps
// briefly on the Switch. GPU progress here is measured in milliseconds, so 100 us adds no meaningful latency
// (cvhd-recomp measured a 100 us sleep per unmet GPU poll as the best setting; nfsmw-nx hooks the same kind of
// wait). Must stay out of tools/scripts/direct_calls.py (codegen.sh passes --keep 0x82396360).

#include <rex/hook.h>
#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REX_EXTERN(__imp__sub_82396360);
REX_HOOK_RAW(sub_82396360) {
  __imp__sub_82396360(ctx, base);
#if REX_PLATFORM_SWITCH
  svcSleepThread(100'000);  // 100 us
#endif
}
