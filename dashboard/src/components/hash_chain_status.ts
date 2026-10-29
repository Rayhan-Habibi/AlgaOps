// Audit chain status with on-demand verification.

import { ApiError, api } from "../api/client";
import type { VerificationReport } from "../api/types";
import { shortHash } from "../util/format";
import { fmtAgo } from "../util/time";
import { h, replaceChildren } from "../util/dom";

export interface AuditStatus {
  head_seq: number;
  head_hash: string;
  event_counts: Record<string, number>;
  checkpoints: { seq: number; hash: string; ts_ms: number }[];
  checkpoint_file: string;
  last_verification: { ok: boolean; head_seq: number; issues: number; ts_ms: number } | null;
  hash_algorithm: string;
}

export function reportView(r: VerificationReport): HTMLElement {
  return h("div", { class: `verify ${r.ok ? "ok" : "bad"}`, "data-testid": "verify-result" },
    h("strong", {}, r.ok ? "CHAIN VERIFIED" : "TAMPERING / INCONSISTENCY DETECTED"),
    h("p", {}, `${r.events} events, head #${r.head_seq} ${shortHash(r.head_hash, 16)} · ` +
      `${r.windows_checked} windows, ${r.decisions_checked} decisions, ${r.checkpoints_checked} checkpoints checked`),
    r.issues.length ? h("ul", { class: "issues" }, ...r.issues.slice(0, 30).map((i) =>
      h("li", {}, h("code", {}, i.kind), ` seq ${i.seq ?? "—"}: ${i.detail}`))) : null,
    ...r.notes.map((n) => h("p", { class: "muted" }, n)),
    h("p", { class: "muted small" }, r.limitation));
}

/** @param last report of the latest on-demand verification, kept by the page so
 *  that refreshing the status after a verification does not hide its details. */
export function hashChainStatus(st: AuditStatus, onVerified?: (r: VerificationReport) => void,
                                last: VerificationReport | null = null): HTMLElement {
  const out = h("div", {}, last ? reportView(last) : null);
  const btn = h("button", { class: "btn primary", "data-testid": "verify-btn" }, "Verify chain now");
  btn.addEventListener("click", async () => {
    btn.setAttribute("disabled", "");
    replaceChildren(out, h("p", { class: "muted" }, "verifying…"));
    try {
      const r = await api.post<VerificationReport>("/api/audit/verify");
      replaceChildren(out, reportView(r));
      onVerified?.(r);
    } catch (e) {
      replaceChildren(out, h("p", { class: "error" }, e instanceof ApiError ? e.message : "failed"));
    } finally {
      btn.removeAttribute("disabled");
    }
  });
  const lv = st.last_verification;
  const cp = st.checkpoints[st.checkpoints.length - 1];
  return h("section", { class: "card", "data-testid": "chain-status" },
    h("h3", {}, "Tamper-evident audit chain"),
    h("dl", { class: "kv" },
      h("dt", {}, "Algorithm"), h("dd", {}, st.hash_algorithm),
      h("dt", {}, "Head"), h("dd", {}, h("code", {}, `#${st.head_seq} ${shortHash(st.head_hash, 24)}`)),
      h("dt", {}, "Last checkpoint"), h("dd", {}, cp ? `#${cp.seq} (${fmtAgo(cp.ts_ms)})` : "none yet"),
      h("dt", {}, "Last verification"),
      h("dd", { class: lv ? (lv.ok ? "ok-text" : "bad-text") : "" },
        lv ? `${lv.ok ? "OK" : "FAILED"} at #${lv.head_seq}, ${fmtAgo(lv.ts_ms)}` : "never")),
    btn, out);
}
