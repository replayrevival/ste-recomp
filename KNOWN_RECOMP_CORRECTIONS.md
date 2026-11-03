# Known Recomp Corrections

- 0x001A3DB0: memcpy full CFG (phase3f) — internal block / jump-table tail at 0x001A4054 only [validator: memcpy CFG preservation]
- 0x002ACC95;0x002D934E;0x00252C8D;0x00252C98;0x00255B5A;0x00255B65;0x002E0917;0x002E0947: cinit data-pointer-only functions (phase3i) — explicit dispatchable leaf initializers [validator: cinit dispatch + source/boundary checks]
- 0x0024E22D;0x0024CFDC;0x0024D321;0x0024D593: shared epilogue / helper chain (phase3l) — shared epilogue preserved with original stack cleanup [validator: shared epilogue CFG preservation]
- 0x001A4054: memcpy internal block (phase3f) — internal block, not a standalone function [validator: memcpy CFG preservation + no standalone f-entry]
