// Cloudflare Worker: SubTropica submission proxy + PDF proxy
// Routes: POST /submit -> triggers GitHub workflow_dispatch
//         GET  /pdf?arxivId=... -> proxies arXiv PDF with CORS headers
//
// Deploy:
//   cd infrastructure/cloudflare-worker
//   wrangler deploy
//
// Secrets (set via wrangler secret put):
//   GITHUB_PAT — fine-grained token scoped to SubTropica/SubTropica
//                with Actions (write) permission only
//
// KV namespace (for dedup):
//   Create: wrangler kv namespace create SUBMISSIONS
//   Then add the binding to wrangler.toml

const GITHUB_REPO = "SubTropica/SubTropica";
const WORKFLOW_FILE = "submit.yml";
const CORRECTION_WORKFLOW_FILE = "correction.yml";

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

export default {
  async fetch(request, env) {
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

      // Size guard: reject payloads > 1 MB
      const payloadStr = JSON.stringify(payload);
      if (payloadStr.length > 1_000_000) {
        return Response.json(
          { status: "error", error: "Payload too large (max 1 MB)" },
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
          body: JSON.stringify({
            ref: "main",
            inputs: { payload: payloadStr }
          })
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
};
