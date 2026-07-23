import { spawn } from 'node:child_process'
import fs from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import {
  copyAssets,
  renderLlms,
  renderRobots,
  renderSitemap,
  writeFavicon,
  writeFile,
} from '../lib/assets.mjs'
import {
  dist,
  docsRoot,
  binaryRelease,
  installCommand,
  installerHref,
  installPath,
  liquidHeroPath,
  macDownloadUrl,
  repoRoot,
  root,
  siteScriptPath,
} from '../lib/config.mjs'
import { readDocs, sortDocs } from '../lib/docs.mjs'
import { extractToc, markdownToHtml } from '../lib/markdown.mjs'
import {
  buildBreadcrumbs,
  buildDocsIndexCards,
  buildSearchIndex,
  buildSectionNeighbors,
  buildSidebarModel,
  buildTitleMap,
} from '../lib/navigation.mjs'
import { renderPage } from '../lib/render.mjs'
import {
  defaultOgImage,
  defaultOgImageAlt,
  defaultOgImageHeight,
  defaultOgImageWidth,
} from '../lib/seo.mjs'
import { writeStyles } from '../lib/styles.mjs'

const buildScript = fileURLToPath(import.meta.url)

// Production builds stage into a temp dir and swap on success so a failed or
// interrupted run cannot wipe a previously good dist/. Dev already sets
// ARBITER_DIST_PATH to its own staging dir and publishes itself.
if (!process.env.ARBITER_DIST_PATH) {
  const stagingDist = path.join(root, '.build-dist')
  const exitCode = await runStagedBuild(stagingDist)
  if (exitCode !== 0) {
    await fs.rm(stagingDist, { force: true, recursive: true })
    process.exit(exitCode)
  }

  await fs.rm(dist, { force: true, recursive: true })
  await fs.rename(stagingDist, dist)
  console.log(`Built site into ${path.relative(repoRoot, dist)}`)
  process.exit(0)
}

await fs.rm(dist, { force: true, recursive: true })
await fs.mkdir(dist, { recursive: true })
await writeStyles(path.join(dist, 'styles.css'))
await fs.copyFile(siteScriptPath, path.join(dist, 'site.js'))
await fs.copyFile(liquidHeroPath, path.join(dist, 'liquid-hero.js'))
await fs.copyFile(installPath, path.join(dist, 'install.sh'))
await copyAssets()
await writeFavicon()

const docs = sortDocs(await readDocs(docsRoot))
const titlesByHref = buildTitleMap(docs)

await writeFile(
  'index.html',
  renderPage('home.pug', {
    canonicalPath: '/',
    description:
      'Arbiter is a local agent runtime in a single native binary — TUI, one-shot CLI, or HTTP+SSE — with hard tool allowlists and a shared streaming event bus.',
    gettingStartedHref:
      docs.find((doc) => doc.relative === 'getting-started/local.md')?.href ?? '/docs/',
    binaryRelease,
    installCommand,
    installerHref,
    macDownloadUrl,
    ogImage: defaultOgImage,
    ogImageAlt: defaultOgImageAlt,
    ogImageHeight: defaultOgImageHeight,
    ogImageWidth: defaultOgImageWidth,
    philosophyHref: docs.find((doc) => doc.relative === 'philosophy.md')?.href ?? '/docs/',
    title: 'Arbiter — the agent runtime that lives on your machine',
    variant: 'marketing',
  }),
)

await writeFile(
  'docs/index.html',
  renderPage('docs-index.pug', {
    cards: buildDocsIndexCards(docs),
    canonicalPath: '/docs/',
    description: 'Install, operate, and extend the Arbiter self-hosted agent runtime.',
    ogImage: defaultOgImage,
    ogImageAlt: defaultOgImageAlt,
    ogImageHeight: defaultOgImageHeight,
    ogImageWidth: defaultOgImageWidth,
    sidebar: buildSidebarModel(docs, { href: '/docs/', section: null }),
    title: 'Arbiter Documentation',
    variant: 'docs',
  }),
)

await writeFile('docs/search-index.json', `${JSON.stringify(buildSearchIndex(docs), null, 2)}\n`)

for (const doc of docs) {
  const { next, prev } = buildSectionNeighbors(docs, doc)
  const toc = extractToc(doc.source)

  await writeFile(
    doc.outputPath,
    renderPage('doc.pug', {
      breadcrumbs: buildBreadcrumbs(doc),
      canonicalPath: doc.href,
      contentHtml: markdownToHtml(doc.source, doc, { titlesByHref }),
      description: doc.description,
      next,
      ogImage: defaultOgImage,
      ogImageAlt: defaultOgImageAlt,
      ogImageHeight: defaultOgImageHeight,
      ogImageWidth: defaultOgImageWidth,
      prev,
      sidebar: buildSidebarModel(docs, doc),
      title: `${titlesByHref.get(doc.href) ?? doc.title} — Arbiter Docs`,
      toc,
      variant: 'docs',
    }),
  )
}

await writeFile('robots.txt', renderRobots())
await writeFile('sitemap.xml', renderSitemap(docs))
await writeFile('llms.txt', renderLlms(docs))

console.log(`Built ${docs.length + 2} pages into ${path.relative(repoRoot, dist)}`)

function runStagedBuild(stagingDist) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [buildScript], {
      cwd: root,
      env: {
        ...process.env,
        ARBITER_DIST_PATH: stagingDist,
      },
      stdio: 'inherit',
    })

    child.once('error', (error) => {
      console.error('[build] failed to start:', error)
      resolve(1)
    })
    child.once('close', (code) => {
      resolve(code ?? 1)
    })
  })
}
