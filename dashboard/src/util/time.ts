// Time helpers (local time zone of the technician's browser).

export function fmtTime(ms: number | null | undefined): string {
  if (!ms) return "—";
  const d = new Date(ms);
  return d.toLocaleTimeString(undefined, { hour12: false });
}

export function fmtDateTime(ms: number | null | undefined): string {
  if (!ms) return "—";
  const d = new Date(ms);
  return `${d.toLocaleDateString()} ${d.toLocaleTimeString(undefined, { hour12: false })}`;
}

export function fmtDuration(ms: number | null | undefined): string {
  if (ms === null || ms === undefined || !Number.isFinite(ms)) return "—";
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ${s % 60} s`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h} h ${m % 60} min`;
  return `${Math.floor(h / 24)} d ${h % 24} h`;
}

export function fmtAgo(ms: number | null | undefined, now = Date.now()): string {
  if (!ms) return "never";
  const d = now - ms;
  if (d < 0) return "in the future";
  if (d < 2000) return "just now";
  return `${fmtDuration(d)} ago`;
}
