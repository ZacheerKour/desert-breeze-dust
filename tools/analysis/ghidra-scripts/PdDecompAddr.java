import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.util.*;

/** Args: output dir, depth, then hex function addresses. Decompiles each and its callees up to depth. */
public class PdDecompAddr extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] a = getScriptArgs();
		String outDir = a[0];
		int depth = Integer.parseInt(a[1]);
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		Set<Function> seen = new LinkedHashSet<>();
		Deque<Object[]> q = new ArrayDeque<>();
		for (int i = 2; i < a.length; i++) {
			Address ad = toAddr(a[i]);
			Function f = getFunctionAt(ad);
			if (f == null) {
				disassemble(ad);
				f = createFunction(ad, null);
			}
			if (f == null) { println("NOFUNC " + a[i]); continue; }
			q.add(new Object[] { f, 0 });
		}
		while (!q.isEmpty()) {
			Object[] e = q.poll();
			Function f = (Function) e[0];
			int d = (Integer) e[1];
			if (!seen.add(f)) continue;
			DecompileResults r = di.decompileFunction(f, 60, monitor);
			String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed";
			java.nio.file.Files.writeString(java.nio.file.Path.of(outDir, f.getEntryPoint() + "_" + f.getName() + ".c"), c);
			println("WROTE " + f.getEntryPoint() + " " + f.getName() + " depth=" + d + " len=" + c.length());
			if (d < depth) for (Function g : f.getCalledFunctions(monitor)) if (!g.isThunk() || true) q.add(new Object[] { g, d + 1 });
		}
	}
}
