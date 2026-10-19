import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.*;
import java.util.*;

public class PdStats extends GhidraScript {
	@Override
	protected void run() throws Exception {
		FunctionManager fm = currentProgram.getFunctionManager();
		int total = 0, named = 0, thunks = 0, external = 0;
		for (Function f : fm.getFunctions(true)) {
			total++;
			if (!f.getName().startsWith("FUN_")) named++;
			if (f.isThunk()) thunks++;
		}
		println("STATS functions=" + total + " named=" + named + " thunks=" + thunks);
		int strings = 0;
		for (Data d : currentProgram.getListing().getDefinedData(true)) {
			if (d.hasStringValue()) strings++;
		}
		println("STATS defined_strings=" + strings);
		ExternalManager em = currentProgram.getExternalManager();
		int imports = 0;
		for (String lib : em.getExternalLibraryNames()) {
			int n = 0;
			ExternalLocationIterator it = em.getExternalLocations(lib);
			while (it.hasNext()) { it.next(); n++; }
			println("STATS import_lib " + lib + " = " + n);
			imports += n;
		}
		println("STATS total_imports=" + imports);
		// named (non FUN_) function sample, grouped by prefix
		Map<String, Integer> prefixes = new TreeMap<>();
		for (Function f : fm.getFunctions(true)) {
			String n = f.getName();
			if (n.startsWith("FUN_") || n.startsWith("thunk_")) continue;
			int i = n.indexOf('_');
			String p = i > 0 ? n.substring(0, i) : (n.length() > 4 ? n.substring(0, 4) : n);
			prefixes.merge(p, 1, Integer::sum);
		}
		println("STATS name_prefixes=" + prefixes);
		for (MemoryBlock b : currentProgram.getMemory().getBlocks())
			println("STATS block " + b.getName() + " " + b.getStart() + " size=" + b.getSize());
		Address e = currentProgram.getSymbolTable().getExternalEntryPointIterator().hasNext()
			? currentProgram.getSymbolTable().getExternalEntryPointIterator().next() : null;
		println("STATS entry=" + e);
	}
}
