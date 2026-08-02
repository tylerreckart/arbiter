import path from 'node:path'
import * as esbuild from 'esbuild'
import { dist, mermaidInitPath } from './config.mjs'

export async function bundleMermaid() {
  await esbuild.build({
    entryPoints: [mermaidInitPath],
    outfile: path.join(dist, 'mermaid-init.js'),
    format: 'esm',
    bundle: true,
    platform: 'browser',
  })
}
