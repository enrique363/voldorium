// Prints a short summary of the analyzed program: blocks, function count,
// largest functions and printable strings that look like Katana SDK markers.
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;

import java.util.ArrayList;
import java.util.List;

public class SummarizeProgram extends GhidraScript {
    @Override
    protected void run() throws Exception {
        println("== Memory blocks");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(String.format("  %-12s %s-%s %8x", b.getName(), b.getStart(), b.getEnd(), b.getSize()));
        }

        List<Function> fns = new ArrayList<>();
        currentProgram.getFunctionManager().getFunctions(true).forEach(fns::add);
        println("== Functions: " + fns.size());
        fns.sort((a, b) -> Long.compare(b.getBody().getNumAddresses(), a.getBody().getNumAddresses()));
        for (int i = 0; i < Math.min(15, fns.size()); i++) {
            Function f = fns.get(i);
            println(String.format("  %s %6d bytes  %s", f.getEntryPoint(), f.getBody().getNumAddresses(), f.getName()));
        }

        int count = 0;
        println("== Strings (SDK / version markers)");
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (!d.hasStringValue()) {
                continue;
            }
            count++;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null) {
                continue;
            }
            String l = s.toLowerCase();
            if (l.contains("ver") || l.contains("lib") || l.contains("kamui") || l.contains("ninja")
                    || l.contains("shinobi") || l.contains("sg_") || l.contains("(c)") || l.contains("copyright")) {
                println(String.format("  %s  %s", d.getAddress(), s.replace("\n", "\\n")));
            }
        }
        println("== Defined strings: " + count);
    }
}
