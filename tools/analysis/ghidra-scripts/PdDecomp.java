import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.mem.*;
import java.util.*;

/** Args: output dir, then ASCII string literals. Decompiles every function that references each string. */
public class PdDecomp extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] a = getScriptArgs();
		String outDir = a[0];
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		Memory mem = currentProgram.getMemory();
		for (int k = 1; k < a.length; k++) {
			String s = a[k];
			Set<Function> funcs = new LinkedHashSet<>();
			Address hit = mem.findBytes(currentProgram.getMinAddress(), (s + "\0").getBytes("US-ASCII"), null, true, monitor);
			while (hit != null) {
				println("STR '" + s + "' at " + hit);
				for (Reference r : getReferencesTo(hit)) {
					Function f = getFunctionContaining(r.getFromAddress());
					if (f != null) funcs.add(f);
					println("  xref from " + r.getFromAddress() + " in " + (f == null ? "none" : f.getName()));
				}
				Address next = hit.add(1);
				hit = mem.findBytes(next, (s + "\0").getBytes("US-ASCII"), null, true, monitor);
				if (hit != null && hit.equals(next)) break;
			}
			for (Function f : funcs) {
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed";
				java.nio.file.Files.writeString(java.nio.file.Path.of(outDir, "str_" + k + "_" + f.getName() + ".c"), "// via string '" + s + "'\n" + c);
				println("WROTE " + f.getName() + " " + c.length());
			}
		}
	}
}
