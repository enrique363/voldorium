// Exports every function's name and body address ranges as JSON (for the SH-4 -> C translator).
// Arg: output file path.
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        int n = 0;
        try (PrintWriter w = new PrintWriter(getScriptArgs()[0], "UTF-8")) {
            w.println("[");
            boolean first = true;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                StringBuilder r = new StringBuilder();
                for (AddressRange ar : f.getBody()) {
                    if (r.length() > 0) {
                        r.append(",");
                    }
                    r.append(String.format("[\"%s\",\"%s\"]", ar.getMinAddress(), ar.getMaxAddress()));
                }
                w.printf("%s{\"entry\":\"%s\",\"name\":\"%s\",\"ranges\":[%s]}%n", first ? " " : ",",
                        f.getEntryPoint(), f.getName().replace("\"", "'"), r);
                first = false;
                n++;
            }
            w.println("]");
        }
        println("ExportFunctions: " + n + " functions");
    }
}
