// One-time program setup so the decompiler resolves literal-pool constants:
//  - splits "ram" into text (read-only, exec) up to the ROM-data start, and data after it
//  - pins FPSCR_SZ=0 / FPSCR_PR=0 (Hitachi SHC defaults at function entry) across text
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.math.BigInteger;

public class FixupProgram extends GhidraScript {
    // End of game code; initialized-data ROM image starts at 0x8c1036e0 (from _INITSCT tables).
    static final long TEXT_END = 0x8c1036e0L;

    @Override
    protected void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock ram = mem.getBlock("ram");
        if (ram != null) {
            mem.split(ram, toAddr(TEXT_END));
            MemoryBlock text = mem.getBlock(toAddr(0x8c010000L));
            text.setName("text");
            text.setWrite(false);
            text.setExecute(true);
            MemoryBlock data = mem.getBlock(toAddr(TEXT_END));
            data.setName("data");
            // Katana libraries sit inside "data" (0x8c21af20-0x8c245612); keep them executable.
            data.setExecute(true);
            println("split ram into text/data");
        }
        ProgramContext ctx = currentProgram.getProgramContext();
        Address lo = toAddr(0x8c010000L);
        Address hi = toAddr(0x8c245612L);
        for (String r : new String[] {"FPSCR_SZ", "FPSCR_PR"}) {
            Register reg = currentProgram.getRegister(r);
            ctx.setValue(reg, lo, hi, BigInteger.ZERO);
        }
        println("FPSCR_SZ/PR pinned to 0");
    }
}
