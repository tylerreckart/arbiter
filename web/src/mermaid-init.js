import mermaid from 'mermaid'

mermaid.initialize({
  startOnLoad: false,
  theme: 'base',
  securityLevel: 'strict',
  fontFamily: 'IBM Plex Sans, Segoe UI, sans-serif',
  flowchart: {
    curve: 'basis',
    htmlLabels: true,
    padding: 14,
    nodeSpacing: 48,
    rankSpacing: 56,
  },
  themeVariables: {
    darkMode: true,
    background: 'transparent',
    fontFamily: 'IBM Plex Sans, Segoe UI, sans-serif',
    fontSize: '15px',
    primaryColor: '#1a1a1a',
    primaryTextColor: '#f3efe8',
    primaryBorderColor: 'rgba(243, 239, 232, 0.22)',
    secondaryColor: '#161616',
    secondaryTextColor: '#f3efe8',
    secondaryBorderColor: 'rgba(243, 239, 232, 0.18)',
    tertiaryColor: '#141414',
    tertiaryTextColor: '#f3efe8',
    tertiaryBorderColor: 'rgba(243, 239, 232, 0.14)',
    lineColor: 'rgba(243, 239, 232, 0.42)',
    textColor: '#f3efe8',
    mainBkg: '#1a1a1a',
    nodeBorder: 'rgba(243, 239, 232, 0.22)',
    clusterBkg: '#111111',
    clusterBorder: 'rgba(243, 239, 232, 0.12)',
    titleColor: '#f3efe8',
    edgeLabelBackground: '#0b0b0b',
    labelTextColor: 'rgba(243, 239, 232, 0.64)',
  },
})

const nodes = [...document.querySelectorAll('.doc-content pre.mermaid')]

try {
  await mermaid.run({ querySelector: '.doc-content pre.mermaid' })
} catch {
  for (const node of nodes) {
    if (!node.querySelector('svg')) node.classList.add('mermaid-failed')
  }
}

for (const node of nodes) {
  if (!node.querySelector('svg') && !node.classList.contains('mermaid-failed')) {
    node.classList.add('mermaid-failed')
  }
}
