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
      const cacheKey = new Request(`https://pdf-cache.subtropica.org/${arxivId.replace("/", "_")}.pdf`);
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
          headers: { "User-Agent": "SubTropica/1.0 (https://subtropica.org)" }
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
