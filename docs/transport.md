# Audio transport & ring (Phase 3.1 / 3.2)

How samples move between host and card. Recovered from the
`WRITE_REGISTER_BUFFER_ULONG` sites and the `ReadHead out of bounds` guard in
`MOTUAW.sys`. VAs use image base `0x10000`.

## It is PIO into a card aperture, not host bus-master DMA (CONFIRMED)

The vendor driver does **not** hand the card a host ring base. It copies host
audio into a card-side aperture in **window B** with
`WRITE_REGISTER_BUFFER_ULONG`. The push helper is now pinned to **`fn 0x29420`**
(confirmed with `tools/re/xref.py`): it allocs a `'MOTU'`-tagged `0x10000`
bounce buffer, `memset`s it, `memcpy`s ≤`0x10000` bytes of the source, then
feeds the buffer to a stream sub-object (obtained via `this` vtable slot `0x48`)
byte-by-byte through that sub-object's slot `0x8`, and frees the buffer. In
outline:

```
tmp = ExAllocatePool(..., 0x10000) tagged 'MOTU'   // 64 KB bounce buffer
memset(tmp, 0, 0x10000)
len = min(byteLen, 0x10000)
memcpy(tmp, src, len)
dwords = (len >> 2) + 1
dest = base_B + (cardAddr & 0x3fffff)              // window-B aperture slot
WRITE_REGISTER_BUFFER_ULONG(dest, tmp, dwords)     // ds:0x30100
ExFreePoolWithTag(tmp, 'MOTU')                     // ds:0x30104
```

So the transfer granularity is **≤ 64 KB per burst**, dword-aligned, into a
caller-chosen window-B card address (the current write position in the ring).

**Implication for the Linux driver:** the current
`snd_pcm_set_managed_buffer_all` + "hand the card `runtime->dma_addr`" model in
`motu424_hw.c` is wrong for this card. The rewrite (Phase 4.3) needs an indirect
/ copy PCM: on each period, `memcpy_toio`-style push host period data into the
window-B aperture and advance the software write head.

## Ring state machine (CONFIRMED shape — guard at `0x25500`)

The card exposes a hardware position `dmaPoint`; software tracks `readHead`,
`writeHead`, `len`. The bounds-check function (debug string VA `0x2f2e0`,
referenced at `0x255bb`) reads:

- `len`       = ring size in **dword units**, device-ext `[esi-0xa8]`
- `readHead`  = last consumed position,      device-ext `[esi-0x94]`
- `dmaPoint`  = HW play/capture position (function argument)
- `writeHead` = producer position

Logic (paraphrased):

```
writeHead_wrapped = writeHead & (writeHead >= len ? 0 : ~0)   // wrap at len
delta = len - dmaPoint ... newReadHead = ...
if (newReadHead != stored_readHead) {
    if (newReadHead < 0 || newReadHead >= len)
        DbgPrint("ReadHead out of bounds: dmaPoint %08x, readHead %08x, "
                 "writeHead %08x, len %08x", dmaPoint, newReadHead,
                 writeHead, len);
    stored_readHead = newReadHead;
    // notify / advance client (calls at 0x13170, 0x131b0, 0x131e0)
}
```

The `& (>=len ? 0 : ~0)` idiom masks to wrap, which assumes **`len` is a power
of two** in dword units. Positions are in dwords, not bytes.

## Format (from `motu424.h`, unchanged)

Native samples are 24-bit, packed **3 bytes per channel per frame**
(`S24_3LE`/`_3BE`). Confirm endianness on a card. The per-frame channel count
shrinks with the rate family (1x→2x→4x); the concrete mapping is the Phase 3.3
clock/rate work and is still OPEN.

## Open

- Concrete card address of the `dmaPoint` register (the ALSA `.pointer` source).
- Aperture base/size in window B (only the per-burst ≤64 KB and dword units are
  confirmed; the ring `len` and base come from the config path).
- Whether playback and capture use separate apertures/rings or one duplex ring.
