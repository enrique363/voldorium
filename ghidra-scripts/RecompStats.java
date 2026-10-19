// Statistics for scoping a static recompiler: per block (text, libtext) mnemonic histogram,
// indirect control flow, FPU mode switches, code coverage, and calls from game code into the
// libraries. Arg: output directory.
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

public class RecompStats extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        out.mkdirs();
        MemoryBlock lib = currentProgram.getMemory().getBlock("libtext");
        try (PrintWriter w = new PrintWriter(new File(out, "recomp_stats.txt"), "UTF-8");
             PrintWriter libCalls = new PrintWriter(new File(out, "lib_calls.tsv"), "UTF-8")) {
            for (String name : new String[] {"text", "libtext"}) {
                MemoryBlock b = currentProgram.getMemory().getBlock(name);
                AddressSet set = new AddressSet(b.getStart(), b.getEnd());
                Map<String, Integer> hist = new TreeMap<>();
                int insns = 0, inFunc = 0, codeBytes = 0;
                int indirectJsr = 0, indirectJmp = 0, braf = 0, bsrf = 0, fschg = 0, frchg = 0, fpscrWrite = 0;
                int pref = 0, trapa = 0, sleep = 0, ldtlb = 0, rte = 0, srWrite = 0;
                InstructionIterator it = currentProgram.getListing().getInstructions(set, true);
                while (it.hasNext()) {
                    Instruction i = it.next();
                    insns++;
                    codeBytes += i.getLength();
                    if (getFunctionContaining(i.getAddress()) != null) {
                        inFunc++;
                    }
                    String m = i.getMnemonicString().toLowerCase().replace("_", "");
                    hist.merge(m, 1, Integer::sum);
                    String txt = i.toString().toLowerCase();
                    if (m.equals("jsr") && i.getReferencesFrom().length == 0) indirectJsr++;
                    if (m.equals("jmp") && i.getReferencesFrom().length == 0) indirectJmp++;
                    if (m.equals("braf")) braf++;
                    if (m.equals("bsrf")) bsrf++;
                    if (m.equals("fschg")) fschg++;
                    if (m.equals("frchg")) frchg++;
                    if ((m.startsWith("lds") && txt.contains("fpscr"))) fpscrWrite++;
                    if (m.equals("pref")) pref++;
                    if (m.equals("trapa")) trapa++;
                    if (m.equals("sleep")) sleep++;
                    if (m.equals("ldtlb")) ldtlb++;
                    if (m.equals("rte")) rte++;
                    if (m.startsWith("ldc") && txt.contains(",sr")) srWrite++;
                    // game -> library calls
                    if (name.equals("text") && (m.equals("jsr") || m.equals("jmp") || m.equals("bsr") || m.equals("bra"))) {
                        for (Reference r : i.getReferencesFrom()) {
                            if (r.getReferenceType().isFlow() && lib.contains(r.getToAddress())) {
                                Function callee = getFunctionAt(r.getToAddress());
                                libCalls.println(i.getAddress() + "\t" + r.getToAddress() + "\t"
                                        + (callee == null ? "?" : callee.getName()));
                            }
                        }
                    }
                }
                long size = b.getSize();
                w.printf("== %s %s-%s size=%d bytes%n", name, b.getStart(), b.getEnd(), size);
                w.printf("instructions=%d (%d bytes, %.1f%% of block), in functions=%d%n", insns, codeBytes,
                        100.0 * codeBytes / size, inFunc);
                w.printf("indirect jsr=%d jmp=%d braf=%d bsrf=%d%n", indirectJsr, indirectJmp, braf, bsrf);
                w.printf("fschg=%d frchg=%d lds->fpscr=%d ldc->sr=%d pref=%d trapa=%d sleep=%d ldtlb=%d rte=%d%n",
                        fschg, frchg, fpscrWrite, srWrite, pref, trapa, sleep, ldtlb, rte);
                w.printf("distinct mnemonics=%d%n", hist.size());
                for (Map.Entry<String, Integer> e : hist.entrySet()) {
                    w.printf("  %-10s %d%n", e.getKey(), e.getValue());
                }
            }
        }
        println("RecompStats written to " + out);
    }
}
