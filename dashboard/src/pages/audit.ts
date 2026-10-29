// Audit: chain status, verification, event list.

import { ApiError, api } from "../api/client";
import { hashChainStatus, type AuditStatus } from "../components/hash_chain_status";
import type { VerificationReport } from "../api/types";
import { shortHash } from "../util/format";
import { fmtTime } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";

interface AuditEventRow { seq: number; ts_ms: number; type: string; node_id: string | null; hash: string;
                          payload: Record<string, unknown> }

export function mountAudit(root: HTMLElement): () => void {
  const status = h("div", {});
  const events = h("section", { class: "card" });
  root.append(h("h2", {}, "Audit trail"), status, events);
  let lastReport: VerificationReport | null = null;
  const load = async () => {
    try {
      const st = await api.get<AuditStatus>("/api/audit/status");
      replaceChildren(status, hashChainStatus(st, (r) => { lastReport = r; void load(); }, lastReport));
      const after = Math.max(0, st.head_seq - 50);
      const ev = (await api.get<{ items: AuditEventRow[] }>("/api/audit/events", { after_seq: after, limit: 50 })).items;
      replaceChildren(events, h("h3", {}, "Latest audit events"),
        h("table", { class: "data" }, h("thead", {}, h("tr", {}, h("th", {}, "#"), h("th", {}, "Time"),
          h("th", {}, "Type"), h("th", {}, "Node"), h("th", {}, "Summary"), h("th", {}, "Hash"))),
          h("tbody", {}, ...ev.reverse().map((e) => h("tr", {}, h("td", {}, String(e.seq)), h("td", {}, fmtTime(e.ts_ms)),
            h("td", {}, e.type), h("td", {}, e.node_id ?? "—"),
            h("td", {}, String(e.payload["outcome"] ?? e.payload["kind"] ?? e.payload["state"] ?? e.payload["verdict"] ?? "")),
            h("td", {}, h("code", {}, shortHash(e.hash, 14))))))));
    } catch (e) {
      replaceChildren(status, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };
  void load();
  return () => undefined;
}
