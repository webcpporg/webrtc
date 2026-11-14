// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// Drives one program of webrtc's browser lane in headless Chrome, in both suspension variants.
// node run.mjs [--program name] [--expected line] [--timeout seconds] <artifact>
// The artifact is the program's jspi JavaScript, which webcpp.drive gives; the manifest beside it,
// or beside its twin built with exceptions, names both variants, and page.html loads each. The
// program passes when it prints the expected line and nothing on its standard error but
// lightweight_test's own summary. CHROME names the Chrome executable; on macOS the newest cached
// Playwright Chromium is used when it is unset. Each run owns its loopback server, its Chrome
// process and its temporary profile.
//
// The two peers of a test live in one page and use host candidates only, with no STUN server,
// so that nothing leaves the machine. Chrome is started with one flag for them:
//
// --disable-features=WebRtcHideLocalIpsWithMdns: Chrome hides a host candidate's address behind
//   a random mDNS name (<uuid>.local) unless the page may capture media, and the other peer must
//   then resolve that name through the system's multicast DNS responder, which a CI container or
//   a sandboxed runner may lack. With the feature off a host candidate carries its address, which
//   the other peer of the page reaches directly. The control below checks that it does.
//
// --allow-loopback-in-peer-connection, which the interop lane needs for a native peer bound to
// 127.0.0.1, is not given: measured with Chrome 156 and chrome-headless-shell 156, a page that may
// not capture media gathers on the interface of the default route alone, with or without it, and
// two peers of one page need nothing more. --host-resolver-rules resolves no name but 127.0.0.1,
// so that nothing Chrome would look up in the background leaves the machine either.
import assert from 'node:assert/strict'
import {spawn} from 'node:child_process'
import {once} from 'node:events'
import {existsSync, mkdtempSync, readFileSync, readdirSync, rmSync} from 'node:fs'
import {createServer} from 'node:http'
import {homedir, tmpdir} from 'node:os'
import {dirname, extname, join, resolve, sep} from 'node:path'
import {setTimeout as delay} from 'node:timers/promises'
import {fileURLToPath} from 'node:url'

const args = process.argv.slice(2)
let program = 'browser_test'
let expected = 'browser webrtc: ok'
let timeout = 60
while (args[0]?.startsWith('--')) {
    const option = args.shift()
    if (option === '--program') program = args.shift()
    else if (option === '--expected') expected = args.shift()
    else if (option === '--timeout') timeout = Number(args.shift())
    else throw new Error(`unknown option ${option}`)
}
assert.equal(args.length, 1,
    'usage: run.mjs [--program name] [--expected line] [--timeout seconds] <artifact>')
assert.ok(Number.isFinite(timeout) && timeout > 0, '--timeout takes a number of seconds')
const here = dirname(fileURLToPath(import.meta.url))
const artifact = resolve(args[0])

// The manifest of webcpp.variants is beside the jspi artifact, since the Jamfile gives each drive
// its program's requirements, and the asyncify artifact is below it.
const build = dirname(artifact)
const manifest = join(build, `${program}.variants.json`)
assert.ok(existsSync(manifest), `no manifest ${manifest} beside ${artifact}: give the drive `
    + 'the requirements of its program, which are propagated to the manifest')
const manifestUrl = `/build/${program}.variants.json`

const findChrome = () => {
    if (process.env.CHROME) return process.env.CHROME
    assert.equal(process.platform, 'darwin', 'set CHROME to the Chrome executable')
    const cache = join(homedir(), 'Library/Caches/ms-playwright')
    const builds = existsSync(cache) ? readdirSync(cache)
        .filter(name => /^chromium-\d+$/.test(name))
        .sort((a, b) => Number(b.split('-').at(-1)) - Number(a.split('-').at(-1))) : []
    for (const name of builds) {
        for (const arch of ['arm64', 'x64']) {
            const path = join(cache, name, `chrome-mac-${arch}`, 'Google Chrome for Testing.app',
                'Contents/MacOS/Google Chrome for Testing')
            if (existsSync(path)) return path
        }
    }
    throw new Error('no cached Chromium found; set CHROME')
}

