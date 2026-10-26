// Bundles the OpenFront engine plus js/adapter.ts into one classic script
// that QuickJS evaluates at startup (romfs/engine.js).
//
// usage: node scripts/build-bundle.mjs <openfront-checkout> <out.js>
//
// The adapter is copied into the OpenFront checkout so its @openfront/*
// imports resolve through that checkout's npm workspaces, exactly as the web
// client's do.
import { cpSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const [ofDir, outFile] = process.argv.slice(2).map((p) => resolve(p));
if (!ofDir || !outFile) {
  console.error("usage: node build-bundle.mjs <openfront-checkout> <out.js>");
  process.exit(1);
}

const portDir = join(ofDir, "switch-port");
mkdirSync(portDir, { recursive: true });
cpSync(join(here, "..", "js"), portDir, { recursive: true });

const require = createRequire(join(ofDir, "package.json"));
const esbuild = require("esbuild");

const prelude = readFileSync(join(portDir, "prelude.js"), "utf8");
await esbuild.build({
  absWorkingDir: ofDir,
  entryPoints: [join(portDir, "adapter.ts")],
  bundle: true,
  format: "iife",
  platform: "neutral",
  mainFields: ["module", "main"],
  // QuickJS-ng implements ES2023; es2022 keeps esbuild from lowering much.
  target: "es2022",
  minify: true,
  legalComments: "none",
  tsconfig: join(ofDir, "tsconfig.json"),
  banner: { js: prelude },
  outfile: outFile,
  logLevel: "info",
});

// Licence notice: the bundle is OpenFront's AGPL-3.0 engine.
const body = readFileSync(outFile, "utf8");
writeFileSync(
  outFile,
  "/* OpenFront engine (https://github.com/openfrontio/OpenFrontIO), AGPL-3.0 */\n" +
    body,
);
