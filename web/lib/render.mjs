import path from 'node:path'
import pug from 'pug'
import { siteOrigin, viewsRoot } from './config.mjs'
import { sectionLabels } from './config.mjs'

const compileCache = new Map()

export function renderPage(templateName, locals = {}) {
  const templatePath = path.join(viewsRoot, templateName)
  let compile = compileCache.get(templatePath)
  if (!compile) {
    compile = pug.compileFile(templatePath, {
      basedir: viewsRoot,
      pretty: false,
    })
    compileCache.set(templatePath, compile)
  }

  return compile({
    sectionLabels,
    siteOrigin,
    ...locals,
  })
}