const left = {chrome: null, profile: null, server: null, sockets: new Set()}
const exited = child => child.exitCode !== null || child.signalCode !== null
const signalChrome = (child, signal) => {
    if (!child.pid) return
    try {
        if (process.platform === 'win32') child.kill(signal)
        else process.kill(-child.pid, signal)
    } catch (error) {
        if (error.code !== 'ESRCH' && !(error.code === 'EPERM' && exited(child))) throw error
    }
}
const stopChrome = async () => {
    if (left.chrome && !exited(left.chrome) && left.chrome.pid) {
        const ended = once(left.chrome, 'exit')
        const stubborn = setTimeout(() => signalChrome(left.chrome, 'SIGKILL'), 5000)
        signalChrome(left.chrome, 'SIGTERM')
        try { await ended } finally { clearTimeout(stubborn) }
    }
    // Chrome's helpers may keep the inherited standard error open after the browser exits.
    if (left.chrome) {
        signalChrome(left.chrome, 'SIGKILL')
        left.chrome.stderr.destroy()
    }
    left.chrome = null
    if (left.profile) rmSync(left.profile, {recursive: true, force: true, maxRetries: 3})
    left.profile = null
}
let cleaning
const cleanUp = () => (cleaning ??= (async () => {
    for (const socket of left.sockets) socket.close()
    await stopChrome()
    if (left.server) {
        left.server.closeAllConnections()
        await new Promise(resolve => left.server.close(resolve))
    }
})())
for (const [signal, code] of [['SIGHUP', 129], ['SIGINT', 130], ['SIGTERM', 143]]) {
    process.on(signal, () => cleanUp().finally(() => process.exit(code)))
}

const types = {'.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
    '.wasm': 'application/wasm', '.json': 'application/json'}
const roots = {'/build/': build, '/loader/': resolve(here, '../../../asio/include/webcpp/asio'),
    '/': here}
const startServer = async () => {
    const server = createServer((request, response) => {
        try {
            const path = decodeURIComponent(new URL(request.url, 'http://127.0.0.1').pathname)
            if (path === '/control') {
                response.writeHead(200, {'Content-Type': 'text/html'})
                    .end('<!doctype html><title>webrtc control</title>')
                return
            }
            const [prefix, root] = Object.entries(roots).find(([prefix]) => path.startsWith(prefix))
            const file = resolve(root, path.slice(prefix.length))
            if (!file.startsWith(root + sep)) { response.writeHead(404).end(); return }
            const content = readFileSync(file)
            response.writeHead(200, {'Content-Type': types[extname(file)] ?? 'application/octet-stream',
                'Cross-Origin-Opener-Policy': 'same-origin',
                'Cross-Origin-Embedder-Policy': 'require-corp'}).end(content)
        } catch { response.writeHead(404).end() }
    })
    left.server = server
    server.listen(0, '127.0.0.1')
    await once(server, 'listening')
    return `http://127.0.0.1:${server.address().port}`
}

