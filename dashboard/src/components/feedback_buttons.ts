// Technician feedback on an alert: true alarm / false alarm / unsure.
// Feedback is audited and used for later calibration; it never edits decisions.

import { ApiError, api } from "../api/client";
import { h } from "../util/dom";

export function feedbackButtons(alertId: number, onDone: () => void): HTMLElement {
  const who = h("input", { type: "text", maxlength: 64, placeholder: "initials", "aria-label": "technician",
                           required: true });
  const note = h("input", { type: "text", maxlength: 500, placeholder: "note (optional)", "aria-label": "note" });
  const msg = h("span", { class: "muted", role: "status" });
  const send = async (verdict: string) => {
    if (!who.value.trim()) {
      msg.textContent = "enter your initials first";
      who.focus();
      return;
    }
    msg.textContent = "saving…";
    try {
      await api.post(`/api/alerts/${alertId}/feedback`, { verdict, technician: who.value.trim(),
                                                          note: note.value.trim() });
      msg.textContent = "saved (audited)";
      onDone();
    } catch (e) {
      msg.textContent = e instanceof ApiError ? `failed: ${e.message}` : "failed";
    }
  };
  return h("div", { class: "feedback", "data-testid": "feedback" }, who, note,
           h("button", { class: "btn ok", onclick: () => send("true_alarm") }, "True alarm"),
           h("button", { class: "btn bad", onclick: () => send("false_alarm") }, "False alarm"),
           h("button", { class: "btn", onclick: () => send("unsure") }, "Unsure"), msg);
}
