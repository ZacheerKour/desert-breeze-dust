import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.util.regex.*;

/** Args: outDir, then regexes that must ALL match the decompiled C of a function. Writes matches to outDir. */
public class PdScan extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] a = getScriptArgs();
		String outDir = a[0];
		Pattern[] ps = new Pattern[a.length - 1];
		for (int i = 1; i < a.length; i++) ps[i - 1] = Pattern.compile(a[i], Pattern.DOTALL);
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int n = 0, hit = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (monitor.isCancelled()) break;
			if (f.getBody().getNumAddresses() > 6000) continue;
			DecompileResults r = di.decompileFunction(f, 20, monitor);
			n++;
			if (!r.decompileCompleted()) continue;
			String c = r.getDecompiledFunction().getC();
			boolean ok = true;
			for (Pattern p : ps) if (!p.matcher(c).find()) { ok = false; break; }
			if (ok) {
				hit++;
				java.nio.file.Files.writeString(java.nio.file.Path.of(outDir, f.getEntryPoint() + "_" + f.getName() + ".c"), c);
				println("HIT " + f.getEntryPoint() + " " + f.getName() + " len=" + c.length());
			}
		}
		println("SCANNED " + n + " HITS " + hit);
	}
}
