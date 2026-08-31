// SPDX-License-Identifier: MIT

import crypto from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const acsRootDefault = path.resolve(scriptDirectory, "..");

function parseArguments(argv) {
  const result = {
    acsRoot: acsRootDefault,
    output: path.join(acsRootDefault, "docs", "reference", "source"),
  };
  for (let index = 0; index < argv.length; index += 1) {
    if (argv[index] === "--acs-root" && argv[index + 1]) {
      result.acsRoot = path.resolve(argv[index + 1]);
      index += 1;
    } else if (argv[index] === "--output" && argv[index + 1]) {
      result.output = path.resolve(argv[index + 1]);
      index += 1;
    } else {
      throw new Error(`不明な引数です: ${argv[index]}`);
    }
  }
  return result;
}

function stripTags(value) {
  return String(value ?? "").replace(/<[^>]+>/g, "").trim();
}

function slug(value) {
  const normalized = stripTags(value)
    .normalize("NFKC")
    .toLowerCase()
    .replace(/[^a-z0-9\u3040-\u30ff\u3400-\u9fff]+/g, "-")
    .replace(/^-+|-+$/g, "");
  return normalized || "item";
}

function shortHash(value) {
  return crypto.createHash("sha256").update(value).digest("hex").slice(0, 10);
}

function stableJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

async function writeJson(root, relativePath, value, written) {
  const target = path.resolve(root, relativePath);
  const relative = path.relative(root, target);
  if (relative.startsWith("..") || path.isAbsolute(relative)) {
    throw new Error(`出力先が source の外側です: ${target}`);
  }
  await fs.mkdir(path.dirname(target), { recursive: true });
  await fs.writeFile(target, stableJson(value), "utf8");
  written.push(relative.replaceAll(path.sep, "/"));
}

async function loadLegacyData(dataDirectory) {
  const entries = (await fs.readdir(dataDirectory, { withFileTypes: true }))
    .filter((entry) => entry.isFile() && entry.name.endsWith(".js"))
    .map((entry) => entry.name)
    .sort((left, right) => {
      if (left === "_meta.js") return -1;
      if (right === "_meta.js") return 1;
      return left.localeCompare(right, "en");
    });

  const reference = {
    modules: [],
    glossary: {},
    guide: [],
    troubleshooting: [],
  };
  const context = vm.createContext({ ACS_REF: reference, console });
  context.window = context;
  context.window.ACS_REF = reference;

  for (const name of entries) {
    const filePath = path.join(dataDirectory, name);
    const source = await fs.readFile(filePath, "utf8");
    vm.runInContext(source, context, { filename: filePath, timeout: 10_000 });
  }
  return reference;
}

function splitGuide(blocks) {
  const sections = [];
  let current = null;
  for (const block of blocks ?? []) {
    if (block && block.h2) {
      current = { title: stripTags(block.h2), blocks: [block] };
      sections.push(current);
    } else {
      if (!current) {
        current = { title: "概要", blocks: [] };
        sections.push(current);
      }
      current.blocks.push(block);
    }
  }
  return sections;
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const dataDirectory = path.join(options.acsRoot, "docs", "reference", "data");
  const output = options.output;
  const outputParent = path.dirname(output);
  const relativeOutput = path.relative(outputParent, output);
  if (!relativeOutput || relativeOutput.startsWith("..") || path.isAbsolute(relativeOutput)) {
    throw new Error(`安全に初期化できない出力先です: ${output}`);
  }

  try {
    const existing = await fs.readdir(output);
    if (existing.length > 0) {
      throw new Error(`出力先が空ではありません: ${output}`);
    }
  } catch (error) {
    if (error && error.code !== "ENOENT") throw error;
  }

  const reference = await loadLegacyData(dataDirectory);
  const written = [];
  let featureCount = 0;

  for (const moduleRecord of reference.modules ?? []) {
    const moduleId = slug(moduleRecord.id || "module");
    for (const feature of moduleRecord.types ?? []) {
      const identity = JSON.stringify({
        module: moduleId,
        name: stripTags(feature.name),
        kind: stripTags(feature.kind),
        header: stripTags(feature.header),
      });
      const filename = `${slug(feature.name)}-${shortHash(identity)}.json`;
      await writeJson(output, path.join("features", moduleId, filename), {
        schema: 1,
        module: {
          id: moduleId,
          title: stripTags(moduleRecord.title || moduleId),
          description: moduleRecord.blurb || "",
        },
        feature,
      }, written);
      featureCount += 1;
    }
  }

  const guideSections = splitGuide(reference.guide);
  for (let index = 0; index < guideSections.length; index += 1) {
    const section = guideSections[index];
    const filename = `${String(index + 1).padStart(2, "0")}-${slug(section.title)}.json`;
    await writeJson(output, path.join("guides", filename), {
      schema: 1,
      title: section.title,
      blocks: section.blocks,
    }, written);
  }

  const troubleshooting = reference.troubleshooting ?? [];
  for (let index = 0; index < troubleshooting.length; index += 1) {
    const item = troubleshooting[index];
    const title = stripTags(item.title || item.h2 || item.symptom || `項目${index + 1}`);
    const filename = `${String(index + 1).padStart(3, "0")}-${slug(title)}.json`;
    await writeJson(output, path.join("troubleshooting", filename), {
      schema: 1,
      title,
      item,
    }, written);
  }

  const glossaryEntries = Object.entries(reference.glossary ?? {})
    .sort(([left], [right]) => left.localeCompare(right, "ja"));
  for (const [term, definition] of glossaryEntries) {
    const filename = `${slug(term)}-${shortHash(term)}.json`;
    await writeJson(output, path.join("glossary", filename), {
      schema: 1,
      term,
      definition,
    }, written);
  }

  written.sort();
  await writeJson(output, "manifest.json", {
    schema: 1,
    featureCount,
    guideSectionCount: guideSections.length,
    troubleshootingCount: troubleshooting.length,
    glossaryCount: glossaryEntries.length,
    files: written,
  }, written);

  process.stdout.write(
    `機能 ${featureCount} 件、ガイド ${guideSections.length} 件、` +
    `トラブル対処 ${troubleshooting.length} 件、用語 ${glossaryEntries.length} 件を分割しました。\n`,
  );
}

await main();
