import fs from 'node:fs/promises'
import path from 'node:path'
import { assetAllowlist, assetsPath, dist, siteOrigin } from './config.mjs'

export async function writeFile(relativePath, contents) {
  const target = path.join(dist, relativePath)
  await fs.mkdir(path.dirname(target), { recursive: true })
  await fs.writeFile(target, contents)
}

export async function copyAssets() {
  try {
    await fs.access(assetsPath)
  } catch {
    return
  }

  const targetDir = path.join(dist, 'assets')
  await fs.mkdir(targetDir, { recursive: true })
  const entries = await fs.readdir(assetsPath, { withFileTypes: true })
  for (const entry of entries) {
    if (!entry.isFile()) continue
    if (!assetAllowlist.has(entry.name)) continue
    await fs.copyFile(path.join(assetsPath, entry.name), path.join(targetDir, entry.name))
  }
}

export async function writeFavicon() {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" role="img">
  <rect width="64" height="64" fill="#17130f"/>
  <rect x="12" y="12" width="40" height="40" fill="#ff8a24"/>
  <text x="32" y="41" text-anchor="middle" font-family="ui-monospace, monospace" font-size="26" fill="#191510">⛮</text>
</svg>`
  await writeFile('favicon.svg', svg)
}

export function renderSitemap(docs) {
  const urls = ['/', '/docs/', ...docs.map((doc) => doc.href)]
  return `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
${urls.map((url) => `  <url><loc>${siteOrigin}${url}</loc></url>`).join('\n')}
</urlset>
`
}

export function renderRobots() {
  return `User-agent: *\nAllow: /\nSitemap: ${siteOrigin}/sitemap.xml\n`
}
