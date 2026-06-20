// Cloudflare Worker: SubTropica submission proxy + PDF proxy
// Routes: POST /submit -> triggers GitHub workflow_dispatch
//         GET  /pdf?arxivId=... -> proxies arXiv PDF with CORS headers
//         GET  /staged/<sha256> -> serves a staged submission (token-gated)
//
// Deploy:
//   cd infrastructure/cloudflare-worker
//   wrangler deploy
//
// Secrets (set via wrangler secret put):
//   GITHUB_PAT — fine-grained token scoped to SubTropica/SubTropica
//                with Actions (write) permission only
//   STAGED_TOKEN — bearer token for GET /staged/<sha256>, shared with the
//                  GitHub Actions secret of the same name
//   STAGED_TOKEN_NEXT — optional second token accepted during rotation
//                       (set _NEXT, flip CI, retire old value into
//                       STAGED_TOKEN, then clear _NEXT)
//
// KV namespace (for dedup):
//   Create: wrangler kv namespace create SUBMISSIONS
//   Then add the binding to wrangler.toml
//
// R2 bucket (for staged large submissions, results-split spec §5.1):
//   Create: wrangler r2 bucket create subtropica-staged-submissions
//   Binding STAGED in wrangler.toml

const GITHUB_REPO = "SubTropica/SubTropica";
const WORKFLOW_FILE = "submit.yml";
const CORRECTION_WORKFLOW_FILE = "correction.yml";
const PAPER_REQUEST_WORKFLOW_FILE = "paper-request.yml";

// arXiv ID validation: new-style YYMM.NNNNN OR old-style category/YYMMNNN.
// Same regex used for /pdf — kept inline rather than refactored to a
// shared constant to keep this Worker single-file and dependency-free.
const ARXIV_ID_RE = /^(\d{4}\.\d{4,5}|(?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})$/;

function paperRequestDedupKey(payload) {
  // Dedup purely by arXiv ID: re-requests for the same paper within the
  // TTL window are no-ops, regardless of who asked.
  return "papreq:" + (payload.arxivId || "").trim().toLowerCase();
}

// Build a dedup key from the submission's identity fields
function dedupKey(payload) {
  const parts = [
    payload.cnickelIndex || "",
    payload.dimension || "",
    String(payload.epsOrder ?? ""),
    JSON.stringify((payload.propExponents || []).sort()),
    payload.substitutions || "{}",
    payload.normalization || "Automatic",
  ];
  return "sub:" + parts.join("|");
}

// Whitelist of correction categories.  Duplicated in the UI select; the
// GitHub workflow uses the same strings as label names (prefixed with
// "correction:") so the set must be kept in sync across the three layers.
const CORRECTION_CATEGORIES = new Set([
  "misidentified-diagram",
  "wrong-masses",
  "wrong-propagators",
  "wrong-description",
  "wrong-reference",
  "citation-request",
  "other",
]);

function correctionDedupKey(payload) {
  // Normalize whitespace in the free-text comment so a user clicking
  // Submit twice is deduped, but two reporters flagging the same record
  // with different comments each get an issue.
  const commentNorm = (payload.comment || "").replace(/\s+/g, " ").trim().toLowerCase();
  const parts = [
    payload.cnickelIndex || "",
    payload.recordId || "",
    payload.category || "",
    commentNorm,
  ];
  return "corr:" + parts.join("|");
}

// Stamp Access-Control-Allow-Origin on EVERY response, not just the OPTIONS
// preflight and GET /pdf. The POST JSON replies (/submit, /correction,
// /request-paper) are built with Response.json() and previously carried no
// CORS headers, so the browser blocked the response body and the client's
// fetch() rejected — the UI showed "Network error" even though the request
// had reached the Worker and the GitHub workflow_dispatch had already fired
// (filing the issue / dispatching the run). A single wrapper is the reliable
// chokepoint; per-route headers drift out of sync, which is exactly how the
// POST routes ended up uncovered.
export default {
  async fetch(request, env) {
    const resp = await handleRequest(request, env);
    resp.headers.set("Access-Control-Allow-Origin", "*");
    return resp;
  },
};

