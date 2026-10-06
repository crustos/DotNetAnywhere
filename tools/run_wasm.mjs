// Run build/dna.wasm under node's WASI:   node tools/run_wasm.mjs build/dna.wasm prog.exe [args]
// The guest sees the current directory as "." (relative paths work as they do natively) and the host's root as "/"
// (absolute paths do too: the test suite passes some).
//
// This is also the host of the wasm JIT (native/src/WasmJIT.c): the import dna.emit_wasm compiles a module that the runtime made from a
// method's CIL, which imports the runtime's memory, and puts its function "f" in the runtime's indirect function table.
import { WASI } from 'node:wasi';
import { readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { isAbsolute, resolve } from 'node:path';

const [, , wasmPath, ...rawArgs] = process.argv;
if (!wasmPath) { console.error('usage: node run_wasm.mjs build/dna.wasm prog.exe [args]'); process.exit(2); }
// wasi-libc resolves a relative path against "." but DNA also builds paths for corlib.dll from it, and with "/" preopened as well that
// goes wrong. So a relative argument that names an existing file is passed as an absolute path.
const args = rawArgs.map(a => (!a.startsWith('-') && !isAbsolute(a) && existsSync(a)) ? resolve(a) : a);
const debug = process.env.DNA_WASM_JIT_DEBUG !== undefined;

const wasi = new WASI({ version: 'preview1', args: ['dna', ...args], env: process.env, preopens: { '.': process.cwd(), '/': '/' }, returnOnExit: true });
let instance;
const imports = {
  ...wasi.getImportObject(),
  dna: {
    emit_wasm(ptr, len) {
      try {
        const bytes = new Uint8Array(instance.exports.memory.buffer, ptr, len).slice();   // (a copy: the memory may grow while it compiles)
        const module = new WebAssembly.Module(bytes);
        const inst = new WebAssembly.Instance(module, { env: { memory: instance.exports.memory, table: instance.exports.__indirect_function_table } });
        const table = instance.exports.__indirect_function_table;
        const index = table.length;
        table.grow(1);
        table.set(index, inst.exports.f);
        return index;
      } catch (e) {
        if (debug) console.error('emit_wasm failed: ' + e.message);
        return -1;
      }
    },
  },
};
const wasmBytes = await readFile(wasmPath);
// a module built with --no-wasm-jit has no such import and does not mind having it offered
({ instance } = await WebAssembly.instantiate(wasmBytes, imports));
process.exitCode = wasi.start(instance);
