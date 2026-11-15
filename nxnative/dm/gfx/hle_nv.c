// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "../host/hle.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;

#define WHY "NVN has been replaced wholesale with a host null device, so the driver glue layer has nothing to do"
// SetGraphicsAllocator does more than hand NVN its allocator: it also points NVIDIA's own allocator (NvOsAlloc, the "nv core" NvOsHeapAllocator) at the game's functions, and
// NvOsAlloc serves more than the driver. The SDK's media stack (stagefright: android::HeapObject::operator new is a jump to NvOsAlloc) allocates its messages through it. Without the
// game's allocator that heap is a 16 KB block in the module's bss that grows by nn::os::AllocateMemoryBlock(2 MB), and a game whose nninitStartup hands the whole memory heap to its
// malloc (SetMemoryHeapSize(total - used), then AllocateMemoryBlock of all of it) has nothing left to grow into.
// Disgaea 5 (SDK 1.7.1, NVIDIA's libraries in subsdk0, the movie library's stagefright in subsdk4): its opening movie's extractor (NuCachedSource2::onFetch, subsdk4+0x277ab4) asks
// HeapObject::operator new for a 0xa20-byte AMessage, got null once a few messages were alive at once, and the AMessage constructor wrote through it (RefBase::RefBase, subsdk0+0x73b80),
// about 12 s after boot or as soon as A skipped the logo; with fewer messages alive (timing) the 16 KB sufficed and the title came up. On the Switch the game's call
// (main+0x1e4368, aligned_alloc / free / realloc over its StandardAllocator) reroutes that heap, so here the SDK's own SetGraphicsAllocator (subsdk0+0xc700: it records the functions
// and swaps the heap's four entries for forwarders) runs too. Only SDKs before 4: every game the host has run on SDK 4 or later boots with the heap left as it is (Engage's sdk has
// the same NvOs heap at sdk+0x3e4e40 with a 32 KB block), so their behaviour does not change
static int nv_forward_alloc(void) { int m = dm_sdk_major(); return m > 0 && m < 4; }
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_alloc (SDK 1-3: also passed to the SDK's own, which points NvOsAlloc at it)",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) {
    g_nv_alloc = (struct nv_allocator){a, f, r, u};
    if (!nv_forward_alloc()) return;
    void (*orig)(nv_alloc_fn, nv_free_fn, nv_realloc_fn, void *) = (void (*)(nv_alloc_fn, nv_free_fn, nv_realloc_fn, void *))guest_sym("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_");
    if (!orig) { LOG("[RED] nv: SDK %d has no nv::SetGraphicsAllocator of its own; NvOsAlloc keeps its own heap", dm_sdk_major()); return; }
    module_t *m = module_of((void *)orig);
    LOG("nv: SetGraphicsAllocator also passed to the SDK's own (%s+%#lx): NvOsAlloc (stagefright's HeapObject) takes the game's allocator, as on the Switch (SDK %d)",
        m ? m->name : "?", m ? (unsigned long)((uint8_t *)orig - m->base) : 0ul, dm_sdk_major());
    orig(a, f, r, u);
}
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY, void, h_nv_Init, void *mem, unsigned long size) { (void)mem; (void)size; }
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) {}