async function handleRequest(request, env) {
    // CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, {
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, POST",
          "Access-Control-Allow-Headers": "Content-Type"
        }
      });
    }

    const url = new URL(request.url);

    // ── GET /pdf?arxivId=... — proxy arXiv PDFs with CORS headers ──
    if (request.method === "GET" && url.pathname === "/pdf") {
      const arxivId = url.searchParams.get("arxivId");
      if (!arxivId || !/^(\d{4}\.\d{4,5}|(?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})$/.test(arxivId)) {
        return new Response("Invalid arxivId", {
          status: 400,
          headers: { "Access-Control-Allow-Origin": "*" }
        });
      }

      // Check Cloudflare cache first
      const cacheKey = new Request(`https://pdf-cache.subtropi.ca/${arxivId.replace("/", "_")}.pdf`);
      const cache = caches.default;
      let cached = await cache.match(cacheKey);
      if (cached) {
        return new Response(cached.body, {
          headers: {
            "Content-Type": "application/pdf",
            "Access-Control-Allow-Origin": "*",
            "Cache-Control": "public, max-age=604800"
          }
        });
      }

      // Fetch from arXiv
      try {
        const pdfResp = await fetch(`https://arxiv.org/pdf/${arxivId}.pdf`, {
          headers: { "User-Agent": "SubTropica/1.0 (https://subtropi.ca)" }
        });
        if (!pdfResp.ok) {
          return new Response(`arXiv returned ${pdfResp.status}`, {
            status: 502,
            headers: { "Access-Control-Allow-Origin": "*" }
          });
        }
        const pdfBytes = await pdfResp.arrayBuffer();
        const response = new Response(pdfBytes, {
          headers: {
            "Content-Type": "application/pdf",
            "Access-Control-Allow-Origin": "*",
            "Cache-Control": "public, max-age=604800"
          }
        });
        // Store in Cloudflare cache (7 day TTL)
        await cache.put(cacheKey, response.clone());
        return response;
      } catch (e) {
        return new Response("Fetch error: " + e.message, {
          status: 502,
          headers: { "Access-Control-Allow-Origin": "*" }
        });
      }
    }

    // ── GET /staged/<sha256> — serve a staged submission to the workflow ──
    // Auth: bearer token shared with GitHub Actions secrets. Two secrets are
    // checked so rotation can overlap (set STAGED_TOKEN_NEXT, flip CI, then
    // retire the old value into STAGED_TOKEN and clear _NEXT).
    if (request.method === "GET" && url.pathname.startsWith("/staged/")) {
      const auth = request.headers.get("Authorization") || "";
      const tok = auth.replace(/^Bearer\s+/i, "");
      const valid = [env.STAGED_TOKEN, env.STAGED_TOKEN_NEXT].filter(Boolean);
      if (!valid.length || !valid.includes(tok)) {
        return Response.json({ status: "error", error: "Unauthorized" }, { status: 401 });
      }
      const key = url.pathname.slice("/staged/".length);
      if (!/^[0-9a-f]{64}$/.test(key)) {
        return Response.json({ status: "error", error: "Bad key" }, { status: 400 });
      }
      if (!env.STAGED) {
        return Response.json({ status: "error", error: "R2 binding missing" }, { status: 503 });
      }
      let obj;
      try {
        obj = await env.STAGED.get(`submissions/${key}.json`);
      } catch (e) {
        return Response.json(
          { status: "error", error: "Staging storage unavailable" },
          { status: 503 });
      }
      if (!obj) {
        return Response.json(
          { status: "error", error: "Submission expired or unknown; please resubmit" },
          { status: 404 });
      }
      return new Response(obj.body, {
        headers: {
          "Content-Type": "application/json",
          "Cache-Control": "no-store",
        },
      });
    }

    // ── POST /correction — correction-report proxy ──
    // Triggers the correction.yml workflow, which opens a GitHub issue.
    // Separate path from /submit because validation, dedup TTL, and the
    // downstream workflow all differ.
    if (request.method === "POST" && url.pathname === "/correction") {
      try {
        const payload = await request.json();

        const required = ["cnickelIndex", "recordId", "category", "comment"];
        for (const f of required) {
          if (payload[f] === undefined || payload[f] === null || payload[f] === "") {
            return Response.json(
              { status: "error", error: `Missing required field: ${f}` },
              { status: 400 }
            );
          }
        }

        if (!CORRECTION_CATEGORIES.has(payload.category)) {
          return Response.json(
            { status: "error", error: `Invalid category: ${payload.category}` },
            { status: 400 }
          );
        }

        // Free-text fields are capped at 2000 chars in the UI; enforce an
        // overall 32 KB payload guard here as a backstop against abuse.
        const payloadStr = JSON.stringify(payload);
        if (payloadStr.length > 32_000) {
          return Response.json(
            { status: "error", error: "Payload too large (max 32 KB)" },
            { status: 413 }
          );
        }

        // Dedup via KV (same namespace as /submit, 7-day TTL).
        const ckey = correctionDedupKey(payload);
        if (env.SUBMISSIONS) {
          const existing = await env.SUBMISSIONS.get(ckey);
          if (existing) {
            return Response.json({
              status: "duplicate",
              message: "An identical correction was already reported.",
              previousSubmission: existing,
            });
          }
        }

        const ghResp = await fetch(
          `https://api.github.com/repos/${GITHUB_REPO}/actions/workflows/${CORRECTION_WORKFLOW_FILE}/dispatches`,
          {
            method: "POST",
            headers: {
              "Authorization": `Bearer ${env.GITHUB_PAT}`,
              "Accept": "application/vnd.github+json",
              "User-Agent": "SubTropica-Worker",
              "X-GitHub-Api-Version": "2022-11-28",
            },
            body: JSON.stringify({
              ref: "main",
              inputs: { payload: payloadStr },
            }),
          }
        );

        if (!ghResp.ok) {
          const err = await ghResp.text();
          console.error("GitHub API error (correction):", ghResp.status, err);
          return Response.json(
            { status: "error", error: "GitHub API error", detail: err },
            { status: 502 }
          );
        }

        if (env.SUBMISSIONS) {
          await env.SUBMISSIONS.put(ckey, new Date().toISOString(), {
            expirationTtl: 7 * 24 * 3600,
          });
        }

        return Response.json({
          status: "ok",
          message: "Correction dispatched. A maintainer will review shortly.",
        });
      } catch (e) {
        console.error("Worker error (correction):", e);
        return Response.json(
          { status: "error", error: e.message },
          { status: 500 }
        );
      }
    }

    // ── POST /request-paper — "please analyze this paper" intake ──
    // Opens a public GitHub issue with label `paper-request` so a
    // maintainer can run the v2 arXiv pipeline on the paper at their
    // leisure. No integration / submission required — this is the
    // pre-integration intake path for elliptic or higher-genus papers
    // that current SubTropica can't integrate.
    if (request.method === "POST" && url.pathname === "/request-paper") {
      try {
        const payload = await request.json();

        const arxivId = (payload.arxivId || "").trim();
        if (!arxivId) {
          return Response.json(
            { status: "error", error: "Missing required field: arxivId" },
            { status: 400 }
          );
        }
        if (!ARXIV_ID_RE.test(arxivId)) {
          return Response.json(
            { status: "error", error: "Invalid arXiv ID format" },
            { status: 400 }
          );
        }

        // Free-text reason capped to keep issue bodies readable; name +
        // email each capped at typical-form sizes.
        const reason = (payload.reason || "").toString().slice(0, 2000);
        const name   = (payload.name   || "").toString().slice(0, 200);
        const email  = (payload.email  || "").toString().slice(0, 320);  // RFC 5321
        // Loose email format check — only used to suppress garbage in
        // the issue body; the field is optional and we don't actually
        // email anyone from the Worker.
        if (email && !/^\S+@\S+\.\S+$/.test(email)) {
          return Response.json(
            { status: "error", error: "Invalid email format" },
            { status: 400 }
          );
        }

        const normalized = { arxivId, reason, name, email,
                             timestamp: new Date().toISOString() };
        const payloadStr = JSON.stringify(normalized);
        if (payloadStr.length > 8_000) {
          return Response.json(
            { status: "error", error: "Payload too large (max 8 KB)" },
            { status: 413 }
          );
        }

        // Dedup: 24 h cooldown per arXiv ID. Long enough to swallow
        // double-clicks and accidental re-submissions; short enough
        // that a user who realises they want to add context can re-file
        // next day.
        const dkey = paperRequestDedupKey(normalized);
        if (env.SUBMISSIONS) {
          const existing = await env.SUBMISSIONS.get(dkey);
          if (existing) {
            return Response.json({
              status: "duplicate",
              message: "A request for this paper was already filed in the last 24 hours.",
              previousSubmission: existing,
            });
          }
        }

        const ghResp = await fetch(
          `https://api.github.com/repos/${GITHUB_REPO}/actions/workflows/${PAPER_REQUEST_WORKFLOW_FILE}/dispatches`,
          {
            method: "POST",
            headers: {
              "Authorization": `Bearer ${env.GITHUB_PAT}`,
              "Accept": "application/vnd.github+json",
              "User-Agent": "SubTropica-Worker",
              "X-GitHub-Api-Version": "2022-11-28",
            },
            body: JSON.stringify({
              ref: "main",
              inputs: { payload: payloadStr },
            }),
          }
        );

        if (!ghResp.ok) {
          const err = await ghResp.text();
          console.error("GitHub API error (paper-request):", ghResp.status, err);
          return Response.json(
            { status: "error", error: "GitHub API error", detail: err },
            { status: 502 }
          );
        }

        if (env.SUBMISSIONS) {
          await env.SUBMISSIONS.put(dkey, new Date().toISOString(), {
            expirationTtl: 24 * 3600,
          });
        }

        return Response.json({
          status: "ok",
          message: "Paper request filed. A maintainer will pick it up shortly.",
        });
      } catch (e) {
        console.error("Worker error (paper-request):", e);
        return Response.json(
          { status: "error", error: e.message },
          { status: 500 }
        );
      }
    }

    // ── POST /submit — submission proxy ──
    if (request.method !== "POST" || !url.pathname.startsWith("/submit")) {
      return Response.json({ status: "error", error: "Not found" }, { status: 404 });
    }

    try {
      const payload = await request.json();

      // Validate required fields
      const required = ["cnickelIndex", "resultCompressed", "dimension", "epsOrder"];
      for (const f of required) {
        if (payload[f] === undefined || payload[f] === null || payload[f] === "") {
          return Response.json(
            { status: "error", error: `Missing required field: ${f}` },
            { status: 400 }
          );
        }
      }

      // Size guard: reject payloads > 16 MB
      const payloadStr = JSON.stringify(payload);
      if (payloadStr.length > 16_000_000) {
        return Response.json(
          { status: "error", error: "Payload too large (max 16 MB)" },
          { status: 413 }
        );
      }

      // Dedup check via KV store
      const key = dedupKey(payload);
      if (env.SUBMISSIONS) {
        const existing = await env.SUBMISSIONS.get(key);
        if (existing) {
          return Response.json({
            status: "duplicate",
            message: "A result with these parameters was already submitted.",
            previousSubmission: existing
          });
        }
      }

      // Results-split spec §5.1: stage the payload in R2 and dispatch only a
      // reference. Fixes the old latent ceiling (dispatch inputs ~64 KB, env
      // var ~128 KiB) that made the nominal 1 MB cap partly fictional.
      let inputs;
      if (env.STAGED) {
        const digest = await crypto.subtle.digest(
          "SHA-256", new TextEncoder().encode(payloadStr));
        const sha256 = [...new Uint8Array(digest)]
          .map(b => b.toString(16).padStart(2, "0")).join("");
        await env.STAGED.put(`submissions/${sha256}.json`, payloadStr);
        inputs = { submission_key: sha256, sha256,
                   size: String(payloadStr.length) };
      } else {
        // Legacy inline path (R2 not yet bound): only viable for payloads
        // under the GitHub dispatch-input limit; preserved for rollout
        // ordering, not as a permanent mode.
        if (payloadStr.length > 60_000) {
          return Response.json(
            { status: "error",
              error: "Large submissions need the R2 staging deployment" },
            { status: 503 });
        }
        inputs = { payload: payloadStr };
      }

      // Trigger GitHub workflow_dispatch
      const ghResp = await fetch(
        `https://api.github.com/repos/${GITHUB_REPO}/actions/workflows/${WORKFLOW_FILE}/dispatches`,
        {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${env.GITHUB_PAT}`,
            "Accept": "application/vnd.github+json",
            "User-Agent": "SubTropica-Worker",
            "X-GitHub-Api-Version": "2022-11-28"
          },
          body: JSON.stringify({ ref: "main", inputs })
        }
      );

      if (!ghResp.ok) {
        const err = await ghResp.text();
        console.error("GitHub API error:", ghResp.status, err);
        return Response.json(
          { status: "error", error: "GitHub API error", detail: err },
          { status: 502 }
        );
      }

      // Record in KV to prevent future duplicates (TTL: 1 year)
      if (env.SUBMISSIONS) {
        await env.SUBMISSIONS.put(key, new Date().toISOString(), {
          expirationTtl: 365 * 24 * 3600
        });
      }

      // workflow_dispatch returns 204 No Content on success
      return Response.json({
        status: "ok",
        message: "Submission dispatched. A maintainer will review the PR."
      });

    } catch (e) {
      console.error("Worker error:", e);
      return Response.json(
        { status: "error", error: e.message },
        { status: 500 }
      );
    }
}
