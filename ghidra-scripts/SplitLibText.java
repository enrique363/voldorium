// Splits the Katana library code (0x8c21af20-0x8c24561f) out of "data" into a read-only
// "libtext" block so the decompiler resolves its literal pools too.
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class SplitLibText extends GhidraScript {
    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        if (mem.getBlock("libtext") != null) {
            return;
        }
        mem.split(mem.getBlock(toAddr(0x8c21af20L)), toAddr(0x8c21af20L));
        mem.split(mem.getBlock(toAddr(0x8c245620L)), toAddr(0x8c245620L));
        MemoryBlock lib = mem.getBlock(toAddr(0x8c21af20L));
        lib.setName("libtext");
        lib.setWrite(false);
        lib.setExecute(true);
        mem.getBlock(toAddr(0x8c245620L)).setName("libdata");
        mem.getBlock(toAddr(0x8c1036e0L)).setExecute(false);
        println("libtext split");
    }
}
