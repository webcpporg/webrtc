// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// The interop lane's driver (Jamfile): webcpp::webrtc's peers meet through the relay, and every
// part's verdict is read here.
//
//   node run.mjs --native <native>
//       the relay, a native answer and a native offer, three runs of native.cpp's program
//   node run.mjs --browser <browser_peer.js>
//       the relay and a native offer, whose answer is browser_peer.cpp's program in page.html,
//       in headless Chromium, once in each suspension variant of its manifest
//
// b2's driven tests pass the program they linked last: the native program, or the JSPI
// artifact of the page's program, beside which its manifest and the Asyncify artifact lie. The
// browser run meets the native program that WEBCPP_WEBRTC_INTEROP_NATIVE names, else the newest
// native-program a native build left under bin/libs/webrtc/test/interop (Jamfile).
//
// Every address is 127.0.0.1: the relay listens there, the native peer binds its candidates there
// and has no STUN server, the page is served from there, and Chromium resolves no other host. It
// runs headless, through Playwright (package.json pins it), with the two switches without which
// a peer in it cannot meet a native one on the loopback: --allow-loopback-in-peer-connection,
// for a candidate on 127.0.0.1, and --disable-features=WebRtcHideLocalIpsWithMdns, for its host
// candidates as addresses rather than mDNS names, which the native peer cannot resolve. Both are
// checked on the candidates the page sent. Chromium is the one CHROME names, else, on macOS, the
// newest full Chromium in Playwright's cache.
//
// What a run starts is gone when it ends, however it ends: its verdict, a failure, or SIGTERM,
// SIGINT or SIGHUP (the bound of b2's driven test sends SIGTERM).
import assert from 'node:assert/strict'
import {spawn} from 'node:child_process'
import {once} from 'node:events'
import {existsSync, readdirSync, readFileSync, statSync} from 'node:fs'
import {createServer} from 'node:http'
import {homedir} from 'node:os'
import {dirname, extname, join, resolve, sep} from 'node:path'
import {createInterface} from 'node:readline'
import {setTimeout as delay} from 'node:timers/promises'
import {fileURLToPath} from 'node:url'
import {chromium} from 'playwright'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '../../../..')
const usage = 'usage: node run.mjs --native <native> | --browser <browser_peer.js>'
const [mode, program, ...rest] = process.argv.slice(2)
assert.ok(['--native', '--browser'].includes(mode) && program && rest.length === 0, usage)

// How long each part may take to reach its verdict, past which the run fails naming it.
const listening = 10_000
const exchange = 90_000
const leaving = 10_000

// ---- what a run leaves behind, removed however it ends ------------------------------------------
const left = {parts: new Set(), browser: null, server: null}
let cleaning
const cleanUp = () => (cleaning ??= (async () => {
    for (const part of left.parts) part.stop()
    await left.browser?.close().catch(() => {})
    left.server?.closeAllConnections()
    left.server?.close()
})())
for (const [signal, number] of [['SIGHUP', 1], ['SIGINT', 2], ['SIGTERM', 15]]) {
    process.on(signal, () => cleanUp().finally(() => process.exit(128 + number)))
}

// ---- the native program's parts ---------------------------------------------------------------
// A part is one process of the native program: what it prints, a line each, is echoed and kept.
const start = (name, file, args) => {
    const child = spawn(file, args, {stdio: ['ignore', 'pipe', 'pipe']})
    const part = {name, lines: [], waiters: new Set(), status: null}
    part.exited = new Promise(done => child.on('close', (code, signal) => {
        part.status = signal ? `signal ${signal}` : `status ${code}`
        left.parts.delete(part)
        for (const waiter of part.waiters) waiter()
        done(part.status)
    }))
    child.on('error', error => {
        part.lines.push(`${name}: could not start: ${error.message}`)
        for (const waiter of part.waiters) waiter()
    })
    const keep = (stream, suffix) => createInterface({input: stream}).on('line', line => {
        const kept = suffix ? `${name} ${suffix}: ${line}` : line
        part.lines.push(kept)
        console.log(kept)
        for (const waiter of part.waiters) waiter()
    })
    keep(child.stdout, '')
    keep(child.stderr, '(stderr)')
    part.stop = () => {
        if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL')
    }
    left.parts.add(part)
    return part
}

// What `promise` gives, or `late` once `ms` have passed. Its timer goes as soon as either has
// happened, so that no timer keeps Node alive past the run.
const within = async (promise, ms, late) => {
    const abort = new AbortController()
    try {
        return await Promise.race([promise, delay(ms, late, {signal: abort.signal})])
    } finally {
        abort.abort()
    }
}

