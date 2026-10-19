// Dumps per-function facts for offline classification:
//   functions.tsv : entry, size, name, callers, callees(list)
//   literals.tsv  : function, instruction addr, literal addr, 32-bit value, float value, string at value (if any)
// Output directory is the first script argument.
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.util.Set;
import java.util.StringJoiner;

public class DumpFacts extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        out.mkdirs();
        Memory mem = currentProgram.getMemory();

        try (PrintWriter fw = new PrintWriter(new File(out, "functions.tsv"), "UTF-8");
             PrintWriter lw = new PrintWriter(new File(out, "literals.tsv"), "UTF-8")) {
            fw.println("entry\tsize\tname\tncallers\tcallees");
            lw.println("func\tinsn\tlit\tvalue\tfloat\tstring");
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                Set<Function> callers = f.getCallingFunctions(monitor);
                Set<Function> callees = f.getCalledFunctions(monitor);
                StringJoiner cj = new StringJoiner(",");
                for (Function c : callees) {
                    cj.add(c.getEntryPoint().toString());
                }
                fw.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName() + "\t"
                        + callers.size() + "\t" + cj);

                InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction insn = it.next();
                    String mnem = insn.getMnemonicString().toLowerCase();
                    if (!mnem.startsWith("mov.l") && !mnem.startsWith("mova") && !mnem.startsWith("fmov")) {
                        continue;
                    }
                    for (Reference r : insn.getReferencesFrom()) {
                        Address lit = r.getToAddress();
                        if (!mem.contains(lit) || !r.getReferenceType().isRead()) {
                            continue;
                        }
                        int v;
                        try {
                            v = mem.getInt(lit);
                        } catch (MemoryAccessException e) {
                            continue;
                        }
                        String s = "";
                        long uv = v & 0xffffffffL;
                        if (uv >= 0x8c010000L && uv < 0x8d000000L) {
                            Address a = toAddr(uv);
                            Data d = getDataAt(a);
                            if (d != null && d.hasStringValue()) {
                                Object o = d.getValue();
                                s = o == null ? "" : o.toString().replace("\t", " ").replace("\n", "\\n").replace("\r", "");
                            }
                        }
                        lw.println(f.getEntryPoint() + "\t" + insn.getAddress() + "\t" + lit + "\t"
                                + String.format("%08x", v) + "\t" + Float.intBitsToFloat(v) + "\t" + s);
                    }
                }
            }
        }
        println("DumpFacts wrote " + out);
    }
}
