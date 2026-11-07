# Sound banks: WX V, MIC, SAMP, UBER

Credit: Digitzaki / GZBuildr for the bundle/type and extension handling. This note separates observed containers from unresolved codecs.

- Xbox character bundles expose `.wxv` files beside character assets; staged STE examples include `Gigan.wxv`, `Godzilla2k.wxv`, `MechaGodzilla2.wxv`, and `Rodan.wxv` (observed).
- PS2/Wii inventories expose `.mic` music/voice banks and `.samp`/`.uber` sound-bank pairs; these extensions are present in the extracted source trees (observed).
- GZBuildr maps `mic` to file type 25 and maps `bsf` to type 28 in its extension-to-type inference table (from GZBuildr source); it does not claim a complete codec for the audio payloads.
- `.samp` appears to carry sample-bank data and `.uber` companion metadata/lookup data in the observed inventories (inferred from paired filenames).
- `.mic` is used for music/stream-style banks in PS2/Wii inventories (observed; internal encoding not decoded here).
- `.wxv` is an Xbox-side audio/container asset; the current GZBuildr source does not provide a complete WX V decoder (observed / inferred).
- Sound names and companion-bank pairs are useful for cross-source matching, but byte-level equivalence cannot be assumed across Xbox, PS2, and Wii (inferred).
- No audio files were modified by this documentation pass (observed).
