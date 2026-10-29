-- Retention (archive, then prune): deleting a window makes SQLite look up the
-- decisions that reference it (foreign key diagnostics.window_id).  Without
-- an index that is a full scan of diagnostics per deleted window.

CREATE INDEX IF NOT EXISTS idx_diag_window ON diagnostics (window_id);
