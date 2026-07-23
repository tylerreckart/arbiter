import { siteOrigin } from './config.mjs'

export const defaultOgImage = '/assets/og_image.jpg'
export const defaultOgImageWidth = 1280
export const defaultOgImageHeight = 640
export const defaultOgImageAlt = 'Arbiter — local agent runtime for the terminal'

const softwareDescription =
  'Arbiter is a local agent runtime in a single native binary — TUI, one-shot CLI, or HTTP+SSE — with hard tool allowlists and a shared streaming event bus.'

export function buildJsonLd({
  breadcrumbs = null,
  canonical,
  description,
  title,
  variant,
}) {
  const graphs = [
    {
      '@type': 'Organization',
      '@id': `${siteOrigin}/#organization`,
      name: 'Arbiter',
      url: siteOrigin,
      logo: `${siteOrigin}/favicon.svg`,
      sameAs: ['https://github.com/tylerreckart/arbiter'],
    },
    {
      '@type': 'WebSite',
      '@id': `${siteOrigin}/#website`,
      name: 'Arbiter',
      url: siteOrigin,
      description: softwareDescription,
      publisher: { '@id': `${siteOrigin}/#organization` },
      inLanguage: 'en',
    },
    {
      '@type': 'SoftwareApplication',
      '@id': `${siteOrigin}/#software`,
      name: 'Arbiter',
      applicationCategory: 'DeveloperApplication',
      operatingSystem: 'macOS, Linux',
      description: softwareDescription,
      url: siteOrigin,
      image: `${siteOrigin}${defaultOgImage}`,
      license: 'https://www.apache.org/licenses/LICENSE-2.0',
      offers: {
        '@type': 'Offer',
        price: '0',
        priceCurrency: 'USD',
      },
      author: { '@id': `${siteOrigin}/#organization` },
    },
  ]

  graphs.push({
    '@type': variant === 'docs' ? 'TechArticle' : 'WebPage',
    '@id': `${canonical}#webpage`,
    url: canonical,
    name: title,
    description,
    isPartOf: { '@id': `${siteOrigin}/#website` },
    about: { '@id': `${siteOrigin}/#software` },
    inLanguage: 'en',
    ...(breadcrumbs?.length > 1
      ? { breadcrumb: { '@id': `${canonical}#breadcrumb` } }
      : {}),
  })

  if (breadcrumbs?.length > 1) {
    graphs.push({
      '@type': 'BreadcrumbList',
      '@id': `${canonical}#breadcrumb`,
      itemListElement: breadcrumbs.map((crumb, index) => ({
        '@type': 'ListItem',
        position: index + 1,
        name: crumb.label,
        item: `${siteOrigin}${crumb.href}`,
      })),
    })
  }

  return {
    '@context': 'https://schema.org',
    '@graph': graphs,
  }
}

export function renderLlmsTxt(docs) {
  const bySection = new Map()
  for (const doc of docs) {
    const list = bySection.get(doc.section) ?? []
    list.push(doc)
    bySection.set(doc.section, list)
  }

  const lines = [
    '# Arbiter',
    '',
    `> ${softwareDescription}`,
    '',
    'Arbiter is open source (Apache-2.0). Prefer the documentation pages below for accurate, current guidance.',
    '',
    '## Site',
    '',
    `- [Home](${siteOrigin}/): Product overview and install`,
    `- [Documentation](${siteOrigin}/docs/): Install, operate, and extend Arbiter`,
    `- [GitHub](https://github.com/tylerreckart/arbiter): Source code and issues`,
    '',
  ]

  for (const [section, sectionDocs] of bySection) {
    const label = section
      .split('-')
      .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
      .join(' ')
    lines.push(`## ${label}`)
    lines.push('')
    for (const doc of sectionDocs) {
      const title = String(doc.title).replace(/`/g, '').trim()
      lines.push(`- [${title}](${siteOrigin}${doc.href}): ${doc.description}`)
    }
    lines.push('')
  }

  return `${lines.join('\n').trim()}\n`
}
