// Prints the listing (address, bytes, instruction or data) for address ranges.
// Args: lo-hi ...
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.CodeUnitIterator;

import java.util.HexFormat;

public class ListingRange extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String arg : getScriptArgs()) {
            String[] p = arg.split("-");
            Address lo = toAddr(p[0]);
            Address hi = toAddr(p[1]);
            println("== " + arg);
            CodeUnitIterator it = currentProgram.getListing().getCodeUnits(
                    currentProgram.getAddressFactory().getAddressSet(lo, hi), true);
            while (it.hasNext()) {
                CodeUnit cu = it.next();
                println(String.format("  %s  %-12s %s", cu.getAddress(),
                        HexFormat.of().formatHex(cu.getBytes()), cu.toString()));
            }
        }
    }
}
