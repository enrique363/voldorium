// Recovers functions Ghidra's auto-analysis skipped (code only reached via pointer tables):
//  1. every run of disassembled instructions not inside a function gets a function at its start
//  2. in undefined gaps of text/libtext, 4-byte aligned SHC prologues ("sts.l pr,@-r15" or
//     "mov.l r14,@-r15") that follow an rts/delay slot or padding are disassembled and made functions
// Repeats until nothing new is found.
// @category SC

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;

public class RecoverFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        Listing listing = currentProgram.getListing();
        int total = 0;
        for (int pass = 0; pass < 6; pass++) {
            int made = 0;
            // 1. orphan instruction runs
            InstructionIterator it = listing.getInstructions(true);
            Address prevEnd = null;
            while (it.hasNext()) {
                Instruction insn = it.next();
                Address a = insn.getAddress();
                boolean contiguous = prevEnd != null && prevEnd.equals(a);
                prevEnd = insn.getMaxAddress().add(1);
                if (getFunctionContaining(a) != null || contiguous && getFunctionContaining(a.subtract(2)) == null) {
                    continue;
                }
                if (!contiguous || getInstructionAt(a.subtract(2)) == null) {
                    if (createFunction(a, null) != null) {
                        made++;
                    }
                }
            }
            // 2. prologues in undefined gaps
            for (String name : new String[] {"text", "libtext"}) {
                MemoryBlock b = currentProgram.getMemory().getBlock(name);
                for (Address a = b.getStart(); a.compareTo(b.getEnd().subtract(4)) < 0; a = a.add(4)) {
                    if (listing.getCodeUnitContaining(a) == null || getInstructionContaining(a) != null
                            || listing.getDefinedDataContaining(a) != null) {
                        continue;
                    }
                    short w = getShort(a);
                    if (w != (short) 0x4f22 && w != (short) 0x2fe6) {
                        continue;
                    }
                    short prev = getShort(a.subtract(2));
                    short prev2 = getShort(a.subtract(4));
                    boolean afterRet = prev2 == (short) 0x000b || prev == (short) 0x0009 || prev == 0;
                    if (!afterRet) {
                        continue;
                    }
                    new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                    Function f = createFunction(a, null);
                    if (f != null) {
                        made++;
                    }
                }
            }
            total += made;
            println("pass " + pass + ": " + made + " functions");
            if (made == 0) {
                break;
            }
        }
        println("RecoverFunctions: " + total + " new, total "
                + currentProgram.getFunctionManager().getFunctionCount());
    }
}
