// Decompiles the given functions to <outdir>/<entry>_<name>.c
// Args: outdir addr1 [addr2 ...]   (an arg of the form @file reads one address per line)
// @category SC

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class DecompileList extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        out.mkdirs();
        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        opts.setRespectReadOnly(true);
        di.setOptions(opts);
        di.openProgram(currentProgram);
        List<String> addrs = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                addrs.addAll(Files.readAllLines(Path.of(args[i].substring(1))));
            } else {
                addrs.add(args[i]);
            }
        }
        for (String addr : addrs) {
            if (addr.isBlank()) {
                continue;
            }
            Function f = getFunctionContaining(toAddr(addr.strip()));
            if (f == null) {
                println("no function at " + addr);
                continue;
            }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage();
            try (PrintWriter w = new PrintWriter(new File(out, f.getEntryPoint() + "_" + f.getName() + ".c"), "UTF-8")) {
                w.print(c);
            }
        }
        di.dispose();
    }
}
