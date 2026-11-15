// A request on a host-served HIPC session, decoded by hle_sf.c for a service implemented in its own file (hle_audren_srv.c): the command, its raw arguments, handles and
// buffer descriptors in; raw output, copied handles and out objects back. An out object becomes a new host session whose handle is moved to the client (sessions that are
// not domains, as the SDK's clients open them)
#pragma once
#include "hle.h"

typedef struct { uint64_t addr, size; } sf_buf;
typedef struct {
    const char *iface;                       // the session's interface: the service name, or the name its out object was given
    void *obj;                               // the session's host state (from the out object that created it)
    unsigned cmd;
    const uint8_t *in; size_t in_n;          // raw arguments after the CMIF header (a copy: the answer is written over the message buffer)
    uint32_t copy_in[8], move_in[8]; unsigned ncopy_in, nmove_in;
    sf_buf x[8], a[8], b[8]; unsigned nx, na, nb;   // pointer (X), send (A) and receive (B) buffers, in their order in the message
    // the answer
    uint8_t out[0x40]; size_t out_n;         // raw output
    uint32_t copy_out[4]; unsigned ncopy_out;
    struct { const char *iface; void *obj; } objs[2]; unsigned nobjs;
} sf_call;

// hle_audren_srv.c: audren:u and the renderer / device objects it hands out
int dm_audren_served(void);                  // this game's SDK has no in-process renderer, so the host serves audren:u (else it stays unserved, as before)
Result sf_audren(sf_call *c);
void sf_audren_close(const char *iface, void *obj);   // the last session of an object closed
