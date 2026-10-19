// Applies function names from a TSV (addr<TAB>name[<TAB>comment]); '#' lines are ignored.
// Names starting with g_ become data labels. Otherwise disassembles and creates the function first if Ghidra has none at that address.
// Arg: path to the TSV.
// @category SC

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.nio.file.Files;
import java.nio.file.Path;

public class ApplyNames extends GhidraScript {
    @Override
    protected void run() throws Exception {
        int named = 0;
        int created = 0;
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            String[] p = line.split("\t");
            Address a = toAddr(p[0]);
            if (p[1].startsWith("g_")) {
                // data label, not a function
                createLabel(a, p[1], true, SourceType.USER_DEFINED);
                if (p.length > 2 && !p[2].isEmpty()) {
                    currentProgram.getListing().setComment(a, CodeUnit.PRE_COMMENT, p[2]);
                }
                named++;
                continue;
            }
            Function f = getFunctionAt(a);
            if (f == null) {
                if (getInstructionAt(a) == null) {
                    new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                }
                f = createFunction(a, null);
                if (f == null) {
                    println("could not create function at " + a);
                    continue;
                }
                created++;
            }
            f.setName(p[1], SourceType.USER_DEFINED);
            if (p.length > 2 && !p[2].isEmpty()) {
                currentProgram.getListing().setComment(a, CodeUnit.PLATE_COMMENT, p[2]);
            }
            named++;
        }
        println("ApplyNames: named " + named + ", created " + created);
    }
}
