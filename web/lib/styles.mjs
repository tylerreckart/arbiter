import fs from 'node:fs/promises'
import path from 'node:path'
import { stylesDir, styleSheets } from './config.mjs'

export async function bundleStyles() {
  const parts = []
  for (const name of styleSheets) {
    const filePath = path.join(stylesDir, name)
    const source = await fs.readFile(filePath, 'utf8')
    parts.push(`/* === ${name} === */\n${source.trimEnd()}\n`)
  }
  return `${parts.join('\n')}\n`
}

export async function writeStyles(targetPath) {
  await fs.mkdir(path.dirname(targetPath), { recursive: true })
  await fs.writeFile(targetPath, await bundleStyles())
}
