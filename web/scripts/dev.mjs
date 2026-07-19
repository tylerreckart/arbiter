import { spawn } from 'node:child_process'
import path from 'node:path'
import browserSyncPackage from 'browser-sync'
import chokidar from 'chokidar'
import {
  assetsPath,
  dist,
  docsRoot,
  installPath,
  root,
  siteScriptPath,
  stylesPath,
  viewsRoot,
} from '../lib/config.mjs'

const browserSync = browserSyncPackage.create()
const buildScript = path.join(root, 'scripts', 'build.mjs')
const port = parsePort(process.env.PORT)
const debounceMs = 100

let debounceTimer
let building = false
let rebuildQueued = false
let shuttingDown = false

const watchPaths = [
  viewsRoot,
  path.join(root, 'lib'),
  stylesPath,
  siteScriptPath,
  installPath,
  docsRoot,
  assetsPath,
]

if (!(await runBuild())) {
  process.exitCode = 1
} else {
  await startServer()
  const watcher = chokidar.watch(watchPaths, {
    awaitWriteFinish: {
      pollInterval: 25,
      stabilityThreshold: 75,
    },
    ignoreInitial: true,
  })

  watcher.on('all', (eventName, changedPath) => {
    scheduleRebuild(`${eventName}: ${path.relative(root, changedPath)}`)
  })
  watcher.on('error', (error) => {
    console.error('[watch] error:', error)
  })

  const shutdown = async () => {
    if (shuttingDown) return
    shuttingDown = true
    clearTimeout(debounceTimer)
    await watcher.close()
    browserSync.exit()
  }

  process.once('SIGINT', shutdown)
  process.once('SIGTERM', shutdown)

  console.log(
    `[watch] views, build modules, styles, scripts, docs, and assets`,
  )
}

function parsePort(value) {
  if (value === undefined) return 4173
  const parsed = Number.parseInt(value, 10)
  if (!Number.isInteger(parsed) || parsed < 1 || parsed > 65535) {
    throw new Error(`Invalid PORT: ${value}`)
  }
  return parsed
}

function scheduleRebuild(reason) {
  clearTimeout(debounceTimer)
  debounceTimer = setTimeout(() => {
    void rebuild(reason)
  }, debounceMs)
}

async function rebuild(reason) {
  if (building) {
    rebuildQueued = true
    return
  }

  building = true
  console.log(`\n[watch] ${reason}`)
  const succeeded = await runBuild()
  building = false

  if (succeeded) {
    browserSync.reload()
  } else {
    console.error('[watch] build failed; waiting for the next change')
  }

  if (rebuildQueued) {
    rebuildQueued = false
    await rebuild('changes queued during build')
  }
}

function runBuild() {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [buildScript], {
      cwd: root,
      env: process.env,
      stdio: 'inherit',
    })

    child.once('error', (error) => {
      console.error('[build] failed to start:', error)
      resolve(false)
    })
    child.once('close', (code) => {
      resolve(code === 0)
    })
  })
}

function startServer() {
  return new Promise((resolve, reject) => {
    browserSync.init(
      {
        files: false,
        ghostMode: false,
        logFileChanges: false,
        logPrefix: 'arbiter-web',
        notify: false,
        open: false,
        port,
        server: {
          baseDir: dist,
        },
        ui: false,
      },
      (error) => {
        if (error) {
          reject(error)
          return
        }
        resolve()
      },
    )
  })
}