// Waits until `found` finds what it looks for in what `part` printed, or the part has exited, or
// `ms` have passed; fails naming `what` unless it was found.
const waitFor = async (part, what, found, ms) => {
    let wake
    const woken = () => wake?.()
    part.waiters.add(woken)
    try {
        const deadline = Date.now() + ms
        for (;;) {
            const result = found(part.lines)
            if (result) return result
            if (part.status) throw new Error(`${part.name} exited with ${part.status} before ${what}`)
            const remaining = deadline - Date.now()
            if (remaining <= 0) throw new Error(`${part.name}: no ${what} within ${ms / 1000} s`)
            await within(new Promise(resolve => { wake = resolve }), remaining)
        }
    } finally {
        part.waiters.delete(woken)
    }
}

// Waits until `part` has exited, and fails unless its status was 0.
const ended = async (part, ms) => {
    const status = await within(part.exited, ms, null)
    assert.ok(status, `${part.name} did not exit within ${ms / 1000} s`)
    assert.equal(status, 'status 0', `${part.name} exited with ${status}`)
}

const startRelay = async () => {
    const relay = start('relay', native(), ['relay'])
    const [, port] = await waitFor(relay, 'port', lines => lines
        .map(line => /^relay: listening on 127\.0\.0\.1:([0-9]+)$/.exec(line)).find(Boolean), listening)
    return {relay, port}
}

// The address of each candidate a peer sent, from the lines `<who> <role>: sent the candidate
// candidate:<foundation> <component> <protocol> <priority> <address> <port> typ <type> ...`.
const sentAddresses = (lines, who) => {
    const prefix = `${who}: sent the candidate `
    return lines.filter(line => line.startsWith(prefix))
        .map(line => line.slice(prefix.length).split(' ')[4])
}

// The verdict a peer printed, and the lines the exchange needs: what it received, and that the
// native peer's candidates are on the loopback alone.
const checkPeer = (lines, who, received) => {
    const verdict = lines.find(line => line.startsWith(`${who}: passed`) || line.startsWith(`${who}: failed`))
    assert.equal(verdict, `${who}: passed`, `${who} did not pass`)
    assert.ok(lines.includes(`${who}: received ${received}`), `${who} did not receive ${received}`)
    const addresses = sentAddresses(lines, who)
    assert.ok(addresses.length > 0, `${who} sent no candidate`)
    return addresses
}

const checkNative = (lines, who, received) => {
    const addresses = checkPeer(lines, who, received)
    assert.deepEqual([...new Set(addresses)], ['127.0.0.1'],
        `${who} sent candidates off the loopback: ${addresses}`)
}

// ---- natively: two native peers through the relay ---------------------------------------------
const native = () => (mode === '--native' ? resolve(program) : nativeProgram())

const runNative = async () => {
    console.log('interop native: the relay, a native answer and a native offer')
    const {relay, port} = await startRelay()
    const answer = start('native answer', native(), ['answer', port])
    const offer = start('native offer', native(), ['offer', port])
    await Promise.all([ended(answer, exchange), ended(offer, exchange)])
    checkNative(offer.lines, 'native offer', 'pong')
    checkNative(answer.lines, 'native answer', 'ping')
    await ended(relay, leaving)
    console.log('interop native: passed')
}

// ---- in the browser: the native peer and the page's ----------------------------------------------
// The native program a native build made: the newest native-program under its build directory,
// at any depth, unless WEBCPP_WEBRTC_INTEROP_NATIVE names one.
function nativeProgram() {
    if (process.env.WEBCPP_WEBRTC_INTEROP_NATIVE) return resolve(process.env.WEBCPP_WEBRTC_INTEROP_NATIVE)
    const found = []
    const look = directory => {
        if (!existsSync(directory)) return
        for (const entry of readdirSync(directory, {withFileTypes: true})) {
            const path = join(directory, entry.name)
            if (entry.isDirectory()) look(path)
            else if (['native-program', 'native-program.exe'].includes(entry.name)) found.push(path)
        }
    }
    look(join(root, 'bin/libs/webrtc/test/interop'))
    assert.ok(found.length > 0, 'no native program: build libs/webrtc/test/interop//native-program'
        + ' natively first (b2 threading=multi libs/webrtc/test/interop//native-program),'
        + ' or name it in WEBCPP_WEBRTC_INTEROP_NATIVE')
    return found.sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs)[0]
}

