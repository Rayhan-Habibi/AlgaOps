// Export: verifiable evidence bundles and CSV / JSONL history.

import { ApiError, api } from "../api/client";
import { store } from "../state/store";
import { shortHash } from "../util/format";
import { fmtDateTime } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";

interface ExportResult { file: string; url: string; manifest: { chain: { head_seq: number; head_hash: string };
  data_label: string; verification: { ok: boolean }; bundle_digest: string; counts: Record<string, number> } }

export function mountExport(root: HTMLElement): () => void {
  const result = h("div", { role: "status" });
  const files = h("section", { class: "card" });
  const node = h("select", { "aria-label": "node scope" }, h("option", { value: "" }, "all nodes"),
    ...store.get().nodes.map((n) => h("option", { value: n.node_id }, n.node_id)));
  const btn = h("button", { class: "btn primary", "data-testid": "export-btn" }, "Create evidence bundle");
  btn.addEventListener("click", async () => {
    btn.setAttribute("disabled", "");
    replaceChildren(result, stateBox("loading", "building bundle and verifying chain…"));
    try {
      const r = await api.post<ExportResult>("/api/export/evidence", { node_id: node.value || null });
      const m = r.manifest;
      replaceChildren(result, h("div", { class: `verify ${m.verification.ok ? "ok" : "bad"}`, "data-testid": "export-result" },
        h("strong", {}, m.verification.ok ? "Bundle created — chain verified" : "Bundle created — VERIFICATION FAILED"),
        h("p", {}, `${m.data_label} · ${m.counts["windows"]} windows · ${m.counts["diagnostics"]} decisions · `
          + `chain head #${m.chain.head_seq} ${shortHash(m.chain.head_hash, 16)}`),
        h("p", {}, "Bundle digest (Ascon-Hash256): ", h("code", {}, m.bundle_digest)),
        h("a", { class: "btn", href: r.url, download: r.file }, `Download ${r.file}`)));
      void loadFiles();
    } catch (e) {
      replaceChildren(result, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    } finally {
      btn.removeAttribute("disabled");
    }
  });
  const csv = (table: string) => h("a", { class: "btn", href: `/api/export/csv?table=${table}` }, `${table}.csv`);
  root.append(h("h2", {}, "Export"),
    h("section", { class: "card" }, h("h3", {}, "Evidence bundle"),
      h("p", { class: "muted" }, "Zip with the full audit chain, the referenced windows and decisions, checkpoints, a manifest "
        + "and Ascon-Hash256 digests. Verify it on any machine with: python tools/verify_evidence.py <bundle.zip>"),
      h("div", { class: "toolbar" }, h("label", {}, "Scope ", node), btn), result),
    h("section", { class: "card" }, h("h3", {}, "History downloads"),
      h("div", { class: "toolbar" }, csv("windows"), csv("diagnostics"), csv("alerts"),
        h("a", { class: "btn", href: "/api/export/decisions.jsonl" }, "decisions.jsonl"))),
    files);
  const loadFiles = async () => {
    if (node.options.length - 1 !== store.get().nodes.length) {
      const cur = node.value;
      replaceChildren(node, h("option", { value: "" }, "all nodes"),
        ...store.get().nodes.map((n) => h("option", { value: n.node_id, selected: n.node_id === cur }, n.node_id)));
    }
    try {
      const items = (await api.get<{ items: { file: string; bytes: number; modified_ms: number }[] }>("/api/export/files")).items;
      replaceChildren(files, h("h3", {}, "Previous bundles"), !items.length ? stateBox("empty", "none yet")
        : h("ul", {}, ...items.map((f) => h("li", {}, h("a", { href: `/api/export/files/${f.file}`, download: f.file }, f.file),
          ` · ${(f.bytes / 1024).toFixed(1)} KiB · ${fmtDateTime(f.modified_ms)}`))));
    } catch (e) {
      replaceChildren(files, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };
  void loadFiles();
  return () => undefined;
}
