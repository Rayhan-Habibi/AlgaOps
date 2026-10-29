// Response shapes of the gateway API (gateway/algaops_gateway/api/*).

export type Outcome = "NORMAL" | "CHECK_SENSOR" | "CHECK_EQUIPMENT" | "NOT_EVALUATED";
export type Mode = "UNKNOWN" | "OFF" | "STARTUP" | "STEADY";
export type DataSource = "hardware" | "simulator" | "replay" | "unknown";

export interface MethodResult {
  status: "skipped" | "ok" | "unavailable";
  anomalous?: boolean;
  score?: number | null;
  threshold?: number | null;
  dims?: number;
  top_feature?: string | null;
  feature_scores?: Record<string, number | null>;
}

export interface Decision {
  id?: number;
  window_id?: number | null;
  audit_seq?: number | null;
  schema: string;
  kind: "window" | "stream_timeout" | "malformed";
  node_id: string;
  data_source: DataSource;
  boot_id: number;
  seq: number;
  timestamp_ms: number;
  received_ms: number;
  decided_ms: number;
  trust: {
    score: number;
    trusted: boolean;
    flag_bits: number;
    flags: string[];
    blocking: boolean;
    explanation: string;
  };
  outcome: Outcome;
  mode: Mode;
  mode_changed: boolean;
  time_in_mode_ms: number;
  method: string | null;
  decided_by?: string | null;
  baseline_id: number | null;
  features: Record<string, number | null>;
  methods: Record<string, MethodResult>;
  alert: { event: string; kind: Outcome | null; prev_kind: Outcome | null; consecutive: number };
  stream: { missing_count: number; completeness: number | null };
  reason: string;
  policy_hash: string;
  engine_version: string;
  window_digest: string | null;
}

export interface PacketCounts {
  valid: number;
  late_stale: number;
  duplicate: number;
  out_of_order: number;
  invalid: number;
  lost: number;
  duplicate_delivery: number;
  malformed: number;
}

export interface Alert {
  id: number;
  node_id: string;
  kind: Outcome;
  status: "open" | "cleared" | "superseded";
  opened_ms: number;
  closed_ms: number | null;
  open_diag_id: number | null;
  close_diag_id: number | null;
  summary: string;
  last_reason: string;
  last_verdict?: string | null;
}

export interface NodeSummary {
  node_id: string;
  data_source: DataSource;
  fw_version: string | null;
  last_boot_id: number | null;
  first_seen_ms: number;
  last_seen_ms: number;
  liveness: "live" | "late" | "stale" | "never";
  connection: "online" | "offline" | null;
  status: Record<string, unknown> | null;
  window_ms: number | null;
  completeness_10min: number | null;
  latest: Decision | null;
  open_alert: Alert | null;
  baselines: { id: number; mode: string; n: number; activated_ms: number | null }[];
  packets_1h: PacketCounts;
}

export interface GatewayStatus {
  gateway_id: string;
  site: string;
  version: string;
  engine_version: string;
  policy: { name: string | null; version: string | null; hash: string };
  started_ms: number;
  now_ms: number;
  uptime_s: number | null;
  read_only: boolean;
  mqtt: { state: string; host: string; port: number; connects: number; disconnects: number;
          last_error: string | null } | null;
  pipeline: {
    received: number; accepted: number; duplicate_deliveries: number; rejected: number;
    stream_timeouts: number; storage_errors: number; queue_full: number;
    latency_ms: { last: number | null; p50: number | null; p95: number | null; max: number | null;
                  samples: number };
  };
  audit: {
    head_seq: number; head_hash: string;
    last_checkpoint: { seq: number; hash: string; ts_ms: number } | null;
    last_verification: { ok: boolean; head_seq: number; issues: number; ts_ms?: number } | null;
  };
  data: { label: string; sources: Record<string, number> };
  nodes: number;
}

export interface TrendPoint { t: number; min: number | null; max: number | null; mean: number | null; n: number }
export interface Trend {
  node_id: string;
  metric: string;
  since_ms: number;
  until_ms: number;
  bucket_ms: number;
  points: TrendPoint[];
  modes: { mode: Mode; since: number; until: number }[];
  markers: { outcome: Outcome; since: number; until: number }[];
}

export interface VerificationIssue { kind: string; seq: number | null; detail: string }
export interface VerificationReport {
  ok: boolean;
  events: number;
  head_seq: number;
  head_hash: string;
  windows_checked: number;
  decisions_checked: number;
  checkpoints_checked: number;
  notes: string[];
  issues: VerificationIssue[];
  limitation: string;
  recorded_as_seq?: number;
}

export interface Baseline {
  id: number;
  node_id: string;
  mode: string;
  status: "candidate" | "active" | "retired";
  created_ms: number;
  activated_ms: number | null;
  n: number;
  stats: { features: string[]; mean: Record<string, number>; std: Record<string, number> };
  source: Record<string, unknown>;
}

export interface CaptureSession {
  node_id: string; mode: string; target: number; accepted: number;
  skipped_untrusted: number; skipped_other_mode: number; started_ms: number;
}

export interface ScenarioInfo { name: string; description: string; windows: number; duration_s: number; node_id: string }

export interface ReplayJob {
  id: number; kind: "offline" | "live"; scenario: string; state: string; progress: number;
  error?: string; sent?: number; total?: number; result?: ScenarioResult;
}

export interface ConfusionDict {
  tp: number; fp: number; tn: number; fn: number;
  precision: number | null; recall: number | null; fpr: number | null; f1: number | null;
  support: number; negatives: number; windows?: number;
}

export interface ScenarioResult {
  name: string;
  description: string;
  messages: number;
  outcome_counts: Record<string, number>;
  truth_vs_outcome: Record<string, Record<string, number>>;
  expected_match_rate: number | null;
  trust_detection: ConfusionDict;
  equipment_detection: ConfusionDict;
  methods_steady: Record<string, ConfusionDict>;
  attribution_errors: number;
  untrusted_check_equipment: number;
  fault_events: { fault: string; truth: string; messages: number; detected: boolean;
                  latency_s: number | null; latency_messages: number | null }[];
  stream_timeouts: number;
  alerts_opened: Record<string, number>;
  duration_s: number;
  timeline?: { t_ms: number; window: number | null; truth: string; fault: string | null;
               outcome: string; flags?: string[]; mode?: string; reason?: string }[];
}

export interface EvaluationReport {
  generated_ms: number;
  engine_version: string;
  policy_hash: string;
  disclaimer: string;
  scenarios: ScenarioResult[];
  overall: {
    trust_detection: ConfusionDict;
    equipment_detection: ConfusionDict;
    methods_steady: Record<string, ConfusionDict>;
    attribution_errors: number;
    untrusted_check_equipment: number;
  };
  wall_time_s: number;
}

export interface MethodComparison {
  windows: Record<string, number>;
  per_mode: Record<string, Record<string, { evaluated: number; unavailable: number;
                                            flagged: number; flag_rate: number | null }>>;
  agreement: Record<string, number>;
  note: string;
}
