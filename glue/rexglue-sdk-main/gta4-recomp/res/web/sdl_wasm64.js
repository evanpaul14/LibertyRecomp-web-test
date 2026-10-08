// Pre-js for SDL3 on wasm64. SDL's own CPtrToHeap32Index (src/SDL.c) divides
// by 4n, but EM_ASM passes pointers as Numbers, so every audio callback threw
// "Cannot mix BigInt and other types" and Chrome played nothing. SDL keeps a
// definition made before it initializes; this one takes either type.
Module['SDL3'] = Module['SDL3'] || {};
Module['SDL3'].CPtrToHeap32Index = (ptr) => Math.floor(Number(ptr) / 4);
