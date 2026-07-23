import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))

export const root = path.resolve(here, '..')
export const repoRoot = path.resolve(root, '..')
export const docsRoot = process.env.ARBITER_DOCS_PATH
  ? path.resolve(process.env.ARBITER_DOCS_PATH)
  : path.join(repoRoot, 'docs')
export const dist = process.env.ARBITER_DIST_PATH
  ? path.resolve(process.env.ARBITER_DIST_PATH)
  : path.join(root, 'dist')
export const viewsRoot = path.join(root, 'views')
export const stylesDir = path.join(root, 'src', 'styles')
export const styleSheets = [
  'tokens.css',
  'base.css',
  'chrome.css',
  'marketing.css',
  'docs.css',
  'footer.css',
  'responsive.css',
]
export const siteScriptPath = path.join(root, 'src', 'site.js')
export const liquidHeroPath = path.join(root, 'src', 'liquid-hero.js')
export const installPath = path.join(root, 'install.sh')
export const assetsPath = path.join(repoRoot, 'assets')

export const siteOrigin = 'https://arbiter.run'
export const githubBlobBase = 'https://github.com/tylerreckart/arbiter/blob/main'
export const installCommand = 'curl -fsSL https://arbiter.run/install.sh | sh'
export const binaryRelease = 'v0.8.7'
export const macDownloadUrl =
  `https://github.com/tylerreckart/arbiter/releases/download/${binaryRelease}/` +
  'arbiter-macos-arm64.tar.gz'
export const installerHref = `${githubBlobBase}/web/install.sh`

export const sectionOrder = [
  'getting-started',
  'concepts',
  'cli',
  'tui',
  'api',
  'philosophy',
]

export const sectionLabels = {
  'getting-started': 'Getting Started',
  concepts: 'Concepts',
  cli: 'CLI',
  tui: 'TUI',
  api: 'API Reference',
  philosophy: 'Philosophy',
}

export const assetAllowlist = new Set([
  'og_image.jpg',
  'vesper.jpg',
])
