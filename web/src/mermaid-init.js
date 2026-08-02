import mermaid from 'mermaid'

mermaid.initialize({
  startOnLoad: false,
  theme: 'dark',
  securityLevel: 'strict',
  fontFamily: 'IBM Plex Sans, Segoe UI, sans-serif',
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