const startChrome = async () => {
    left.profile = mkdtempSync(join(tmpdir(), `webrtc-browser-${process.pid}-`))
    const chrome = spawn(findChrome(), [
        '--headless=new', '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
        `--user-data-dir=${left.profile}`, '--no-first-run', '--no-default-browser-check',
        '--disable-component-update', '--disable-background-networking', '--disable-sync',
        '--disable-features=WebRtcHideLocalIpsWithMdns',
        '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1', 'about:blank'
    ], {stdio: ['ignore', 'ignore', 'pipe'], detached: process.platform !== 'win32'})
    left.chrome = chrome
    let error
    let said = ''
    chrome.on('error', value => { error = value })
    chrome.stderr.on('data', data => { said = (said + data).slice(-2000) })
    const deadline = Date.now() + 15000
    while (Date.now() < deadline) {
        if (error) throw error
        if (exited(chrome)) throw new Error(`Chrome exited during startup: ${said}`)
        const active = join(left.profile, 'DevToolsActivePort')
        const port = existsSync(active) ? readFileSync(active, 'utf8').split('\n')[0] : ''
        if (/^\d+$/.test(port)) {
            const devtools = `http://127.0.0.1:${port}`
            try {
                const version = await (await fetch(`${devtools}/json/version`,
                    {signal: AbortSignal.timeout(10000)})).json()
                console.log(`browser: ${version.Browser}`)
                return devtools
            } catch {}
        }
        await delay(50)
    }
    throw new Error(`Chrome startup timed out: ${said}`)
}

// A page of its own, driven through the DevTools protocol, which records what the page throws
// and what it writes to console.error.
const openPage = async (devtools, url, {variant = 'jspi'} = {}) => {
    const target = await (await fetch(`${devtools}/json/new?about:blank`,
        {method: 'PUT', signal: AbortSignal.timeout(10000)})).json()
    const ws = new WebSocket(target.webSocketDebuggerUrl)
    left.sockets.add(ws)
    const pending = new Map()
    const problems = []
    let id = 0
    const send = (method, params = {}) => new Promise((resolve, reject) => {
        const current = ++id
        const timer = setTimeout(() => {
            pending.delete(current)
            reject(new Error(`${method} did not answer within 10 s`))
        }, 10000)
        pending.set(current, {resolve, reject, timer})
        ws.send(JSON.stringify({id: current, method, params}))
    })
    ws.addEventListener('message', event => {
        const message = JSON.parse(event.data)
        const waiting = pending.get(message.id)
        if (waiting) {
            clearTimeout(waiting.timer)
            pending.delete(message.id)
            if (message.error) waiting.reject(new Error(JSON.stringify(message.error)))
            else waiting.resolve(message.result)
        }
        if (message.method === 'Runtime.exceptionThrown') {
            const details = message.params.exceptionDetails
            problems.push(details.exception?.description ?? details.text)
        }
        if (message.method === 'Runtime.consoleAPICalled' && message.params.type === 'error') {
            problems.push(message.params.args.map(arg => arg.value ?? arg.description).join(' '))
        }
        if (message.method === 'Inspector.targetCrashed') problems.push('the page crashed')
    })
    ws.addEventListener('close', () => {
        left.sockets.delete(ws)
        for (const entry of pending.values()) {
            clearTimeout(entry.timer)
            entry.reject(new Error('Chrome debugging connection closed'))
        }
        pending.clear()
    })
    await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('Chrome WebSocket connection timed out')), 10000)
        ws.addEventListener('open', () => { clearTimeout(timer); resolve() }, {once: true})
        ws.addEventListener('error', () => {
            clearTimeout(timer)
            reject(new Error('Chrome WebSocket failed'))
        }, {once: true})
    })
    await send('Runtime.enable')
    await send('Page.enable')
    await send('Inspector.enable')
    // A page without JSPI, which the loader reads as an engine that needs Asyncify.
    if (variant === 'asyncify') {
        await send('Page.addScriptToEvaluateOnNewDocument', {source: 'delete WebAssembly.Suspending'})
    }
    await send('Page.navigate', {url})
    const evaluate = async expression => {
        const result = await send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true})
        assert.ok(!result.exceptionDetails, JSON.stringify(result.exceptionDetails))
        return result.result.value
    }
    return {
        problems,
        evaluate,
        async close() {
            ws.close()
            const response = await fetch(`${devtools}/json/close/${target.id}`,
                {signal: AbortSignal.timeout(10000)})
            await response.text()
        }
    }
}

