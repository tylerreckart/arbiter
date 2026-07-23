import * as THREE from 'https://cdn.jsdelivr.net/npm/three@0.128.0/build/three.module.js'

// Ported from Cameron Knight's "Interactive Liquid Gradient" (Scheme 5),
// without pointer / touch interaction. Motion slowed for a marketing hero.
// Uses Three r128 like the pen — newer Three color-management washed the look.
// https://codepen.io/cameronknight/pen/ogxWmBP

const VERT = `
varying vec2 vUv;
void main() {
  vec3 pos = position.xyz;
  gl_Position = projectionMatrix * modelViewMatrix * vec4(pos, 1.);
  vUv = uv;
}
`

const FRAG = `
uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uColor1;
uniform vec3 uColor2;
uniform vec3 uColor3;
uniform vec3 uColor4;
uniform vec3 uColor5;
uniform vec3 uColor6;
uniform float uSpeed;
uniform float uIntensity;
uniform float uGrainIntensity;
uniform vec3 uDarkNavy;
uniform float uGradientSize;
uniform float uGradientCount;
uniform float uColor1Weight;
uniform float uColor2Weight;

varying vec2 vUv;

float grain(vec2 uv, float time) {
  vec2 grainUv = uv * uResolution * 0.5;
  float grainValue = fract(sin(dot(grainUv + time, vec2(12.9898, 78.233))) * 43758.5453);
  return grainValue * 2.0 - 1.0;
}

vec3 getGradientColor(vec2 uv, float time) {
  float gradientRadius = uGradientSize;

  vec2 center1 = vec2(
    0.5 + sin(time * uSpeed * 0.4) * 0.4,
    0.5 + cos(time * uSpeed * 0.5) * 0.4
  );
  vec2 center2 = vec2(
    0.5 + cos(time * uSpeed * 0.6) * 0.5,
    0.5 + sin(time * uSpeed * 0.45) * 0.5
  );
  vec2 center3 = vec2(
    0.5 + sin(time * uSpeed * 0.35) * 0.45,
    0.5 + cos(time * uSpeed * 0.55) * 0.45
  );
  vec2 center4 = vec2(
    0.5 + cos(time * uSpeed * 0.5) * 0.4,
    0.5 + sin(time * uSpeed * 0.4) * 0.4
  );
  vec2 center5 = vec2(
    0.5 + sin(time * uSpeed * 0.7) * 0.35,
    0.5 + cos(time * uSpeed * 0.6) * 0.35
  );
  vec2 center6 = vec2(
    0.5 + cos(time * uSpeed * 0.45) * 0.5,
    0.5 + sin(time * uSpeed * 0.65) * 0.5
  );
  vec2 center7 = vec2(
    0.5 + sin(time * uSpeed * 0.55) * 0.38,
    0.5 + cos(time * uSpeed * 0.48) * 0.42
  );
  vec2 center8 = vec2(
    0.5 + cos(time * uSpeed * 0.65) * 0.36,
    0.5 + sin(time * uSpeed * 0.52) * 0.44
  );
  vec2 center9 = vec2(
    0.5 + sin(time * uSpeed * 0.42) * 0.41,
    0.5 + cos(time * uSpeed * 0.58) * 0.39
  );
  vec2 center10 = vec2(
    0.5 + cos(time * uSpeed * 0.48) * 0.37,
    0.5 + sin(time * uSpeed * 0.62) * 0.43
  );
  vec2 center11 = vec2(
    0.5 + sin(time * uSpeed * 0.68) * 0.33,
    0.5 + cos(time * uSpeed * 0.44) * 0.46
  );
  vec2 center12 = vec2(
    0.5 + cos(time * uSpeed * 0.38) * 0.39,
    0.5 + sin(time * uSpeed * 0.56) * 0.41
  );

  float dist1 = length(uv - center1);
  float dist2 = length(uv - center2);
  float dist3 = length(uv - center3);
  float dist4 = length(uv - center4);
  float dist5 = length(uv - center5);
  float dist6 = length(uv - center6);
  float dist7 = length(uv - center7);
  float dist8 = length(uv - center8);
  float dist9 = length(uv - center9);
  float dist10 = length(uv - center10);
  float dist11 = length(uv - center11);
  float dist12 = length(uv - center12);

  float influence1 = 1.0 - smoothstep(0.0, gradientRadius, dist1);
  float influence2 = 1.0 - smoothstep(0.0, gradientRadius, dist2);
  float influence3 = 1.0 - smoothstep(0.0, gradientRadius, dist3);
  float influence4 = 1.0 - smoothstep(0.0, gradientRadius, dist4);
  float influence5 = 1.0 - smoothstep(0.0, gradientRadius, dist5);
  float influence6 = 1.0 - smoothstep(0.0, gradientRadius, dist6);
  float influence7 = 1.0 - smoothstep(0.0, gradientRadius, dist7);
  float influence8 = 1.0 - smoothstep(0.0, gradientRadius, dist8);
  float influence9 = 1.0 - smoothstep(0.0, gradientRadius, dist9);
  float influence10 = 1.0 - smoothstep(0.0, gradientRadius, dist10);
  float influence11 = 1.0 - smoothstep(0.0, gradientRadius, dist11);
  float influence12 = 1.0 - smoothstep(0.0, gradientRadius, dist12);

  vec2 rotatedUv1 = uv - 0.5;
  float angle1 = time * uSpeed * 0.15;
  rotatedUv1 = vec2(
    rotatedUv1.x * cos(angle1) - rotatedUv1.y * sin(angle1),
    rotatedUv1.x * sin(angle1) + rotatedUv1.y * cos(angle1)
  );
  rotatedUv1 += 0.5;

  vec2 rotatedUv2 = uv - 0.5;
  float angle2 = -time * uSpeed * 0.12;
  rotatedUv2 = vec2(
    rotatedUv2.x * cos(angle2) - rotatedUv2.y * sin(angle2),
    rotatedUv2.x * sin(angle2) + rotatedUv2.y * cos(angle2)
  );
  rotatedUv2 += 0.5;

  float radialGradient1 = length(rotatedUv1 - 0.5);
  float radialGradient2 = length(rotatedUv2 - 0.5);
  float radialInfluence1 = 1.0 - smoothstep(0.0, 0.8, radialGradient1);
  float radialInfluence2 = 1.0 - smoothstep(0.0, 0.8, radialGradient2);

  vec3 color = vec3(0.0);
  color += uColor1 * influence1 * (0.55 + 0.45 * sin(time * uSpeed)) * uColor1Weight;
  color += uColor2 * influence2 * (0.55 + 0.45 * cos(time * uSpeed * 1.2)) * uColor2Weight;
  color += uColor3 * influence3 * (0.55 + 0.45 * sin(time * uSpeed * 0.8)) * uColor1Weight;
  color += uColor4 * influence4 * (0.55 + 0.45 * cos(time * uSpeed * 1.3)) * uColor2Weight;
  color += uColor5 * influence5 * (0.55 + 0.45 * sin(time * uSpeed * 1.1)) * uColor1Weight;
  color += uColor6 * influence6 * (0.55 + 0.45 * cos(time * uSpeed * 0.9)) * uColor2Weight;

  if (uGradientCount > 6.0) {
    color += uColor1 * influence7 * (0.55 + 0.45 * sin(time * uSpeed * 1.4)) * uColor1Weight;
    color += uColor2 * influence8 * (0.55 + 0.45 * cos(time * uSpeed * 1.5)) * uColor2Weight;
    color += uColor3 * influence9 * (0.55 + 0.45 * sin(time * uSpeed * 1.6)) * uColor1Weight;
    color += uColor4 * influence10 * (0.55 + 0.45 * cos(time * uSpeed * 1.7)) * uColor2Weight;
  }
  if (uGradientCount > 10.0) {
    color += uColor5 * influence11 * (0.55 + 0.45 * sin(time * uSpeed * 1.8)) * uColor1Weight;
    color += uColor6 * influence12 * (0.55 + 0.45 * cos(time * uSpeed * 1.9)) * uColor2Weight;
  }

  color += mix(uColor1, uColor3, radialInfluence1) * 0.45 * uColor1Weight;
  color += mix(uColor2, uColor4, radialInfluence2) * 0.4 * uColor2Weight;

  color = clamp(color, vec3(0.0), vec3(1.0)) * uIntensity;

  float luminance = dot(color, vec3(0.299, 0.587, 0.114));
  color = mix(vec3(luminance), color, 1.35);
  color = pow(color, vec3(0.92));

  float brightness1 = length(color);
  float mixFactor1 = max(brightness1 * 1.2, 0.15);
  color = mix(uDarkNavy, color, mixFactor1);

  float maxBrightness = 1.0;
  float brightness = length(color);
  if (brightness > maxBrightness) {
    color = color * (maxBrightness / brightness);
  }

  return color;
}

void main() {
  vec2 uv = vUv;
  vec3 color = getGradientColor(uv, uTime);

  float grainValue = grain(uv, uTime);
  color += grainValue * uGrainIntensity;

  float timeShift = uTime * 0.5;
  color.r += sin(timeShift) * 0.02;
  color.g += cos(timeShift * 1.4) * 0.02;
  color.b += sin(timeShift * 1.2) * 0.02;

  float brightness2 = length(color);
  float mixFactor2 = max(brightness2 * 1.2, 0.15);
  color = mix(uDarkNavy, color, mixFactor2);

  color = clamp(color, vec3(0.0), vec3(1.0));

  float maxBrightness = 1.0;
  float brightness = length(color);
  if (brightness > maxBrightness) {
    color = color * (maxBrightness / brightness);
  }

  // Soft dissolve into page black at the bottom (vUv.y = 0).
  // Ease keeps color longer, then falls off cleanly — no muddy overlay.
  float keep = smoothstep(0.0, 0.58, vUv.y);
  keep = keep * keep * (3.0 - 2.0 * keep);
  vec3 pageBlack = vec3(0.043); // #0b0b0b
  color = mix(pageBlack, color, keep);

  gl_FragColor = vec4(color, 1.0);
}
`

