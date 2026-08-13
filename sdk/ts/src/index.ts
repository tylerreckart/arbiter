// @arbiter/sdk — Intent classify + reconcile client (no runtime deps).

export type IntentClassifyRequest = {
  message: string;
  requestedAgent?: string;
  mode?: "off" | "heuristic" | "hybrid" | "llm";
  applyRouting?: boolean;
};

export type IntentClassifyResult = {
  kind: string;
  confidence: number;
  source: string;
  target_agent: string;
  brief: string;
  applied: boolean;
  llm_used?: boolean;
  malformed?: boolean;
};

export type ReconcileWorkspace =
  | { kind: "sandbox" }
  | { kind: "path"; root: string };

export type ReconcileRequest = {
  targetState: Record<string, unknown>;
  invariants?: string[];
  workspace: ReconcileWorkspace;
  verification?: { requireTests?: boolean; command?: string };
  rollbackOnFailure?: boolean;
  mode?: "observe" | "ensure";
  idempotencyKey?: string;
};

export type ReconcileStatus =
  | "satisfied"
  | "failed"
  | "rolled_back"
  | "canceled"
  | "running";

export type ReconcileResult = {
  request_id?: string;
  status: ReconcileStatus;
  reason?: string;
  contract?: unknown;
  delta?: unknown;
  verification?: {
    ran?: boolean;
    passed?: boolean;
    command?: string;
    exit_code?: number;
    reason?: string;
    log?: string;
  };
  files_changed?: string[];
  rolled_back?: boolean;
  brief?: string;
};

export type SseFrame = {
  event: string;
  data: unknown;
  id?: string;
};

export type IntentClientOptions = {
  baseUrl?: string;
  apiKey: string;
  fetch?: typeof fetch;
};

function joinUrl(base: string, path: string): string {
  return base.replace(/\/+$/, "") + path;
}

async function readError(res: Response): Promise<string> {
  const text = await res.text();
  try {
    const j = JSON.parse(text) as { error?: string };
    if (j.error) return j.error;
  } catch {
    /* plain */
  }
  return text || res.statusText;
}

function parseSseBlock(block: string): SseFrame | null {
  let event = "message";
  let id: string | undefined;
  const dataLines: string[] = [];
  for (const line of block.split("\n")) {
    if (line.startsWith("event:")) event = line.slice(6).trim();
    else if (line.startsWith("id:")) id = line.slice(3).trim();
    else if (line.startsWith("data:")) dataLines.push(line.slice(5).trimStart());
  }
  if (dataLines.length === 0) return null;
  const raw = dataLines.join("\n");
  let data: unknown = raw;
  try {
    data = JSON.parse(raw);
  } catch {
    /* keep string */
  }
  return { event, data, id };
}

async function* iterateSse(res: Response): AsyncGenerator<SseFrame> {
  if (!res.body) throw new Error("SSE response has no body");
  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buf = "";
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += decoder.decode(value, { stream: true });
    buf = buf.replace(/\r\n/g, "\n");
    let idx: number;
    while ((idx = buf.indexOf("\n\n")) >= 0) {
      const block = buf.slice(0, idx);
      buf = buf.slice(idx + 2);
      if (block.startsWith(":")) continue; // heartbeat comment
      const frame = parseSseBlock(block);
      if (frame) yield frame;
    }
  }
}

export class IntentClient {
  readonly baseUrl: string;
  readonly apiKey: string;
  private readonly fetchImpl: typeof fetch;

  constructor(opts: IntentClientOptions) {
    if (!opts.apiKey) throw new Error("apiKey is required");
    this.baseUrl = opts.baseUrl || "http://127.0.0.1:8080";
    this.apiKey = opts.apiKey;
    this.fetchImpl = opts.fetch || globalThis.fetch.bind(globalThis);
  }

  private headers(extra?: Record<string, string>): Record<string, string> {
    return {
      Authorization: `Bearer ${this.apiKey}`,
      "Content-Type": "application/json",
      ...(extra || {}),
    };
  }

  async classify(req: IntentClassifyRequest): Promise<IntentClassifyResult> {
    const body: Record<string, unknown> = { message: req.message };
    if (req.requestedAgent) body.requested_agent = req.requestedAgent;
    if (req.mode) body.mode = req.mode;
    if (req.applyRouting !== undefined) body.apply_routing = req.applyRouting;
    const res = await this.fetchImpl(joinUrl(this.baseUrl, "/v1/intent"), {
      method: "POST",
      headers: this.headers(),
      body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(await readError(res));
    return (await res.json()) as IntentClassifyResult;
  }

  async reconcile(req: ReconcileRequest): Promise<ReconcileResult> {
    let last: ReconcileResult | undefined;
    for await (const ev of this.reconcileStream(req)) {
      if (ev.event === "reconcile.done" && ev.data && typeof ev.data === "object") {
        last = ev.data as ReconcileResult;
      }
    }
    if (!last) throw new Error("reconcile stream ended without reconcile.done");
    return last;
  }

  async *reconcileStream(req: ReconcileRequest): AsyncGenerator<SseFrame> {
    const body: Record<string, unknown> = {
      target_state: req.targetState,
      workspace: req.workspace,
      rollback_on_failure: !!req.rollbackOnFailure,
    };
    if (req.invariants) body.invariants = req.invariants;
    if (req.mode) body.mode = req.mode;
    if (req.verification) {
      body.verification = {
        require_tests: req.verification.requireTests !== false,
        command: req.verification.command || "auto",
      };
    }
    const headers = this.headers();
    if (req.idempotencyKey) headers["Idempotency-Key"] = req.idempotencyKey;
    const res = await this.fetchImpl(joinUrl(this.baseUrl, "/v1/reconcile"), {
      method: "POST",
      headers,
      body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(await readError(res));
    yield* iterateSse(res);
  }

  async getReconcile(id: string): Promise<ReconcileResult> {
    const res = await this.fetchImpl(joinUrl(this.baseUrl, `/v1/reconcile/${id}`), {
      method: "GET",
      headers: this.headers(),
    });
    if (!res.ok) throw new Error(await readError(res));
    return (await res.json()) as ReconcileResult;
  }

  async cancelReconcile(id: string): Promise<unknown> {
    const res = await this.fetchImpl(
      joinUrl(this.baseUrl, `/v1/reconcile/${id}/cancel`),
      { method: "POST", headers: this.headers() },
    );
    if (!res.ok) throw new Error(await readError(res));
    return res.json();
  }
}
