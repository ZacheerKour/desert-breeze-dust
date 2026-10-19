# pd-game-v2 automated bring-up loop

Started Thu Oct 15 09:06:28 2026. Each cycle: run (<=420s), harvest failed ICALL targets, seed (gated), regen, rebuild.

## Cycle 1
- run: lines=3924, timed_out=False, game still running after kill=False
- failed ICALL: 1 total, 1 unique ['0xe2fc0']
- CRASH lines: 1
- kernel summary calls: ['232585']
- first crash:
```
[CRASH] Access violation at RIP=0x7FF7B7CD52AE, fault addr=0xB2D15DC4 (read)
  Xbox regs: eax=0x00073DA4 ecx=0x00000000 edx=0x0179BCB8 esp=0x00F7FEF8
  Xbox regs: ebx=0xB2D05DC0 esi=0xB2D05DC0 edi=0x01793860
  Xbox VA of fault: 0xB2D05DC4
```
- tail: `[esp+204 ] 0x0014596A | [esp+212 ] 0x00145B72 | [esp+224 ] 0x00145AA5 | Native stack (first 8 return addrs):`
- seeded: 1 new (total 546)
- regen: [('disasm', 0), ('extend', 0), ('func_id', 0), ('abi', 0), ('lift', 0)]; lifted 12604/12604 functions (failed 0), unresolved stubs 46, untranslated sites 1122; build errors 0

## Cycle 2
- run: lines=5821, timed_out=True, game still running after kill=False
- failed ICALL: 0 total, 0 unique []
- CRASH lines: 0
- kernel summary calls: ['346405']
- tail: `[NV2A] DMA_PUT = 0x001BB4AC  DMA_GET = 0x001BB4AC | [NV2A] DMA_PUT = 0x001BD6A0  DMA_GET = 0x001BD6A0 | [NV2A] DMA_PUT = 0x001BD6F0  DMA_GET = 0x001BD6F0 | [NV2A] DMA_PUT = 0x001BD710  DMA_GET = 0x001BD710`

No failed ICALL targets this cycle; stopping loop (see crash/tail above).

Finished Thu Oct 15 09:21:45 2026.
