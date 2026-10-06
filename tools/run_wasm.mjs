// Run build/dna.wasm under node's WASI:   node tools/run_wasm.mjs build/dna.wasm prog.exe [args]
//
// With --app the module is a complete program that has the runtime linked into it (a CC# build with --wasm), not the interpreter:
//   node tools/run_wasm.mjs --app NAME.wasm [args]
// Its argv[0] is NAME and the arguments are passed as they are (they are not paths to a program). A NAME.managed.dll beside the module is
// where it finds its managed assembly (CCS_MANAGED_DLL, unless that is set), and corlib.dll is looked for beside that, whatever the current
// directory. A program that ends in abort() (an exception nothing caught, as in .NET) is killed by SIGABRT, as it is natively (exit status 134 in a shell).
// The guest sees the current directory as "." (relative paths work as they do natively) and the host's root as "/"
// (absolute paths do too: the test suite passes some).
//
// This is also the host of the wasm JIT (native/src/WasmJIT.c): the import dna.emit_wasm compiles a module that the runtime made from a
// method's CIL, which imports the runtime's memory, and puts its function "f" in the runtime's indirect function table.
import { WASI } from 'node:wasi';
import { readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { basename, dirname, isAbsolute, join, resolve } from 'node:path';

const argv = process.argv.slice(2);
const appMode = argv[0] === '--app';
if (appMode) argv.shift();
const [wasmPath, ...rawArgs] = argv;
if (!wasmPath) { console.error('usage: node run_wasm.mjs build/dna.wasm prog.exe [args]\n       node run_wasm.mjs --app NAME.wasm [args]'); process.exit(2); }
// wasi-libc resolves a relative path against "." but DNA also builds paths for corlib.dll from it, and with "/" preopened as well that
// goes wrong. So a relative argument that names an existing file is passed as an absolute path.
const args = appMode ? rawArgs : rawArgs.map(a => (!a.startsWith('-') && !isAbsolute(a) && existsSync(a)) ? resolve(a) : a);
const debug = process.env.DNA_WASM_JIT_DEBUG !== undefined;

let env = process.env;
if (appMode) {
  const dll = join(dirname(resolve(wasmPath)), basename(wasmPath).replace(/\.wasm$/, '') + '.managed.dll');
  if (!env.CCS_MANAGED_DLL && existsSync(dll)) env = { ...env, CCS_MANAGED_DLL: dll };
}
const wasi = new WASI({ version: 'preview1', args: [appMode ? basename(wasmPath).replace(/\.wasm$/, '') : 'dna', ...args], env, preopens: { '.': process.cwd(), '/': '/' }, returnOnExit: true });
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
try {
  process.exitCode = wasi.start(instance);
} catch (e) {
  // abort() is a trap in wasm: a program that ends in one ends as it does natively, killed by SIGABRT (status 134 in a shell, -6 for the
  // program that started it).  Everything it printed was written already (the WASI writes are synchronous).
  if (appMode && e instanceof WebAssembly.RuntimeError) {
    process.exitCode = 134;
    try { process.kill(process.pid, 'SIGABRT'); } catch (_) { /* no signals here: the exit status above is what is left */ }
  }
  else throw e;
}