function startLiquidHero(root) {
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches

  // Scheme 5: F15A22 + 004238 + F15A22 + 000000 + F15A22 + 000000
  const orange = new THREE.Vector3(0.945, 0.353, 0.133) // #F15A22
  const teal = new THREE.Vector3(0.0, 0.259, 0.22) //rgb(255, 187, 0)
  const black = new THREE.Vector3(0.0, 0.0, 0.0) // #000000
  const navy = new THREE.Vector3(0.039, 0.055, 0.153) // #0a0e27

  const renderer = new THREE.WebGLRenderer({
    antialias: true,
    alpha: false,
    powerPreference: 'high-performance',
    stencil: false,
    depth: false,
  })
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2))
  root.appendChild(renderer.domElement)
  renderer.domElement.setAttribute('aria-hidden', 'true')

  const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 10000)
  camera.position.z = 50
  const scene = new THREE.Scene()
  scene.background = new THREE.Color(0x0a0e27)

  const getViewSize = () => {
    const fovInRadians = (camera.fov * Math.PI) / 180
    const height = Math.abs(camera.position.z * Math.tan(fovInRadians / 2) * 2)
    return { width: height * camera.aspect, height }
  }

  const uniforms = {
    uTime: { value: 4.2 },
    uResolution: { value: new THREE.Vector2(1, 1) },
    uColor1: { value: orange.clone() },
    uColor2: { value: teal.clone() },
    uColor3: { value: orange.clone() },
    uColor4: { value: black.clone() },
    uColor5: { value: orange.clone() },
    uColor6: { value: black.clone() },
    // Pen uses 1.5; slow drift for the marketing hero.
    uSpeed: { value: reducedMotion ? 0 : 0.28 },
    uIntensity: { value: 1.45 },
    uGrainIntensity: { value: 0.08 },
    uDarkNavy: { value: navy.clone() },
    uGradientSize: { value: 0.45 },
    uGradientCount: { value: 12.0 },
    uColor1Weight: { value: 0.55 },
    uColor2Weight: { value: 1.6 },
  }

  const material = new THREE.ShaderMaterial({
    uniforms,
    vertexShader: VERT,
    fragmentShader: FRAG,
  })

  let mesh = null
  const clock = new THREE.Clock()
  let frameId = 0
  let running = true

  const rebuildMesh = () => {
    const viewSize = getViewSize()
    const geometry = new THREE.PlaneGeometry(viewSize.width, viewSize.height, 1, 1)
    if (mesh) {
      mesh.geometry.dispose()
      mesh.geometry = geometry
    } else {
      mesh = new THREE.Mesh(geometry, material)
      mesh.position.z = 0
      scene.add(mesh)
    }
  }

  const resize = () => {
    const width = Math.max(1, root.clientWidth)
    const height = Math.max(1, root.clientHeight)
    camera.aspect = width / height
    camera.updateProjectionMatrix()
    renderer.setSize(width, height, false)
    uniforms.uResolution.value.set(width, height)
    rebuildMesh()
  }

  const tick = () => {
    if (!running) return
    const delta = Math.min(clock.getDelta(), 0.05)
    if (!reducedMotion) uniforms.uTime.value += delta
    renderer.render(scene, camera)
    frameId = window.requestAnimationFrame(tick)
  }

  const onVisibility = () => {
    if (document.hidden) {
      running = false
      window.cancelAnimationFrame(frameId)
      return
    }
    if (!running) {
      running = true
      clock.getDelta()
      frameId = window.requestAnimationFrame(tick)
    }
  }

  resize()
  tick()

  const observer = new ResizeObserver(resize)
  observer.observe(root)
  document.addEventListener('visibilitychange', onVisibility)
  window.addEventListener(
    'pagehide',
    () => {
      running = false
      window.cancelAnimationFrame(frameId)
      observer.disconnect()
      material.dispose()
      mesh?.geometry.dispose()
      renderer.dispose()
    },
    { once: true },
  )
}

const root = document.querySelector('[data-hero-liquid]')
if (root) startLiquidHero(root)