// Two peers in a page of plain JavaScript, before any program: each host candidate carries an
// address, not an mDNS name, and a channel between them opens and carries a message. A Chrome
// that ignores the flags above fails here, by name, rather than as a program that times out.
// Each description carries every candidate, gathered before it is given, so that no candidate
// meets a peer without a remote description.
const control = `(async () => {
    const a = new RTCPeerConnection()
    const b = new RTCPeerConnection()
    const addresses = []
    a.onicecandidate = ({candidate}) => {
        if (candidate && candidate.candidate) addresses.push(candidate.address)
    }
    const gathered = pc => new Promise(resolve => {
        const check = () => { if (pc.iceGatheringState === 'complete') resolve() }
        pc.onicegatheringstatechange = check
        check()
    })
    const channel = a.createDataChannel('control')
    const received = new Promise(resolve => {
        b.ondatachannel = ({channel: remote}) => { remote.onmessage = ({data}) => resolve(data) }
    })
    channel.onopen = () => channel.send('ping')
    const exchange = (async () => {
        await a.setLocalDescription()
        await gathered(a)
        await b.setRemoteDescription(a.localDescription)
        await b.setLocalDescription()
        await gathered(b)
        await a.setRemoteDescription(b.localDescription)
        return received
    })()
    const message = await Promise.race([exchange,
        new Promise(resolve => setTimeout(() => resolve('timeout'), 10000))])
    a.close()
    b.close()
    return {addresses, message}
})()`

let base
try {
    base = await startServer()
    const devtools = await startChrome()
    const page = await openPage(devtools, `${base}/control`)
    try {
        const deadline = Date.now() + 5000
        while (!await page.evaluate('document.readyState === "complete"')) {
            assert.ok(Date.now() < deadline, 'the control page did not load')
            await delay(25)
        }
        const {addresses, message} = await page.evaluate(control)
        assert.ok(addresses.length > 0, 'the control peer gathered no host candidate')
        const named = addresses.filter(address => !/^[0-9.]+$|:/.test(address ?? ''))
        assert.deepEqual(named, [], 'a host candidate carries a name, not an address: Chrome '
            + 'ignored --disable-features=WebRtcHideLocalIpsWithMdns')
        assert.equal(message, 'ping', 'the control peers did not exchange a message')
        assert.deepEqual(page.problems, [])
        console.log(`browser control: host candidates ${[...new Set(addresses)].join(' ')}; `
            + 'a channel carried a message')
    } finally { await page.close() }

    const marker = /^check failed|^error:/
    for (const variant of ['jspi', 'asyncify']) {
        const page = await openPage(devtools, `${base}/page.html?manifest=${manifestUrl}`, {variant})
        let report
        try {
            const deadline = Date.now() + timeout * 1000
            for (;;) {
                report = await page.evaluate('window.report')
                const lines = report?.lines ?? []
                // lightweight_test writes its summary on the standard error; anything else there
                // is a failed check, which fails the run at once.
                const failed = lines.find(entry => marker.test(entry.line)
                    || (entry.stream === 'err' && entry.line !== 'No errors detected.'))
                assert.ok(!report?.error, report?.error)
                assert.deepEqual(page.problems, [], 'Chrome reported an exception or console.error')
                assert.ok(!failed, `${program} (${variant}) failed: ${failed?.line}`)
                if (report?.variant && lines.some(entry => entry.line === expected)) break
                assert.ok(Date.now() < deadline,
                    `${program} (${variant}) did not print "${expected}" within ${timeout} s`)
                await delay(50)
            }
            assert.equal(report.variant, variant)
            console.log(`browser ${variant}: ${program} passed`)
        } catch (error) {
            for (const entry of report?.lines ?? []) {
                console.log(`  ${variant} ${entry.stream}: ${entry.line}`)
            }
            throw error
        } finally { await page.close() }
    }
} catch (error) {
    console.error(error)
    process.exitCode = 1
} finally { await cleanUp() }
