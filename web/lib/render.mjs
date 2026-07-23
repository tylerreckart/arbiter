import path from 'node:path'
import pug from 'pug'
import { siteOrigin, viewsRoot } from './config.mjs'
import { sectionLabels } from './config.mjs'
import {
  buildJsonLd,
  defaultOgImage,
  defaultOgImageAlt,
  defaultOgImageHeight,
  defaultOgImageWidth,
} from './seo.mjs'

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
    buildJsonLd,
    defaultOgImage,
    defaultOgImageAlt,
    defaultOgImageHeight,
    defaultOgImageWidth,
    sectionLabels,
    siteOrigin,
    ...locals,
  })
}
