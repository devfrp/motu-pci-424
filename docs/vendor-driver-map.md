# Vendor driver map (Phase 0.3)

Roles of the binaries in `vendor/` (git-ignored, never redistributed). Facts
below come from static RE (`objdump -d -M intel`, `strings`, import tables). All
VAs are with the `MOTUAW.sys` image base `0x10000` (so an IAT thunk at RVA
`0x200xx` is called as `ds:0x300xx`).

## `MOTUAW.sys` — the audio function driver (RE target)

PE32 i386, image base `0x10000`. C++ with source-file strings
`PCI424Driver.cpp`, `PCI424NanoDriver.cpp`, `AW2408.cpp`, `AW24IO.cpp`,
`Awnano.cpp`. This is the only binary that touches register/DMA/FPGA logic.

Everything hardware-facing funnels through **three tiny accessor helpers** that
implement the windowed card-address dispatch (see `register-map.md`):

| Accessor | VA | Import used | Meaning |
|---|---|---|---|
| `WriteRegister(cardAddr, u32)` | `0x29110` | `WRITE_REGISTER_ULONG` (`ds:0x300fc`) | one 32-bit MMIO write |
| `ReadRegister(cardAddr) -> u32` | `0x29160` | `READ_REGISTER_ULONG` (`ds:0x300f8`) | one 32-bit MMIO read |
| `WriteBlock8(sel, src)` | `0x291a0` | `WRITE_REGISTER_ULONG` ×8 | writes 8 dwords to window-A `0x18c0000\|8` (bank0) or `0x1900000\|8` (bank1) |

The compiler also **inlines** the same dispatch at many call sites, so the raw
import counts are: `WRITE_REGISTER_ULONG` 62, `READ_REGISTER_ULONG` 23,
`WRITE_REGISTER_BUFFER_ULONG` 6, `READ_PORT_ULONG` 1, `WRITE_PORT_ULONG` 5.

Notable absence: **no file-I/O imports** (`ZwCreateFile`/`ZwReadFile` are not in
the IAT). The driver therefore does *not* read `altera424b.rbf` from disk
itself — see `fpga-upload.md`. It does import the registry `Zw*` calls
(`ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`) and `IoGetDmaAdapter`.

Interrupt: `IoConnectInterrupt` at `0x2043c` registers the ISR shim at
`0x201f0` with `ServiceContext = this` (the hardware object). The shim forwards
to a **C++ virtual method** (`call [[ctx]+0x28]`). That slot was resolved by
hand: the hardware class vtable is at `.rdata` `0x30cc0`, and slot `0x28` =
`0x2bae0` = the real ISR (see `register-map.md` for the fully decoded IRQ path,
ack, enable and audio-register block). Vtable resolution was done with a small
Python helper over the raw section bytes, since objdump gives no xrefs.

## `MotuBus.sys` — PCI bus/multifunction enumerator

Small (`~25 KB`). Per `MotuBus.inf`, it is the bus/function driver that
enumerates the card's PCI functions and creates the child device(s) that
`MOTUAW.sys` then attaches to. Not audio-relevant beyond enumeration; a Linux
driver binds the PCI function directly and needs no equivalent.

## `MAWWAVE.sys` — legacy WDM/kmixer wave shim

`~70 KB`. The legacy Windows WDM "wave" personality layered on top of the audio
driver (kmixer path). Low RE priority — irrelevant to ALSA. No unique hardware
knowledge expected here.

## `HDExpress_FullImageRun.bin` — PCIe variant FPGA/DSP image

`~1.2 MB` binary shipped for the PCIe "HD Express" (`DEV_0005`) card. Much
larger than a bare ACEX/Cyclone `.rbf`, so it is likely an FPGA bitstream **plus**
a DSP/firmware image ("FullImageRun"). Not yet dissected. The parallel-PCI
PCI-324/424 cards use `altera424b.rbf` instead.
