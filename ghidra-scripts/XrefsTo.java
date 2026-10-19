// Prints every reference to each given address (or address range lo-hi):
// from-address, containing function, reference type.
// Args: addr|lo-hi ...
// @category SC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class XrefsTo extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String arg : getScriptArgs()) {
            String[] p = arg.split("-");
            Address lo = toAddr(p[0]);
            Address hi = p.length > 1 ? toAddr(p[1]) : lo;
            println("== " + arg);
            for (Address a = lo; a.compareTo(hi) <= 0; a = a.add(1)) {
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                while (it.hasNext()) {
                    Reference r = it.next();
                    Function f = getFunctionContaining(r.getFromAddress());
                    println(String.format("  %s <- %s %s %s", a, r.getFromAddress(),
                            f == null ? "?" : f.getName(), r.getReferenceType()));
                }
            }
        }
    }
}
