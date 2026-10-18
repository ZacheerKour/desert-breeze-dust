import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

/** Args: output json path. Exports every function: entry, min, max, number of addresses, and its address ranges. */
public class PdExportFuncs extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String out = getScriptArgs()[0];
		StringBuilder sb = new StringBuilder("[\n");
		boolean first = true;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (!first) sb.append(",\n");
			first = false;
			AddressSetView body = f.getBody();
			sb.append("{\"entry\":").append(f.getEntryPoint().getOffset())
			  .append(",\"min\":").append(body.getMinAddress().getOffset())
			  .append(",\"max\":").append(body.getMaxAddress().getOffset())
			  .append(",\"n\":").append(body.getNumAddresses()).append(",\"ranges\":[");
			boolean fr = true;
			for (AddressRange r : body) {
				if (!fr) sb.append(",");
				fr = false;
				sb.append("[").append(r.getMinAddress().getOffset()).append(",").append(r.getMaxAddress().getOffset()).append("]");
			}
			sb.append("]}");
		}
		sb.append("\n]\n");
		try (FileWriter w = new FileWriter(out)) { w.write(sb.toString()); }
		println("EXPORTED " + currentProgram.getFunctionManager().getFunctionCount() + " functions to " + out);
	}
}