const findChrome = () => {
    if (process.env.CHROME) return process.env.CHROME
    assert.equal(process.platform, 'darwin', 'set CHROME to the Chrome executable')
    const cache = join(homedir(), 'Library/Caches/ms-playwright')
    const builds = existsSync(cache) ? readdirSync(cache)
        .filter(name => /^chromium-[0-9]+$/.test(name))
        .sort((a, b) => Number(b.split('-')[1]) - Number(a.split('-')[1])) : []
    for (const build of builds) {
        for (const arch of ['arm64', 'x64']) {
            const path = join(cache, build, `chrome-mac-${arch}`, 'Google Chrome for Testing.app',
                'Contents/MacOS/Google Chrome for Testing')
            if (existsSync(path)) return path
        }
    }
    throw new Error(`no Chromium in ${cache}; set CHROME`)
}

// The page, the page's program and webcpp::asio's loader, from 127.0.0.1.
const types = {'.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
    '.wasm': 'application/wasm', '.json': 'application/json'}
const serve = async () => {
    const roots = {'/build/': dirname(resolve(program)),
        '/loader/': join(root, 'libs/asio/include/webcpp/asio'), '/': here}
    const server = createServer((request, response) => {
        const path = decodeURIComponent(new URL(request.url, 'http://127.0.0.1').pathname)
        const [prefix, directory] = Object.entries(roots).find(([prefix]) => path.startsWith(prefix))
        const file = resolve(directory, path.slice(prefix.length))
        if (!file.startsWith(directory + sep) || !existsSync(file) || !statSync(file).isFile()) {
            response.writeHead(404).end()
            return
        }
        response.writeHead(200, {'Content-Type': types[extname(file)] ?? 'application/octet-stream',
            'Cache-Control': 'no-store'}).end(readFileSync(file))
    })
    left.server = server
    server.listen(0, '127.0.0.1')
    await once(server, 'listening')
    return `http://127.0.0.1:${server.address().port}`
}

const runBrowser = async () => {
    const executable = findChrome()
    console.log(`interop browser: the native program ${native()}`)
    const base = await serve()
    left.browser = await chromium.launch({
        executablePath: executable,
        args: [
            '--allow-loopback-in-peer-connection',
            '--disable-features=WebRtcHideLocalIpsWithMdns',
            // No request of Chromium's own leaves the machine: no host resolves but 127.0.0.1.
            '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1',
            // Chromium's sandbox refuses to start as root, as in a container.
            ...(process.getuid?.() === 0 ? ['--no-sandbox'] : []),
        ],
    })
    console.log(`interop browser: ${executable}, version ${left.browser.version()}`)
    for (const variant of ['jspi', 'asyncify']) {
        console.log(`interop browser (${variant}): the relay, a native offer and the page's answer`)
        const {relay, port} = await startRelay()
        const offer = start('native offer', native(), ['offer', port])
        const context = await left.browser.newContext()
        // Without JSPI, webcpp::asio's loader picks the Asyncify artifact.
        if (variant === 'asyncify') await context.addInitScript({content: 'delete WebAssembly.Suspending'})
        const page = await context.newPage()
        const problems = []
        page.on('pageerror', error => problems.push(`exception: ${error.message}`))
        page.on('console', message => {
            if (message.type() === 'error') problems.push(`console.error: ${message.text()}`)
        })
        try {
            const query = new URLSearchParams({manifest: '/build/browser_peer.variants.json',
                role: 'answer', relay: port})
            await page.goto(`${base}/page.html?${query}`)
            const verdict = /^browser answer: (passed|failed)/
            await page.waitForFunction(pattern => window.report.error
                || window.report.lines.some(line => new RegExp(pattern).test(line)),
            verdict.source, {timeout: exchange, polling: 100})
                .catch(() => {})
            const report = await page.evaluate(() => window.report)
            for (const line of report.lines) console.log(line)
            assert.ok(!report.error, `the page failed: ${report.error}`)
            assert.deepEqual(problems, [], 'Chromium reported an exception or a console.error')
            assert.equal(report.variant, variant, `the page loaded ${report.variant}, not ${variant}`)
            const addresses = checkPeer(report.lines, 'browser answer', 'ping')
            assert.ok(addresses.includes('127.0.0.1'), 'the page sent no candidate on 127.0.0.1:'
                + ` --allow-loopback-in-peer-connection had no effect (${addresses})`)
            assert.ok(!addresses.some(address => address.endsWith('.local')), 'the page sent mDNS'
                + ` candidates: --disable-features=WebRtcHideLocalIpsWithMdns had no effect (${addresses})`)
            await ended(offer, exchange)
            checkNative(offer.lines, 'native offer', 'pong')
            await ended(relay, leaving)
            console.log(`interop browser (${variant}): passed`)
        } finally {
            await context.close()
            offer.stop()
            relay.stop()
        }
    }
}

try {
    if (mode === '--native') await runNative()
    else await runBrowser()
} catch (error) {
    console.error(error)
    process.exitCode = 1
} finally {
    await cleanUp()
}
