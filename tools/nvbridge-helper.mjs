#!/usr/bin/env node
import fs from 'node:fs/promises';
import fsSync from 'node:fs';
import path from 'node:path';
import { performance } from 'node:perf_hooks';
import { fileURLToPath, pathToFileURL } from 'node:url';

const DEFAULT_API_BASE = 'http://127.0.0.1:8000/api/headless/v1';
const DEFAULT_MAX_DISTANCE = 700;
const DEFAULT_NATIVE_MAX_DISTANCE_METERS = 10;
const DEFAULT_POLL_MS = 750;
const DEFAULT_API_REQUEST_TIMEOUT_MS = 180_000;
const DEFAULT_API_STT_TIMEOUT_MS = 45_000;
const DEFAULT_TTS_SAMPLE_RATE = 44100;
const DEFAULT_STT_FORMAT = 'wav';
const DEFAULT_STT_MIME_TYPE = 'audio/wav';
const DEFAULT_ACTION_BOOK_IDS = Object.freeze(['Fallout New Vegas Action Book']);
const DEFAULT_ACTION_BOOK_TARGET_GAME = 'fallout-new-vegas';
const DEFAULT_NATIVE_ACTION_CONFIDENCE = 0.65;
const DEFAULT_ADMIN_CHARACTER_ID = 'Todd';
const DEFAULT_ADMIN_CHARACTER_NAME = 'Todd';
const DEFAULT_ADMIN_ACTION_BOOK_LIMIT = 12;
const DEFAULT_GAMESTATE_RADIUS_METERS = 30;
const GAME_UNITS_PER_METER = 70;
const TRUSTED_FNV_ACTION_ENGINE = 'fallout-new-vegas:xnvse';
const NATIVE_ACTION_COMMAND_VERSION = 'NVBRIDGE_ACTION_V2';
const REQUEST_SUPERSEDED_CODE = 'NVBRIDGE_REQUEST_SUPERSEDED';
const NATIVE_SPEECH_AUDIO_READY_TIMEOUT_MS = 2_000;
const NATIVE_SPEECH_AUDIO_SETTLE_MS = 35;
const NATIVE_SPEECH_AUDIO_FILE_RETRY_MS = 50;
const NATIVE_SPEECH_AUDIO_FILE_RETRIES = 8;

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

function findRepoRoot(startDir) {
    let current = startDir;
    while (current && current !== path.dirname(current)) {
        if (
            fsSync.existsSync(path.join(current, '.git'))
            || fsSync.existsSync(path.join(current, 'package.json'))
        ) {
            return current;
        }
        current = path.dirname(current);
    }
    return path.resolve(startDir, '..', '..');
}

const repoRoot = findRepoRoot(__dirname);

function parseArgs(argv) {
    const args = {};
    for (let index = 2; index < argv.length; index++) {
        const arg = argv[index];
        if (!arg.startsWith('--')) {
            continue;
        }
        const key = arg.slice(2);
        const value = argv[index + 1] && !argv[index + 1].startsWith('--') ? argv[++index] : 'true';
        args[key] = value;
    }
    return args;
}

async function loadEnvFile(filePath) {
    if (!fsSync.existsSync(filePath)) {
        return;
    }

    const lines = (await fs.readFile(filePath, 'utf8')).split(/\r?\n/);
    for (const line of lines) {
        const match = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$/);
        if (!match || process.env[match[1]]) {
            continue;
        }
        let value = match[2].trim();
        if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
            value = value.slice(1, -1);
        }
        process.env[match[1]] = value;
    }
}

async function readJsonFile(filePath, fallback = {}) {
    if (!filePath || !fsSync.existsSync(filePath)) {
        return fallback;
    }
    return JSON.parse(await fs.readFile(filePath, 'utf8'));
}

function splitList(value) {
    return String(value || '').split(';').map(item => item.trim()).filter(Boolean);
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function getConfigBoolean(value, fallback = false) {
    if (value === undefined || value === null || value === '') {
        return fallback;
    }
    if (typeof value === 'boolean') {
        return value;
    }
    const normalized = String(value).trim().toLowerCase();
    if (['1', 'true', 'yes', 'on'].includes(normalized)) {
        return true;
    }
    if (['0', 'false', 'no', 'off'].includes(normalized)) {
        return false;
    }
    return fallback;
}

function getConfigPositiveNumber(value, fallback) {
    const number = Number(value);
    return Number.isFinite(number) && number > 0 ? number : fallback;
}

function getConfigIntegerInRange(value, fallback, min, max) {
    const number = Math.trunc(Number(value));
    if (!Number.isFinite(number)) {
        return fallback;
    }
    return Math.min(max, Math.max(min, number));
}

function unique(items) {
    return Array.from(new Set(items.filter(Boolean)));
}

function getPlainObject(value) {
    return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function normalizeLookupKey(value) {
    return String(value || '').trim().toLowerCase();
}

function slugLookupKey(value) {
    return normalizeLookupKey(value).replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
}

// Best-effort default install locations, derived from the environment so they
// work for a stock Mod Organizer 2 + Steam layout without hardcoding a username.
// Override any of these explicitly via config (dataRoots / nativeBridgeRoots) or
// the NVBRIDGE_DATA_ROOT(S) / NVBRIDGE_NATIVE_ROOT(S) environment variables.
const LOCAL_APP_DATA = process.env.LOCALAPPDATA || '';
const PROGRAM_FILES_X86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
const MO2_NV_DEFAULT = LOCAL_APP_DATA
    ? path.join(LOCAL_APP_DATA, 'ModOrganizer', 'New Vegas')
    : '';

function getDefaultDataRoots() {
    return unique([
        process.env.NVBRIDGE_DATA_ROOT,
        ...splitList(process.env.NVBRIDGE_DATA_ROOTS),
        MO2_NV_DEFAULT ? path.join(MO2_NV_DEFAULT, 'mods', 'NVBridge') : '',
        MO2_NV_DEFAULT ? path.join(MO2_NV_DEFAULT, 'overwrite') : '',
        path.join(PROGRAM_FILES_X86, 'Steam', 'steamapps', 'common', 'Fallout New Vegas', 'Data'),
    ]);
}

function getDefaultNativeBridgeRoots() {
    return unique([
        process.env.NVBRIDGE_NATIVE_ROOT,
        ...splitList(process.env.NVBRIDGE_NATIVE_ROOTS),
        MO2_NV_DEFAULT ? path.join(MO2_NV_DEFAULT, 'overwrite', 'NVBridge') : '',
    ]);
}

async function loadConfig(args) {
    await loadEnvFile(path.join(repoRoot, '.env'));
    const fileConfig = await readJsonFile(args.config, {});
    const cliDataRoots = unique([args['data-root'], ...splitList(args['data-roots'])]);
    const cliNativeRoots = unique([args['native-root'], ...splitList(args['native-roots'])]);
    const configDataRoots = Array.isArray(fileConfig.dataRoots) ? fileConfig.dataRoots : [];
    const configNativeRoots = Array.isArray(fileConfig.nativeBridgeRoots) ? fileConfig.nativeBridgeRoots : [];
    const configActionBookIds = Array.isArray(fileConfig.actionBookIds)
        ? fileConfig.actionBookIds
        : splitList(fileConfig.actionBookIds);
    const actionBookIds = unique([
        ...splitList(args['action-book-id']),
        ...splitList(args['action-book-ids']),
        ...configActionBookIds,
        ...DEFAULT_ACTION_BOOK_IDS,
    ]);
    const enableActionBooks = args['disable-action-books'] !== undefined
        ? false
        : getConfigBoolean(args['enable-action-books'] ?? fileConfig.enableActionBooks, true);
    const npcCharacterMap = {
        ...getPlainObject(fileConfig.characterMap),
        ...getPlainObject(fileConfig.npcCharacters),
        ...getPlainObject(fileConfig.npcCharacterMap),
    };
    const ttsOverrides = { ...(fileConfig.ttsOverrides || {}) };
    if (args['tts-provider']) ttsOverrides.provider = args['tts-provider'];
    if (args['tts-voice']) ttsOverrides.voice = args['tts-voice'];
    if (args['tts-rate']) ttsOverrides.rate = Number(args['tts-rate']);
    if (args['tts-volume']) ttsOverrides.volume = Number(args['tts-volume']);
    if (args['tts-sample-rate']) ttsOverrides.sampleRate = Number(args['tts-sample-rate']);
    if (args['tts-model-id']) ttsOverrides.modelId = args['tts-model-id'];
    if (args['tts-language']) ttsOverrides.language = args['tts-language'];
    if (args['tts-delivery-mode']) ttsOverrides.deliveryMode = args['tts-delivery-mode'];
    if (args['tts-text-normalization']) ttsOverrides.applyTextNormalization = args['tts-text-normalization'];
    const speechRecognition = { ...(fileConfig.speechRecognition || fileConfig.stt || {}) };
    if (args['stt-provider']) speechRecognition.provider = args['stt-provider'];
    if (args['stt-language']) speechRecognition.language = args['stt-language'];
    if (args['stt-model']) speechRecognition.model = args['stt-model'];
    if (args['stt-model-id']) speechRecognition.modelId = args['stt-model-id'];
    if (args['stt-prompt']) speechRecognition.prompt = args['stt-prompt'];
    if (args['stt-server']) speechRecognition.server = args['stt-server'];

    return {
        apiBase: String(args['api-base'] || fileConfig.apiBase || process.env.HEADLESS_API_BASE || DEFAULT_API_BASE).replace(/\/+$/, ''),
        // Optional separate base for /speech/* (TTS) so it can point at the Rust
        // port while generation/STT stay on SillyTavern. Empty = use apiBase.
        ttsApiBase: String(args['tts-api-base'] || fileConfig.ttsApiBase || process.env.NVBRIDGE_TTS_API_BASE || '').replace(/\/+$/, ''),
        apiKey: String(args['api-key'] || fileConfig.apiKey || process.env.HEADLESS_API_KEY || ''),
        liveChatId: String(args['live-chat-id'] || fileConfig.liveChatId || 'fnv-goodsprings'),
        groupId: String(args['group-id'] || fileConfig.groupId || ''),
        characterId: String(args['character-id'] || fileConfig.characterId || 'Easy Pete'),
        characterName: String(args['character-name'] || fileConfig.characterName || 'Easy Pete'),
        adminCharacterId: String(args['admin-character-id'] || fileConfig.adminCharacterId || DEFAULT_ADMIN_CHARACTER_ID),
        adminCharacterName: String(args['admin-character-name'] || fileConfig.adminCharacterName || DEFAULT_ADMIN_CHARACTER_NAME),
        adminActionBookLimit: Number(args['admin-action-book-limit'] || fileConfig.adminActionBookLimit || DEFAULT_ADMIN_ACTION_BOOK_LIMIT),
        adminSessionId: String(args['admin-session-id'] || fileConfig.adminSessionId || ''),
        participantId: String(args['participant-id'] || fileConfig.participantId || 'player'),
        npcCharacterMap,
        model: String(args.model || fileConfig.model || process.env.NVBRIDGE_MODEL || ''),
        maxDistanceGameUnits: Number(args['max-distance'] || fileConfig.maxDistanceGameUnits || DEFAULT_MAX_DISTANCE),
        nativeMaxDistanceMeters: Number(args['native-max-distance-meters'] || fileConfig.nativeMaxDistanceMeters || DEFAULT_NATIVE_MAX_DISTANCE_METERS),
        gameStateRadiusMeters: Number(args['gamestate-radius-meters'] || args['game-state-radius-meters'] || fileConfig.gameStateRadiusMeters || fileConfig.gamestateRadiusMeters || DEFAULT_GAMESTATE_RADIUS_METERS),
        pollMs: Number(args['poll-ms'] || fileConfig.pollMs || DEFAULT_POLL_MS),
        requestTimeoutMs: getConfigPositiveNumber(args['request-timeout-ms'] || fileConfig.requestTimeoutMs, DEFAULT_API_REQUEST_TIMEOUT_MS),
        speechRecognitionTimeoutMs: getConfigPositiveNumber(
            args['speech-recognition-timeout-ms']
                || args['stt-timeout-ms']
                || fileConfig.speechRecognitionTimeoutMs
                || fileConfig.sttTimeoutMs
                || speechRecognition.timeoutMs
                || speechRecognition.timeout_ms,
            DEFAULT_API_STT_TIMEOUT_MS,
        ),
        traceRequests: getConfigBoolean(args['trace-requests'] ?? fileConfig.traceRequests, true),
        liveChatStreaming: getConfigBoolean(args['live-chat-streaming'] ?? fileConfig.liveChatStreaming, true),
        dataRoots: unique([...cliDataRoots, ...configDataRoots, ...getDefaultDataRoots()]),
        nativeBridgeRoots: unique([...cliNativeRoots, ...configNativeRoots, ...getDefaultNativeBridgeRoots()]),
        enableActionBooks,
        actionBookIds,
        actionBookTargetGame: String(args['action-book-target-game'] || fileConfig.actionBookTargetGame || DEFAULT_ACTION_BOOK_TARGET_GAME),
        nativeActionConfidence: Number(args['native-action-confidence'] || fileConfig.nativeActionConfidence || DEFAULT_NATIVE_ACTION_CONFIDENCE),
        tts: ttsOverrides,
        speechRecognition,
    };
}

async function ensureRoot(root) {
    await fs.mkdir(path.join(root, 'nvse', 'plugins', 'Chasm', 'outbox'), { recursive: true });
    await fs.mkdir(path.join(root, 'nvse', 'plugins', 'Chasm', 'inbox'), { recursive: true });
    await fs.mkdir(path.join(root, 'nvse', 'plugins', 'Chasm', 'archive'), { recursive: true });
    await fs.mkdir(path.join(root, 'sound', 'fx', 'nvbridge'), { recursive: true });
}

async function ensureNativeRoot(root) {
    await fs.mkdir(path.join(root, 'inbox'), { recursive: true });
    await fs.mkdir(path.join(root, 'outbox', 'chunks'), { recursive: true });
    await fs.mkdir(path.join(root, 'processed'), { recursive: true });
    await fs.mkdir(nativeActionCommandDir(root), { recursive: true });
    await fs.mkdir(nativeSaveStateEventDir(root), { recursive: true });
    await fs.mkdir(nativeSaveStateAckDir(root), { recursive: true });
    await fs.mkdir(nativeAudioDir(root), { recursive: true });
}

function helperLockPath(root) {
    return path.join(root, 'nvbridge-helper.lock');
}

function isProcessAlive(pid) {
    const processId = Number(pid);
    if (!Number.isInteger(processId) || processId <= 0) {
        return false;
    }
    try {
        process.kill(processId, 0);
        return true;
    } catch {
        return false;
    }
}

async function acquireHelperLock(root) {
    const lockPath = helperLockPath(root);
    const payload = {
        pid: process.pid,
        startedAt: new Date().toISOString(),
        root,
    };

    for (let attempt = 0; attempt < 2; attempt++) {
        try {
            const handle = await fs.open(lockPath, 'wx');
            await handle.writeFile(JSON.stringify(payload, null, 2));
            await handle.close();
            return lockPath;
        } catch (error) {
            if (error?.code !== 'EEXIST') {
                throw error;
            }
            const existing = await readJsonFile(lockPath, {}).catch(() => ({}));
            if (isProcessAlive(existing.pid) && Number(existing.pid) !== process.pid) {
                throw new Error(`Another NVBridge helper is already watching ${root} (pid ${existing.pid}).`);
            }
            await fs.rm(lockPath, { force: true });
        }
    }

    throw new Error(`Could not acquire NVBridge helper lock for ${root}.`);
}

function releaseHelperLocks(lockPaths) {
    for (const lockPath of lockPaths) {
        try {
            const existing = fsSync.existsSync(lockPath) ? JSON.parse(fsSync.readFileSync(lockPath, 'utf8')) : {};
            if (Number(existing.pid) === process.pid) {
                fsSync.rmSync(lockPath, { force: true });
            }
        } catch {
            // Best-effort cleanup only.
        }
    }
}

function outbox(root) {
    return path.join(root, 'nvse', 'plugins', 'Chasm', 'outbox');
}

function inbox(root) {
    return path.join(root, 'nvse', 'plugins', 'Chasm', 'inbox');
}

function archive(root) {
    return path.join(root, 'nvse', 'plugins', 'Chasm', 'archive');
}

function nativeInbox(root) {
    return path.join(root, 'inbox');
}

function nativeOutbox(root) {
    return path.join(root, 'outbox');
}

function nativeChunkOutbox(root) {
    return path.join(nativeOutbox(root), 'chunks');
}

function nativeProcessed(root) {
    return path.join(root, 'processed');
}

function nativeActionCommandDir(root) {
    return path.join(root, 'control', 'actions');
}

function nativeSaveStateEventDir(root) {
    return path.join(root, 'control', 'events');
}

function nativeSaveStateAckDir(root) {
    return path.join(root, 'control', 'acks');
}

function nativeSaveStateProcessedDir(root) {
    return path.join(nativeProcessed(root), 'save-state-events');
}

function nativeAudioDir(root) {
    return path.join(path.dirname(root), 'Sound', 'Voice', 'NVBridge', 'Generated');
}

function nativeTraceDir(root) {
    return path.join(root, 'traces');
}

function nativeTracePath(root, requestId) {
    return path.join(nativeTraceDir(root), `${safeFileId(requestId)}.jsonl`);
}

function getTraceRequestId(request) {
    return String(request?.request_id || request?.id || '').trim();
}

function getTraceValue(value) {
    if (typeof value === 'number') {
        return Number.isFinite(value) ? Number(value.toFixed(3)) : null;
    }
    if (typeof value === 'boolean' || value === null) {
        return value;
    }
    if (value === undefined) {
        return undefined;
    }
    return String(value);
}

function getTraceFields(fields = {}) {
    return Object.fromEntries(
        Object.entries(fields)
            .map(([key, value]) => [key, getTraceValue(value)])
            .filter(([, value]) => value !== undefined),
    );
}

function ensureHelperTraceContext(request) {
    if (request && !request.__helperTraceStartedAt) {
        request.__helperTraceStartedAt = performance.now();
    }
    return request?.__helperTraceStartedAt || performance.now();
}

async function traceRequestStage(request, stage, fields = {}) {
    if (!request?.__traceRequests || !request.__nativeRoot) {
        return;
    }

    const requestId = getTraceRequestId(request);
    if (!requestId || !stage) {
        return;
    }

    try {
        const now = performance.now();
        const startedAt = ensureHelperTraceContext(request);
        const payload = {
            request_id: requestId,
            stage,
            source: 'helper',
            at: new Date().toISOString(),
            helper_elapsed_ms: Number((now - startedAt).toFixed(3)),
            ...getTraceFields(fields),
        };

        if (request.__nativeRequestFilePath && fsSync.existsSync(request.__nativeRequestFilePath)) {
            const stat = await fs.stat(request.__nativeRequestFilePath);
            payload.request_file_age_ms = Number(Math.max(0, Date.now() - stat.mtimeMs).toFixed(3));
        }

        await fs.mkdir(nativeTraceDir(request.__nativeRoot), { recursive: true });
        await fs.appendFile(nativeTracePath(request.__nativeRoot, requestId), `${JSON.stringify(payload)}\n`, 'utf8');
    } catch {
        // Tracing must never affect bridge runtime behavior.
    }
}

function safeFileId(value) {
    return String(value || Date.now()).replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 80) || String(Date.now());
}

function escapeRegExp(value) {
    return String(value).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function stripSpeakerPrefix(text, speakerName) {
    let trimmed = String(text || '').trim();
    if (!speakerName) {
        return trimmed;
    }
    const prefix = new RegExp(`^(?:${escapeRegExp(speakerName)}\\b\\s*:?\\s*)+`, 'i');
    while (prefix.test(trimmed)) {
        trimmed = trimmed.replace(prefix, '').trim();
    }
    return trimmed;
}

function sanitizeBridgeLine(value) {
    return String(value ?? '').replace(/[\r\n]/g, ' ').trim();
}

const bridgeAudioTagWords = new Set([
    'angry',
    'annoyed',
    'appalled',
    'applause',
    'breathe',
    'breathes',
    'breathing',
    'cheerful',
    'chuckle',
    'chuckles',
    'clear throat',
    'clear_throat',
    'clears throat',
    'clapping',
    'cough',
    'coughs',
    'crying',
    'curious',
    'curiously',
    'delighted',
    'disgusted',
    'dramatically',
    'excited',
    'excitedly',
    'exhale',
    'exhales',
    'exhales sharply',
    'explosion',
    'fearful',
    'fart',
    'frustrated sigh',
    'gasp',
    'gasps',
    'giggling',
    'gunshot',
    'gulps',
    'happy',
    'happy gasp',
    'inhale',
    'inhales',
    'inhales deeply',
    'laugh',
    'laughing',
    'laughs',
    'laughs harder',
    'long pause',
    'mischievously',
    'muttering',
    'impressed',
    'sad',
    'sarcastic',
    'shout',
    'shouting',
    'shouts',
    'sigh',
    'sighs',
    'singing',
    'sings',
    'snort',
    'snorts',
    'short pause',
    'starts laughing',
    'swallows',
    'softly',
    'surprised',
    'thoughtful',
    'whisper',
    'whispering',
    'whispers',
    'wheezing',
    'with genuine belly laugh',
    'woo',
    'yawn',
    'yawns',
]);

const bridgeAudioTagInstructionWords = [
    'accent',
    'emotion',
    'pace',
    'pitch',
    'serious',
    'speak',
    'speed',
    'tone',
    'voice',
    'volume',
];

function stripTtsAudioTagsForBridge(value) {
    return sanitizeBridgeLine(value)
        .replace(/<\/?(?:speak|voice|audio|break|prosody|emphasis|say-as|sub|phoneme|mstts:express-as|amazon:emotion|p|s|mark|bookmark|lang|w)\b[^>]*>/gi, '')
        .replace(/\[([^\]\r\n]{1,140})\]/g, (match, tagText) => shouldStripBridgeBracketAudioTag(tagText) ? '' : match)
        .replace(/\s+([,.!?;:])/g, '$1')
        .replace(/[ \t]{2,}/g, ' ')
        .trim();
}

function shouldStripBridgeBracketAudioTag(tagText) {
    const normalized = String(tagText || '').trim().toLowerCase().replace(/\s+/g, ' ');
    if (!normalized) {
        return false;
    }
    if (bridgeAudioTagWords.has(normalized)) {
        return true;
    }
    if (normalized.split(/[;,/]+/).every(part => bridgeAudioTagWords.has(part.trim()))) {
        return true;
    }
    return bridgeAudioTagInstructionWords.some(word => normalized.includes(word));
}

function parseNativeMetadata(value) {
    const text = sanitizeBridgeLine(value);
    if (!text) {
        return {};
    }
    try {
        const parsed = JSON.parse(text);
        return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {};
    } catch {
        return {};
    }
}

function coerceNativeMetadataValue(value) {
    const text = sanitizeBridgeLine(value);
    if (/^(?:true|false)$/i.test(text)) {
        return text.toLowerCase() === 'true';
    }
    if (/^-?\d+(?:\.\d+)?$/.test(text)) {
        const number = Number(text);
        if (Number.isFinite(number)) {
            return number;
        }
    }
    return text;
}

function parseNativeMetadataFromLines(lines, startIndex = 10) {
    const metadata = { ...parseNativeMetadata(lines[startIndex]) };
    for (const line of lines.slice(startIndex)) {
        const match = String(line || '').match(/^\s*([A-Za-z0-9_.-]+)\s*=\s*(.*?)\s*$/);
        if (!match) {
            continue;
        }
        metadata[match[1]] = coerceNativeMetadataValue(match[2]);
    }
    return metadata;
}

function getNativeDistanceMeters(request) {
    const candidates = Array.isArray(request.targeting?.nearby_npcs)
        ? request.targeting.nearby_npcs
        : [];
    const focused = candidates.find(candidate => candidate?.under_crosshair)
        || candidates.find(candidate => candidate?.npc_key === request.npc_key)
        || candidates[0];
    const distance = Number(focused?.distance_m);
    return Number.isFinite(distance) ? distance : 0;
}

function isPlayerNpcCandidate(candidate) {
    const keys = [
        candidate.npc_key,
        candidate.npcKey,
        candidate.nativeNpcKey,
        candidate.npc_name,
        candidate.npcName,
        candidate.name,
        candidate.voice_type_key,
        candidate.voiceTypeKey,
    ].map(value => slugLookupKey(value));
    return keys.includes('player') || keys.includes('playervoicemale') || keys.includes('playervoicefemale');
}

function getNpcMappingEntry(config, candidate) {
    const map = getPlainObject(config.npcCharacterMap);
    const keys = unique([
        candidate.npc_key,
        candidate.npcKey,
        candidate.nativeNpcKey,
        candidate.characterId,
        candidate.character_id,
        candidate.npc_name,
        candidate.npcName,
        candidate.name,
        candidate.voice_type_key,
        candidate.voiceTypeKey,
    ].flatMap(value => [String(value || '').trim(), slugLookupKey(value)]));

    for (const key of keys) {
        if (key && map[key]) {
            return map[key];
        }
    }

    const slugKeys = keys.map(slugLookupKey).filter(Boolean);
    for (const [mapKey, entry] of Object.entries(map)) {
        const prefixes = getNpcMappingPrefixes(mapKey, entry);
        if (prefixes.some(prefix => slugKeys.some(key => key.startsWith(prefix)))) {
            return entry;
        }
    }

    return null;
}

function normalizeNpcMappingEntry(entry) {
    if (!entry) {
        return {};
    }
    return typeof entry === 'string' ? { characterId: entry } : getPlainObject(entry);
}

function getNpcMappingPrefixes(mapKey, entry) {
    const mapping = normalizeNpcMappingEntry(entry);
    const configuredPrefixes = [
        mapping.matchPrefix,
        mapping.nativeNpcKeyPrefix,
        mapping.keyPrefix,
        ...(Array.isArray(mapping.matchPrefixes) ? mapping.matchPrefixes : []),
        ...(Array.isArray(mapping.nativeNpcKeyPrefixes) ? mapping.nativeNpcKeyPrefixes : []),
        ...(Array.isArray(mapping.keyPrefixes) ? mapping.keyPrefixes : []),
    ];
    const key = String(mapKey || '').trim();
    if (key.endsWith('*')) {
        configuredPrefixes.push(key.slice(0, -1));
    }

    const slugKey = slugLookupKey(key.replace(/\*+$/g, ''));
    if (slugKey) {
        configuredPrefixes.push(`${slugKey}__ref_`);
    }

    return unique(configuredPrefixes.map(slugLookupKey).filter(Boolean));
}

function normalizeNpcCandidate(config, candidate, fallback = {}) {
    if (isPlayerNpcCandidate(candidate)) {
        return null;
    }

    const mappingEntry = getNpcMappingEntry(config, candidate);
    const mapping = normalizeNpcMappingEntry(mappingEntry);
    const explicitCharacterId = String(candidate.characterId || candidate.character_id || '').replace(/\.png$/i, '').trim();
    if (fallback.requireMappedCharacter && !mappingEntry && !explicitCharacterId) {
        return null;
    }

    const nativeNpcKey = String(candidate.npc_key || candidate.npcKey || fallback.nativeNpcKey || '').trim();
    const nativeNpcName = String(candidate.npc_name || candidate.npcName || candidate.name || fallback.nativeNpcName || '').trim();
    const characterId = String(
        mapping.characterId
        || mapping.character_id
        || mapping.id
        || explicitCharacterId
        || (!fallback.requireMappedCharacter ? nativeNpcName : '')
        || (!fallback.requireMappedCharacter ? nativeNpcKey : '')
        || (fallback.allowConfigFallback ? config.characterId : ''),
    ).replace(/\.png$/i, '').trim();
    const characterName = String(
        mapping.characterName
        || mapping.character_name
        || mapping.name
        || candidate.characterName
        || candidate.character_name
        || nativeNpcName
        || characterId
        || config.characterName,
    ).trim();
    const participantId = String(
        mapping.participantId
        || mapping.participant_id
        || candidate.participantId
        || candidate.participant_id
        || (nativeNpcKey ? `npc:${nativeNpcKey}` : `npc:${characterId}`),
    ).trim();
    const distanceMeters = Number(candidate.distance_m ?? candidate.distanceMeters ?? fallback.distanceMeters);
    const distanceGameUnits = Number(candidate.distanceGameUnits ?? candidate.distance_game_units ?? fallback.distanceGameUnits);

    if (!characterId || !participantId) {
        return null;
    }

    return {
        participantId,
        type: 'npc',
        characterId,
        name: characterName,
        present: true,
        audible: true,
        distance: Number.isFinite(distanceMeters) ? distanceMeters : (Number.isFinite(distanceGameUnits) ? distanceGameUnits : null),
        metadata: {
            nativeNpcKey,
            nativeNpcName,
            characterName,
            voiceTypeKey: candidate.voice_type_key || candidate.voiceTypeKey || mapping.voiceTypeKey || mapping.voice_type_key || '',
            voiceTypeName: candidate.voice_type_name || candidate.voiceTypeName || mapping.voiceTypeName || mapping.voice_type_name || '',
            distanceMeters: Number.isFinite(distanceMeters) ? distanceMeters : null,
            distanceGameUnits: Number.isFinite(distanceGameUnits) ? distanceGameUnits : null,
            underCrosshair: Boolean(candidate.under_crosshair || candidate.underCrosshair),
        },
    };
}

function getFiniteNumber(...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number)) {
            return number;
        }
    }
    return null;
}

function getCandidateCoordinates(candidate) {
    const coordinateSources = [
        candidate.coordinates,
        candidate.coords,
        candidate.position,
        candidate.pos,
        candidate.worldPosition,
        candidate.world_position,
    ].map(getPlainObject);
    const getAxis = axis => {
        for (const source of coordinateSources) {
            const value = getFiniteNumber(source[axis], source[axis.toUpperCase()]);
            if (value !== null) {
                return value;
            }
        }
        return getFiniteNumber(
            candidate[axis],
            candidate[axis.toUpperCase()],
            candidate[`pos_${axis}`],
            candidate[`pos${axis.toUpperCase()}`],
            candidate[`world_${axis}`],
            candidate[`world${axis.toUpperCase()}`],
        );
    };
    const x = getAxis('x');
    const y = getAxis('y');
    const z = getAxis('z');
    return x !== null && y !== null && z !== null ? { x, y, z } : null;
}

function getNativeGamestateNpcEntry(config, candidate) {
    if (isPlayerNpcCandidate(candidate)) {
        return null;
    }

    const mappingEntry = getNpcMappingEntry(config, candidate);
    const mapping = normalizeNpcMappingEntry(mappingEntry);
    const nativeNpcKey = String(candidate.npc_key || candidate.npcKey || candidate.nativeNpcKey || '').trim();
    const nativeNpcName = String(candidate.npc_name || candidate.npcName || candidate.name || nativeNpcKey || '').trim();
    const explicitCharacterId = String(candidate.characterId || candidate.character_id || '').replace(/\.png$/i, '').trim();
    const characterName = String(
        mapping.characterName
        || mapping.character_name
        || mapping.name
        || candidate.characterName
        || candidate.character_name
        || explicitCharacterId
        || '',
    ).trim();
    const distanceMeters = getFiniteNumber(candidate.distance_m, candidate.distanceMeters, candidate.distance);

    if (!nativeNpcKey && !nativeNpcName) {
        return null;
    }

    return {
        internalName: nativeNpcKey || slugLookupKey(nativeNpcName),
        nativeNpcName,
        stCharacterName: characterName || 'unmapped',
        distanceMeters,
        coordinates: getCandidateCoordinates(candidate),
        underCrosshair: Boolean(candidate.under_crosshair || candidate.underCrosshair),
    };
}

function buildNativeGamestate(config, request, location = '') {
    const nearby = Array.isArray(request.targeting?.nearby_npcs)
        ? request.targeting.nearby_npcs
        : [];
    const radiusMeters = Number.isFinite(config.gameStateRadiusMeters)
        ? config.gameStateRadiusMeters
        : DEFAULT_GAMESTATE_RADIUS_METERS;
    const seen = new Set();
    const npcs = [];

    for (const candidate of nearby) {
        const distanceMeters = getFiniteNumber(candidate.distance_m, candidate.distanceMeters, candidate.distance);
        if (distanceMeters !== null && distanceMeters > radiusMeters) {
            continue;
        }
        const entry = getNativeGamestateNpcEntry(config, candidate);
        if (!entry) {
            continue;
        }
        const key = slugLookupKey(entry.internalName || entry.nativeNpcName);
        if (seen.has(key)) {
            continue;
        }
        seen.add(key);
        npcs.push(entry);
    }

    return {
        location,
        radiusMeters,
        npcs,
        includeEmptyNpcList: true,
    };
}

function getNearbyNpcCandidates(config, request, distanceGameUnits) {
    const nearby = Array.isArray(request.targeting?.nearby_npcs)
        ? request.targeting.nearby_npcs
        : [];
    const hasNativeNearbyList = nearby.length > 0;
    const hasNativeTargetIdentity = Boolean(request.nativeNpcKey || request.npc_key || request.targetName || request.npc_name);
    const candidates = nearby.length > 0
        ? nearby
        : [{
            npc_key: request.nativeNpcKey || request.npc_key || '',
            npc_name: request.targetName || request.npc_name || config.characterName,
            distance_m: request.distanceMeters,
            distanceGameUnits,
            under_crosshair: true,
        }];
    const maxMeters = Number(config.nativeMaxDistanceMeters || DEFAULT_NATIVE_MAX_DISTANCE_METERS);
    const participants = [];
    const seen = new Set();

    for (const candidate of candidates) {
        const distanceMeters = Number(candidate.distance_m ?? candidate.distanceMeters);
        if (Number.isFinite(distanceMeters) && Number.isFinite(maxMeters) && distanceMeters > maxMeters) {
            continue;
        }
        const participant = normalizeNpcCandidate(config, candidate, {
            nativeNpcKey: request.nativeNpcKey || request.npc_key || '',
            nativeNpcName: request.targetName || request.npc_name || config.characterName,
            distanceMeters: request.distanceMeters,
            distanceGameUnits,
            requireMappedCharacter: hasNativeNearbyList || hasNativeTargetIdentity,
            allowConfigFallback: !hasNativeNearbyList && !hasNativeTargetIdentity,
        });
        if (!participant || seen.has(participant.participantId)) {
            continue;
        }
        seen.add(participant.participantId);
        participants.push(participant);
    }

    if (participants.length === 0 && !hasNativeNearbyList) {
        const fallback = normalizeNpcCandidate(config, {}, {
            nativeNpcKey: request.nativeNpcKey || request.npc_key || '',
            nativeNpcName: request.targetName || request.npc_name || config.characterName,
            distanceMeters: request.distanceMeters,
            distanceGameUnits,
            requireMappedCharacter: hasNativeTargetIdentity,
            allowConfigFallback: !hasNativeTargetIdentity,
        });
        if (fallback) {
            participants.push(fallback);
        }
    }

    return participants;
}

function getAttentionTargetParticipantId(request, npcParticipants) {
    const focusNativeKey = String(request.targeting?.focus_npc_key || request.nativeNpcKey || request.npc_key || '').trim();
    const focused = npcParticipants.find(participant => participant.metadata?.nativeNpcKey && participant.metadata.nativeNpcKey === focusNativeKey)
        || npcParticipants.find(participant => participant.metadata?.underCrosshair)
        || (npcParticipants.length === 1 ? npcParticipants[0] : null);
    return focused?.participantId || null;
}

function getSelectedSpeakerInfo(config, npcParticipants, turn, request = {}) {
    const participantId = turn?.speaker?.participantId || '';
    const participant = npcParticipants.find(item => item.participantId === participantId);
    return {
        participantId,
        nativeNpcKey: participant?.metadata?.nativeNpcKey || request.nativeNpcKey || request.npc_key || '',
        nativeNpcName: participant?.metadata?.nativeNpcName || turn?.speaker?.name || request.targetName || request.npc_name || config.characterName,
        characterName: turn?.speaker?.name || participant?.metadata?.characterName || participant?.name || config.characterName,
        characterId: turn?.speaker?.characterId || participant?.characterId || config.characterId,
    };
}

function getGeneratedLineItems(config, npcParticipants, turn, request = {}) {
    const turns = Array.isArray(turn?.turns) && turn.turns.length ? turn.turns : [turn];
    return turns.map(item => {
        const speaker = getSelectedSpeakerInfo(config, npcParticipants, item, request);
        const text = stripSpeakerPrefix(item?.message?.content, speaker.characterName || config.characterName);
        return { turn: item, text, speaker };
    }).filter(item => item.text);
}

function takeSpeechSegment(buffer, force = false) {
    const text = String(buffer || '');
    const trimmed = text.trimStart();
    const leading = text.length - trimmed.length;
    if (!trimmed) {
        return null;
    }

    for (let index = 0; index < trimmed.length; index++) {
        const char = trimmed[index];
        if (!/[.!?]/.test(char)) {
            continue;
        }
        if (index < 18) {
            continue;
        }
        const next = trimmed[index + 1] || '';
        if (next && !/\s|["')\]]/.test(next)) {
            continue;
        }
        const end = leading + index + 1;
        return {
            segment: text.slice(0, end).trim(),
            rest: text.slice(end).trimStart(),
        };
    }

    if (trimmed.length >= 180) {
        const boundary = Math.max(
            trimmed.lastIndexOf(',', 160),
            trimmed.lastIndexOf(';', 160),
            trimmed.lastIndexOf(':', 160),
            trimmed.lastIndexOf(' ', 160),
        );
        const end = leading + (boundary > 60 ? boundary + 1 : 180);
        return {
            segment: text.slice(0, end).trim(),
            rest: text.slice(end).trimStart(),
        };
    }

    if (force) {
        return {
            segment: trimmed.trim(),
            rest: '',
        };
    }

    return null;
}

function getNoGameMasterAction() {
    return {
        action: 'NONE',
        confidence: '',
        should_trigger: false,
    };
}

function normalizeStructuredActionId(action) {
    return String(action?.id || action?.actionId || action?.name || '').trim();
}

function normalizeActivatedActionId(action) {
    return String(action?.actionId || action?.action_id || action?.id || '').trim();
}

function collectStructuredActions(turn) {
    const actions = [];
    const collect = node => {
        if (Array.isArray(node?.structured?.actions)) {
            actions.push(...node.structured.actions);
        }
        if (Array.isArray(node?.actions)) {
            actions.push(...node.actions);
        }
    };

    collect(turn);
    if (Array.isArray(turn?.turns)) {
        for (const item of turn.turns) {
            collect(item);
        }
    }

    return actions.filter(action => normalizeStructuredActionId(action));
}

function collectActivatedActions(turn) {
    const actions = [];
    const collect = node => {
        if (Array.isArray(node?.metadata?.activatedActions)) {
            actions.push(...node.metadata.activatedActions);
        }
        if (Array.isArray(node?.metadata?.activatedQuests)) {
            for (const quest of node.metadata.activatedQuests) {
                const events = Array.isArray(quest?.questEvents) ? quest.questEvents : [];
                for (const event of events) {
                    actions.push({
                        ...event,
                        actionId: event.actionId || event.action_id,
                        bookId: quest.bookId || event.bookId || '',
                        questId: quest.questId || event.parameters?.questId || '',
                        questName: quest.questName || '',
                    });
                }
            }
        }
        if (Array.isArray(node?.activatedActions)) {
            actions.push(...node.activatedActions);
        }
        if (Array.isArray(node?.activatedQuests)) {
            for (const quest of node.activatedQuests) {
                const events = Array.isArray(quest?.questEvents) ? quest.questEvents : [];
                for (const event of events) {
                    actions.push({
                        ...event,
                        actionId: event.actionId || event.action_id,
                        bookId: quest.bookId || event.bookId || '',
                        questId: quest.questId || event.parameters?.questId || '',
                        questName: quest.questName || '',
                    });
                }
            }
        }
    };

    collect(turn);
    if (Array.isArray(turn?.turns)) {
        for (const item of turn.turns) {
            collect(item);
        }
    }

    return actions.filter(action => normalizeActivatedActionId(action));
}

function getActivatedActionMap(turn) {
    const map = new Map();
    for (const action of collectActivatedActions(turn)) {
        const actionId = normalizeActivatedActionId(action);
        if (actionId && !map.has(actionId)) {
            map.set(actionId, action);
        }
    }
    return map;
}

function getTrustedActivatedExecution(action, activatedActions) {
    const actionId = normalizeStructuredActionId(action);
    const activatedAction = activatedActions.get(actionId);
    const binding = getPlainObject(activatedAction?.binding);
    const execution = getPlainObject(activatedAction?.execution);
    const engine = String(binding.engine || '').trim().toLowerCase();
    const script = String(execution.script || '').trim();
    if (engine !== TRUSTED_FNV_ACTION_ENGINE || !script) {
        return { activatedAction, binding: {}, execution: {} };
    }
    return { activatedAction, binding, execution };
}

function getActionTarget(action) {
    const parameters = getPlainObject(action?.parameters || action?.params);
    return String(action?.target || parameters.target || '').trim().toLowerCase();
}

function isPlayerActionTarget(action) {
    const target = getActionTarget(action);
    return ['', 'player', 'courier', 'the player', 'target:player'].includes(target);
}

function getActionParameters(action) {
    return getPlainObject(action?.parameters || action?.params);
}

function getActionParameterValue(action, name) {
    const key = String(name || '').trim();
    if (!key) {
        return undefined;
    }
    const parameters = getActionParameters(action);
    return parameters[key] ?? action?.[key];
}

function getActionParameterValueFromConfig(action, config, fallbackName = '') {
    const names = unique([
        config.parameter,
        config.name,
        fallbackName,
        ...(Array.isArray(config.parameters) ? config.parameters : []),
        ...(Array.isArray(config.fallbackParameters) ? config.fallbackParameters : []),
        ...(Array.isArray(config.aliases) ? config.aliases : []),
    ].map(value => String(value || '').trim()));
    for (const name of names) {
        const value = getActionParameterValue(action, name);
        if (value !== undefined && value !== null && value !== '') {
            return value;
        }
    }
    return undefined;
}

function getObjectPathValue(value, pathValue) {
    const pathParts = String(pathValue || '').split('.').map(part => part.trim()).filter(Boolean);
    let current = value;
    for (const part of pathParts) {
        if (!current || typeof current !== 'object') {
            return undefined;
        }
        current = current[part];
    }
    return current;
}

function getCatalogCandidateLookupKeys(candidate) {
    const metadata = getPlainObject(candidate?.metadata);
    const publicMetadata = getPlainObject(candidate?.publicMetadata);
    return unique([
        candidate?.id,
        candidate?.name,
        candidate?.title,
        metadata.editorId,
        metadata.fullName,
        metadata.recordType,
        publicMetadata.editorId,
        publicMetadata.fullName,
        publicMetadata.recordType,
        ...(Array.isArray(candidate?.aliases) ? candidate.aliases : []),
    ].flatMap(value => [String(value || '').trim(), slugLookupKey(value)]));
}

function findScopedCatalogItem(action, config) {
    const parameterName = String(config.parameter || config.parameterName || 'catalogItemId').trim();
    const itemId = String(config.itemId || config.id || getActionParameterValue(action, parameterName) || '').trim();
    if (!itemId) {
        return null;
    }

    const normalizedItemId = slugLookupKey(itemId);
    const catalogId = String(config.catalogId || config.catalog || '').trim();
    const catalogs = Array.isArray(action?.scopedCatalogs) ? action.scopedCatalogs : [];
    for (const catalog of catalogs) {
        if (catalogId && String(catalog.catalogId || '').trim() !== catalogId) {
            continue;
        }
        const items = Array.isArray(catalog.items) ? catalog.items : [];
        const item = items.find(candidate => String(candidate?.id || '').trim() === itemId)
            || items.find(candidate => getCatalogCandidateLookupKeys(candidate).includes(normalizedItemId));
        if (item) {
            return item;
        }
    }
    return null;
}

function normalizeNativeRefId(value) {
    const text = String(value || '').trim();
    if (!text) {
        return '';
    }
    if (/^[0-9]+$/.test(text)) {
        return Number(text).toString(16).toUpperCase();
    }
    const normalized = text.replace(/^0x/i, '').replace(/^refid:/i, '').trim().toUpperCase();
    return /^[0-9A-F]{1,8}$/.test(normalized) ? normalized.padStart(8, '0') : '';
}

function getSpawnRefLookupTerms(value) {
    return unique(String(value || '')
        .split(/[,;/|]+|\s+(?:or|and)\s+/iu)
        .flatMap(part => {
            const text = part.trim();
            const slug = slugLookupKey(text);
            return [text, slug];
        })
        .filter(Boolean));
}

function getCandidateRefId(candidate) {
    return normalizeNativeRefId(
        candidate?.ref_id
        ?? candidate?.refId
        ?? candidate?.referenceId
        ?? candidate?.reference_id
    );
}

function getCandidateSpawnAnchorKeys(candidate) {
    return unique([
        candidate?.npc_key,
        candidate?.npcKey,
        candidate?.nativeNpcKey,
        candidate?.npc_name,
        candidate?.npcName,
        candidate?.name,
        candidate?.characterName,
        candidate?.character_name,
    ].flatMap(value => {
        const text = String(value || '').trim();
        return [text, slugLookupKey(text), ...text.split(/\s+/).map(slugLookupKey)];
    }));
}

function normalizeTrustedNativeRefArgValue(value, context = {}) {
    const text = String(value || '').trim();
    const slug = slugLookupKey(text);
    if (!text || ['player', 'courier', 'me', 'myself', 'the_player', 'target_player'].includes(slug)) {
        return 'ref:player';
    }
    if (['actor', 'speaker', 'npc', 'subject', 'source'].includes(slug)) {
        return 'ref:actor';
    }

    const refId = normalizeNativeRefId(text);
    if (refId) {
        return `refid:${refId}`;
    }

    const candidates = Array.isArray(context.request?.targeting?.nearby_npcs)
        ? context.request.targeting.nearby_npcs
        : [];
    const terms = getSpawnRefLookupTerms(text);
    for (const candidate of candidates) {
        const candidateRefId = getCandidateRefId(candidate);
        if (!candidateRefId) {
            continue;
        }
        const keys = getCandidateSpawnAnchorKeys(candidate);
        if (terms.some(term => keys.includes(term))) {
            return `refid:${candidateRefId}`;
        }
    }

    return null;
}

function normalizeTrustedNativeArgValue(type, value, config = {}, context = {}) {
    const argType = String(type || config.argType || config.nativeType || 'string').trim().toLowerCase();
    if (value === undefined || value === null || value === '') {
        return null;
    }

    if (argType === 'ref' || argType === 'reference' || argType === 'refid' || argType === 'reference_id') {
        return normalizeTrustedNativeRefArgValue(value, context);
    }

    if (argType === 'form' || argType === 'formid' || argType === 'form_id') {
        const normalized = String(value || '').trim().replace(/^0x/i, '').toUpperCase();
        return /^[0-9A-F]{1,8}$/.test(normalized) ? `form:${normalized.padStart(8, '0')}` : null;
    }

    if (argType === 'number' || argType === 'float' || argType === 'int' || argType === 'integer') {
        let number = Number(value);
        if (!Number.isFinite(number)) {
            return null;
        }
        const min = Number(config.min);
        const max = Number(config.max);
        if (Number.isFinite(min)) {
            number = Math.max(min, number);
        }
        if (Number.isFinite(max)) {
            number = Math.min(max, number);
        }
        if (argType === 'int' || argType === 'integer') {
            number = Math.trunc(number);
        }
        return `number:${number}`;
    }

    const text = sanitizeBridgeLine(value).replace(/,/g, ' ').trim();
    return text ? `string:${text}` : null;
}

function resolveTrustedExecutionArgument(argument, action, context = {}) {
    if (typeof argument === 'string') {
        return argument.trim();
    }

    const config = getPlainObject(argument);
    const type = String(config.type || config.source || '').trim().toLowerCase();
    const required = config.required === undefined ? true : Boolean(config.required);
    let value;

    if (type === 'catalogmetadata' || type === 'catalog_metadata' || type === 'scopedcatalogmetadata' || type === 'scoped_catalog_metadata') {
        const item = findScopedCatalogItem(action, config);
        value = getObjectPathValue(getPlainObject(item?.metadata), config.metadataKey || config.key || 'formId');
    } else if (type === 'parameter' || type === 'param') {
        value = getActionParameterValueFromConfig(action, config);
    } else if (type === 'literal' || Object.hasOwn(config, 'value')) {
        value = config.value;
    }

    if ((value === undefined || value === null || value === '') && Object.hasOwn(config, 'default')) {
        value = config.default;
    }

    const encoded = normalizeTrustedNativeArgValue(config.argType || config.nativeType || config.valueType, value, config, context);
    if (!encoded && required) {
        return null;
    }
    return encoded || '';
}

function resolveTrustedExecutionArguments(execution, action, context = {}) {
    if (!Array.isArray(execution.arguments)) {
        return [];
    }

    const args = [];
    for (const argument of execution.arguments) {
        const resolved = resolveTrustedExecutionArgument(argument, action, context);
        if (resolved === null) {
            return null;
        }
        if (resolved) {
            args.push(resolved);
        }
    }
    return args;
}

function getNativeActionRepeatConfig(execution) {
    const repeat = getPlainObject(execution.repeat || execution.repetition);
    const parameter = String(repeat.parameter || repeat.parameterName || repeat.name || '').trim();
    if (!parameter) {
        return null;
    }
    return { ...repeat, parameter };
}

function getTrustedExecutionParameterArgument(execution, parameterName) {
    const expected = String(parameterName || '').trim();
    if (!expected || !Array.isArray(execution.arguments)) {
        return {};
    }
    return getPlainObject(execution.arguments.find(argument => {
        const config = getPlainObject(argument);
        return String(config.parameter || config.name || '').trim() === expected;
    }));
}

function clampRepeatCount(value, repeat, argumentConfig) {
    const fallback = Number(repeat.default ?? argumentConfig.default ?? 1);
    let count = Number(value);
    if (!Number.isFinite(count)) {
        count = Number.isFinite(fallback) ? fallback : 1;
    }

    const min = Number(repeat.min ?? argumentConfig.min ?? 1);
    const max = Number(repeat.max ?? argumentConfig.max);
    if (Number.isFinite(min)) {
        count = Math.max(min, count);
    }
    if (Number.isFinite(max)) {
        count = Math.min(max, count);
    }
    return Math.max(1, Math.trunc(count));
}

function getNativeActionRepeatCount(execution, action) {
    const repeat = getNativeActionRepeatConfig(execution);
    if (!repeat) {
        return 1;
    }

    const argumentConfig = getTrustedExecutionParameterArgument(execution, repeat.parameter);
    return clampRepeatCount(getActionParameterValue(action, repeat.parameter), repeat, argumentConfig);
}

function getNativeActionRepeatArgumentValue(execution) {
    const repeat = getNativeActionRepeatConfig(execution);
    if (!repeat) {
        return undefined;
    }
    if (Object.hasOwn(repeat, 'argumentValue')) {
        return repeat.argumentValue;
    }
    if (Object.hasOwn(repeat, 'perCommandValue')) {
        return repeat.perCommandValue;
    }
    if (Object.hasOwn(repeat, 'value')) {
        return repeat.value;
    }
    return 1;
}

function cloneActionWithParameter(action, parameterName, value) {
    const parameters = {
        ...getActionParameters(action),
        [parameterName]: value,
    };
    const cloned = {
        ...action,
        parameters,
    };
    if (action?.params && !action.parameters) {
        cloned.params = parameters;
    }
    return cloned;
}

function cloneGameMasterWithPrimaryAction(gameMaster, primaryAction, replacementAction) {
    let replaced = false;
    const actions = Array.isArray(gameMaster?.actions)
        ? gameMaster.actions.map(action => {
            if (action === primaryAction) {
                replaced = true;
                return replacementAction;
            }
            return action;
        })
        : [];

    if (!replaced) {
        actions.push(replacementAction);
    }

    return {
        ...gameMaster,
        actions,
    };
}

function getActionConfidence(config, action) {
    const parameters = getActionParameters(action);
    const raw = action?.confidence ?? parameters.confidence ?? config.nativeActionConfidence;
    const confidence = Number(raw);
    if (!Number.isFinite(confidence)) {
        return config.nativeActionConfidence;
    }
    return Math.max(0, Math.min(1, confidence));
}

function getNativeGameMasterAction(config, turn) {
    if (!config.enableActionBooks) {
        return getNoGameMasterAction();
    }

    const structuredActions = collectStructuredActions(turn);
    const activatedActions = getActivatedActionMap(turn);
    const actions = getStructuredActionMetadata(config, turn);
    const getBaseAction = (action, nativeAction) => {
        const actionId = normalizeStructuredActionId(action);
        const confidence = getActionConfidence(config, action);
        if (confidence < config.nativeActionConfidence) {
            return null;
        }
        return {
            action: nativeAction,
            confidence: confidence.toFixed(2),
            should_trigger: true,
            action_id: actionId,
            reason: sanitizeBridgeLine(action.reason || ''),
            actions,
        };
    };

    for (const action of structuredActions) {
        if (normalizeStructuredActionId(action) !== 'combat.start' || !isPlayerActionTarget(action)) {
            continue;
        }
        const mapped = getBaseAction(action, 'ATTACK');
        if (mapped) {
            return mapped;
        }
    }

    for (const action of structuredActions) {
        const actionId = normalizeStructuredActionId(action);
        if (!['movement.stop_follow_target', 'movement.stop_following', 'movement.stop_follow'].includes(actionId) || !isPlayerActionTarget(action)) {
            continue;
        }
        const mapped = getBaseAction(action, 'STOP_FOLLOW');
        if (mapped) {
            return mapped;
        }
    }

    for (const action of structuredActions) {
        if (normalizeStructuredActionId(action) !== 'movement.follow_target' || !isPlayerActionTarget(action)) {
            continue;
        }
        const mapped = getBaseAction(action, 'FOLLOW');
        if (mapped) {
            return mapped;
        }
    }

    for (const action of structuredActions) {
        const { execution } = getTrustedActivatedExecution(action, activatedActions);
        if (!Object.keys(execution).length) {
            continue;
        }
        const mapped = getBaseAction(action, 'ACTION_BOOK');
        if (mapped) {
            return mapped;
        }
    }

    return getNoGameMasterAction();
}

function getAdminTextCommandAction(config, request) {
    if (!config.enableActionBooks) {
        return getNoGameMasterAction();
    }

    const text = String(
        request?.message
        || request?.player_text
        || request?.playerText
        || request?.text
        || '',
    ).trim();
    if (!text) {
        return getNoGameMasterAction();
    }

    const slug = `_${slugLookupKey(text)}_`;
    const mentionsFollow = /_(?:follow|following|follower|escort)_/.test(slug);
    const hasNegation = /_(?:do_not|dont|don_t|never|no)_/.test(slug);
    const wantsStopFollow = (
        /_(?:stop_following|stop_follow|stop)_/.test(slug) && mentionsFollow
    ) || (
        hasNegation && mentionsFollow
    ) || /_(?:dismiss|wait_here|stay_here|stand_down|go_away)_/.test(slug);
    const wantsAttack = /_(?:attack|fight|start_combat|hostile|aggro|kill|shoot|hit)_/.test(slug);
    const wantsFollow = /_(?:follow|follow_me|come_with|come_along|come_here|escort)_/.test(slug);
    if (hasNegation && !wantsStopFollow) {
        return getNoGameMasterAction();
    }

    const action = wantsStopFollow ? 'STOP_FOLLOW' : (wantsAttack ? 'ATTACK' : (wantsFollow ? 'FOLLOW' : ''));
    if (!action) {
        return getNoGameMasterAction();
    }

    const actionId = action === 'ATTACK'
        ? 'combat.start'
        : (action === 'STOP_FOLLOW' ? 'movement.stop_follow_target' : 'movement.follow_target');
    return {
        action,
        confidence: '1.00',
        should_trigger: true,
        action_id: actionId,
        reason: 'admin text command fallback',
        actions: [{
            action_id: actionId,
            target: 'player',
            actor: text,
            parameters: {
                target: 'player',
                actor: text,
            },
            confidence: '1.00',
            reason: 'admin text command fallback',
        }],
    };
}

function getValidatedGameMasterAction(config, turn) {
    return getNativeGameMasterAction(config, turn);
}

function getStructuredActionMetadata(config, turn) {
    if (!config.enableActionBooks) {
        return [];
    }

    const activatedActions = getActivatedActionMap(turn);
    return collectStructuredActions(turn).map(action => {
        const parameters = getActionParameters(action);
        const actionId = normalizeStructuredActionId(action);
        const { activatedAction, binding, execution } = getTrustedActivatedExecution(action, activatedActions);
        return {
            action_id: actionId,
            action_book_id: activatedAction?.bookId || '',
            target: getActionTarget(action),
            actor: getActionActorHint(action),
            parameters,
            scopedCatalogs: Array.isArray(activatedAction?.scopedCatalogs) ? activatedAction.scopedCatalogs : [],
            confidence: getActionConfidence(config, action).toFixed(2),
            reason: sanitizeBridgeLine(action?.reason || ''),
            ...(Object.keys(binding).length > 0 ? { binding } : {}),
            ...(Object.keys(execution).length > 0 ? { execution } : {}),
        };
    });
}

function getAdminGameMasterAction(config, turn, request = {}) {
    const structuredAction = getNativeGameMasterAction(config, turn);
    if (structuredAction.should_trigger) {
        return structuredAction;
    }
    return getAdminTextCommandAction(config, request);
}

function isAdminRequest(request) {
    const fields = [
        request.admin,
        request.adminMode,
        request.admin_mode,
        request.godmode,
    ];
    if (fields.some(value => getConfigBoolean(value, false))) {
        return true;
    }

    const playerText = slugLookupKey(request.player_text || request.playerText || request.message || request.text || '');
    if (playerText === 'todd' || playerText.startsWith('todd_') || playerText.includes('_todd_')) {
        return true;
    }

    const textFields = [
        request.input_mode,
        request.inputMode,
        request.mode,
        request.target,
        request.npc_key,
        request.npc_name,
        request.characterId,
        request.character_id,
    ].map(value => slugLookupKey(value));
    return textFields.some(value => ['admin', 'godmode', 'todd', 'system_todd'].includes(value));
}

function getMappedNativeActors(config) {
    const map = getPlainObject(config.npcCharacterMap);
    return Object.entries(map).map(([key, value]) => {
        const mapping = normalizeNpcMappingEntry(value);
        const nativeNpcKey = String(mapping.nativeNpcKey || mapping.npc_key || key || '').trim();
        const nativeNpcName = String(mapping.nativeNpcName || mapping.npc_name || mapping.characterName || mapping.name || mapping.characterId || key || '').trim();
        const characterId = String(mapping.characterId || mapping.character_id || mapping.id || nativeNpcName || nativeNpcKey).replace(/\.png$/i, '').trim();
        const characterName = String(mapping.characterName || mapping.character_name || mapping.name || characterId || nativeNpcName || nativeNpcKey).trim();
        return {
            nativeNpcKey,
            nativeNpcName,
            characterId,
            characterName,
            searchTerms: unique([
                nativeNpcKey,
                nativeNpcName,
                characterId,
                characterName,
                key,
                ...String(nativeNpcName || characterName || '').split(/\s+/),
            ].map(slugLookupKey)),
        };
    }).filter(item => item.nativeNpcKey && item.characterName);
}

function getActionActorHint(action) {
    const parameters = getActionParameters(action);
    return [
        action?.actor,
        action?.source,
        action?.speaker,
        action?.subject,
        action?.npc,
        action?.npcKey,
        action?.nativeNpcKey,
        parameters.actor,
        parameters.source,
        parameters.speaker,
        parameters.subject,
        parameters.npc,
        parameters.npcKey,
        parameters.nativeNpcKey,
        parameters.actorKey,
        parameters.characterId,
    ].map(value => String(value || '').trim()).find(Boolean) || '';
}

function getAdminNativeActor(config) {
    return {
        nativeNpcKey: 'todd',
        nativeNpcName: config.adminCharacterName,
        characterName: config.adminCharacterName,
        characterId: config.adminCharacterId,
    };
}

function shouldUseAdminActorForActionBook(gameMaster) {
    if (String(gameMaster?.action || '').trim().toUpperCase() !== 'ACTION_BOOK') {
        return false;
    }

    const primaryAction = getPrimaryGameMasterAction(gameMaster);
    const actionId = String(primaryAction?.action_id || primaryAction?.actionId || gameMaster?.action_id || '').trim().toLowerCase();
    return actionId.startsWith('world.');
}

function resolveNativeActorForAdmin(config, request, gameMaster) {
    if (shouldUseAdminActorForActionBook(gameMaster)) {
        return getAdminNativeActor(config);
    }

    const mappedActors = getMappedNativeActors(config);
    const requestHints = [
        request.npc_key,
        request.npcKey,
        request.nativeNpcKey,
        request.targetNpcKey,
        request.targetName,
        request.npc_name,
        request.npcName,
        request.characterId,
        request.character_id,
    ].map(value => String(value || '').trim()).filter(Boolean);
    const structuredHints = Array.isArray(gameMaster?.actions)
        ? gameMaster.actions.flatMap(action => [
            action.actor,
            action.parameters?.actor,
            action.parameters?.source,
            action.parameters?.speaker,
            action.parameters?.subject,
            action.parameters?.npc,
            action.parameters?.npcKey,
            action.parameters?.nativeNpcKey,
            action.parameters?.actorKey,
            action.parameters?.characterId,
        ]).map(value => String(value || '').trim()).filter(Boolean)
        : [];
    const hints = unique([...requestHints, ...structuredHints]);

    for (const hint of hints) {
        const slug = slugLookupKey(hint);
        const exact = mappedActors.find(actor => actor.searchTerms.includes(slug));
        if (exact) {
            return exact;
        }
    }

    for (const hint of hints) {
        const participant = normalizeNpcCandidate(config, {
            npc_key: hint,
            npc_name: hint,
        }, { requireMappedCharacter: true });
        if (participant) {
            return {
                nativeNpcKey: participant.metadata.nativeNpcKey,
                nativeNpcName: participant.metadata.nativeNpcName || participant.name,
                characterId: participant.characterId,
                characterName: participant.name || participant.characterId,
            };
        }
    }

    const haystack = slugLookupKey([
        request.message,
        request.player_text,
        request.playerText,
        request.text,
        ...structuredHints,
    ].filter(Boolean).join(' '));
    if (haystack) {
        const textMatch = mappedActors.find(actor => actor.searchTerms.some(term => term && haystack.includes(term)));
        if (textMatch) {
            return textMatch;
        }
    }

    const focused = Array.isArray(request.targeting?.nearby_npcs)
        ? request.targeting.nearby_npcs.find(candidate => candidate?.under_crosshair || candidate?.underCrosshair)
        : null;
    if (focused) {
        const focusedParticipant = normalizeNpcCandidate(config, focused, { requireMappedCharacter: true });
        if (focusedParticipant) {
            return {
                nativeNpcKey: focusedParticipant.metadata.nativeNpcKey,
                nativeNpcName: focusedParticipant.metadata.nativeNpcName || focusedParticipant.name,
                characterId: focusedParticipant.characterId,
                characterName: focusedParticipant.name || focusedParticipant.characterId,
            };
        }
    }

    return null;
}

function getApiTimeoutMs(config, endpoint) {
    return endpoint === '/speech/recognize'
        ? config.speechRecognitionTimeoutMs
        : config.requestTimeoutMs;
}

function createTimeoutSignal(timeoutMs) {
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) {
        return { signal: undefined, clear: () => {} };
    }

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    timeout.unref?.();
    return {
        signal: controller.signal,
        clear: () => clearTimeout(timeout),
    };
}

function isAbortError(error) {
    return error?.name === 'AbortError' || error?.code === 'ABORT_ERR';
}

function createRequestSupersededError(request) {
    const requestId = request?.request_id || request?.id || '<unknown>';
    const error = new Error(`Native request ${requestId} was superseded by a newer in-game request.`);
    error.code = REQUEST_SUPERSEDED_CODE;
    return error;
}

function isRequestSupersededError(error) {
    return error?.code === REQUEST_SUPERSEDED_CODE
        || error?.cause?.code === REQUEST_SUPERSEDED_CODE
        || error?.reason?.code === REQUEST_SUPERSEDED_CODE;
}

function createLinkedAbortSignal(...signals) {
    const activeSignals = signals.filter(signal => signal);
    if (activeSignals.length === 0) {
        return {
            signal: undefined,
            clear: () => {},
        };
    }

    const controller = new AbortController();
    const listeners = [];
    const abortFrom = signal => {
        if (!controller.signal.aborted) {
            controller.abort(signal.reason);
        }
    };

    for (const signal of activeSignals) {
        if (signal.aborted) {
            abortFrom(signal);
            continue;
        }
        const listener = () => abortFrom(signal);
        signal.addEventListener('abort', listener, { once: true });
        listeners.push([signal, listener]);
    }

    return {
        signal: controller.signal,
        clear: () => {
            for (const [signal, listener] of listeners) {
                signal.removeEventListener('abort', listener);
            }
        },
    };
}

function createNativeRequestAbortMonitor(request, intervalMs = 75) {
    if (!request?.__nativeRequestFilePath || !request?.request_id) {
        return {
            signal: undefined,
            clear: () => {},
        };
    }

    const controller = new AbortController();
    let checking = false;
    const check = async () => {
        if (checking || controller.signal.aborted) {
            return;
        }
        checking = true;
        try {
            if (!(await isNativeRequestStillCurrent(request))) {
                controller.abort(createRequestSupersededError(request));
            }
        } catch {
            // Keep the active request alive if a transient file read fails.
        } finally {
            checking = false;
        }
    };

    const interval = setInterval(() => {
        void check();
    }, Math.max(25, intervalMs));
    interval.unref?.();
    void check();

    return {
        signal: controller.signal,
        clear: () => clearInterval(interval),
    };
}

async function api(config, endpoint, options = {}) {
    const response = await apiFetch(config, endpoint, options);

    const bodyStartedAt = performance.now();
    const text = await response.text();
    await traceRequestStage(options.nativeRequest, 'helper_http_response_body_read', {
        endpoint,
        status: response.status,
        duration_ms: performance.now() - bodyStartedAt,
        bytes: Buffer.byteLength(text || '', 'utf8'),
    });
    const body = text ? JSON.parse(text) : {};
    if (!response.ok) {
        const message = body?.error?.message || `HTTP ${response.status}`;
        const error = new Error(message);
        error.status = response.status;
        error.body = body;
        throw error;
    }
    return body;
}

async function apiFetch(config, endpoint, options = {}) {
    const timeoutMs = getApiTimeoutMs(config, endpoint);
    const timeout = createTimeoutSignal(timeoutMs);
    const nativeMonitor = createNativeRequestAbortMonitor(options.nativeRequest);
    const linked = createLinkedAbortSignal(timeout.signal, options.signal, nativeMonitor.signal);
    const method = options.method || 'GET';
    const startedAt = performance.now();
    const traceRequestId = getTraceRequestId(options.nativeRequest);
    await traceRequestStage(options.nativeRequest, 'helper_http_request_start', {
        endpoint,
        method,
        timeout_ms: timeoutMs,
    });
    try {
        // Route only /speech/* to the TTS base (Rust port) when configured;
        // generation/STT keep using apiBase (SillyTavern).
        const requestBase = config.ttsApiBase && endpoint.startsWith('/speech')
            ? config.ttsApiBase
            : config.apiBase;
        const response = await fetch(`${requestBase}${endpoint}`, {
            method,
            headers: {
                ...(config.apiKey ? { Authorization: `Bearer ${config.apiKey}` } : {}),
                ...(options.body ? { 'Content-Type': 'application/json' } : {}),
                ...(traceRequestId ? { 'X-Chasm-Trace-Id': traceRequestId } : {}),
            },
            body: options.body ? JSON.stringify(options.body) : undefined,
            signal: linked.signal,
        });
        await traceRequestStage(options.nativeRequest, 'helper_http_response_headers', {
            endpoint,
            method,
            status: response.status,
            duration_ms: performance.now() - startedAt,
        });
        return response;
    } catch (error) {
        await traceRequestStage(options.nativeRequest, 'helper_http_request_failed', {
            endpoint,
            method,
            duration_ms: performance.now() - startedAt,
            error: error?.message || String(error),
        });
        if (options.signal?.aborted && isRequestSupersededError(options.signal.reason)) {
            throw options.signal.reason;
        }
        if (nativeMonitor.signal?.aborted && isRequestSupersededError(nativeMonitor.signal.reason)) {
            throw nativeMonitor.signal.reason;
        }
        if (linked.signal?.aborted && isRequestSupersededError(linked.signal.reason)) {
            throw linked.signal.reason;
        }
        if (timeout.signal?.aborted || isAbortError(error)) {
            throw new Error(`Timed out after ${Math.round(timeoutMs / 1000)}s waiting for Headless API ${endpoint}.`);
        }
        throw error;
    } finally {
        linked.clear();
        nativeMonitor.clear();
        timeout.clear();
    }
}

async function readNdjsonEvents(response, onEvent) {
    const decoder = new TextDecoder();
    let pending = '';

    const consumeLine = async line => {
        const trimmed = String(line || '').trim();
        if (!trimmed) {
            return;
        }
        await onEvent(JSON.parse(trimmed));
    };

    for await (const chunk of response.body) {
        pending += decoder.decode(chunk, { stream: true });
        const lines = pending.split(/\r?\n/);
        pending = lines.pop() || '';
        for (const line of lines) {
            await consumeLine(line);
        }
    }

    pending += decoder.decode();
    if (pending.trim()) {
        await consumeLine(pending);
    }
}

async function streamApi(config, endpoint, options = {}, onEvent = async () => {}) {
    const response = await apiFetch(config, endpoint, options);
    if (!response.ok) {
        const text = await response.text();
        let body = {};
        try {
            body = text ? JSON.parse(text) : {};
        } catch {
            body = {};
        }
        const message = body?.error?.message || text || `HTTP ${response.status}`;
        const error = new Error(message);
        error.status = response.status;
        error.body = body;
        throw error;
    }

    const streamStartedAt = performance.now();
    await traceRequestStage(options.nativeRequest, 'helper_stream_read_start', {
        endpoint,
        status: response.status,
    });
    await readNdjsonEvents(response, onEvent);
    await traceRequestStage(options.nativeRequest, 'helper_stream_read_done', {
        endpoint,
        duration_ms: performance.now() - streamStartedAt,
    });
}

async function ensureLiveChat(config, request, participants, attentionTarget) {
    try {
        await api(config, `/live-chats/${encodeURIComponent(config.liveChatId)}`, {
            nativeRequest: request,
        });
        return;
    } catch (error) {
        if (error.status !== 404) {
            throw error;
        }
    }

    if (!config.groupId || config.groupId.includes('replace-with')) {
        throw new Error('Live Chat does not exist and groupId is not configured. Create a SillyTavern group containing the mapped NPC characters and set groupId.');
    }

    await api(config, '/live-chats', {
        method: 'POST',
        body: {
            id: config.liveChatId,
            groupId: config.groupId,
            title: 'Fallout New Vegas - Goodsprings',
            location: request.location || 'Goodsprings',
            participants,
            attentionTarget,
            attentionStrength: attentionTarget ? 0.35 : 0,
        },
        nativeRequest: request,
    });
}

async function writeInbox(root, result) {
    const destination = inbox(root);
    await fs.writeFile(path.join(destination, 'latest.json'), JSON.stringify(result, null, 2), 'utf8');
    await fs.writeFile(path.join(destination, 'message.txt'), result.message || '', 'utf8');
    await fs.writeFile(path.join(destination, 'sound_path.txt'), result.soundPath || '', 'utf8');
    await fs.writeFile(path.join(destination, 'error.txt'), result.error || '', 'utf8');
    await fs.writeFile(path.join(destination, 'ready.txt'), result.ok ? '1' : '0', 'utf8');
    await fs.writeFile(path.join(destination, 'm.txt'), result.message || '', 'utf8');
    await fs.writeFile(path.join(destination, 's.txt'), result.soundPath || '', 'utf8');
    await fs.writeFile(path.join(destination, 'e.txt'), result.error || '', 'utf8');
    await fs.writeFile(path.join(destination, 'r.txt'), result.ok ? '1' : '0', 'utf8');
}

async function synthesizeLine(config, root, requestId, text, characterName = config.characterName) {
    const speech = await api(config, '/speech/synthesize', {
        method: 'POST',
        body: {
            ...config.tts,
            text,
            characterName,
            format: 'wav',
            encoding: 'base64',
        },
    });
    const filename = `nvbridge_${safeFileId(requestId)}.wav`;
    const audio = Buffer.from(speech.audio.data, 'base64');
    const roots = unique([root, ...config.dataRoots]);
    for (const dataRoot of roots) {
        const soundPath = path.join(dataRoot, 'sound', 'fx', 'nvbridge', filename);
        await fs.mkdir(path.dirname(soundPath), { recursive: true });
        await fs.writeFile(soundPath, audio);
    }
    return `fx\\nvbridge\\${filename}`;
}

async function synthesizeNativeLine(config, root, requestId, text, characterName = config.characterName, index = 0, nativeRequest = null) {
    await traceRequestStage(nativeRequest, 'tts_buffered_start', {
        character_name: characterName,
        text_length: text.length,
        index,
    });
    const startedAt = performance.now();
    const speech = await api(config, '/speech/synthesize', {
        method: 'POST',
        body: {
            ...config.tts,
            text,
            characterName,
            format: 'wav',
            encoding: 'base64',
        },
        nativeRequest,
    });
    const filename = index === 0
        ? `nvbridge_${safeFileId(requestId)}.wav`
        : `nvbridge_${safeFileId(requestId)}.${String(index).padStart(4, '0')}.wav`;
    const audio = Buffer.from(speech.audio.data, 'base64');
    const outputPath = path.join(nativeAudioDir(root), filename);
    await fs.mkdir(path.dirname(outputPath), { recursive: true });
    await fs.writeFile(outputPath, audio);
    await traceRequestStage(nativeRequest, 'tts_buffered_audio_written', {
        audio_file: filename,
        output_path: outputPath,
        audio_bytes: audio.length,
        index,
        duration_ms: performance.now() - startedAt,
    });
    return {
        filename,
        outputPath,
        mimeType: speech.mimeType || 'audio/wav',
    };
}

async function writeNativeAudioFile(root, requestId, audioBase64, index = 0, mimeType = 'audio/wav') {
    const filename = index === 0
        ? `nvbridge_${safeFileId(requestId)}.wav`
        : `nvbridge_${safeFileId(requestId)}.${String(index).padStart(4, '0')}.wav`;
    const outputPath = path.join(nativeAudioDir(root), filename);
    await fs.mkdir(path.dirname(outputPath), { recursive: true });
    await fs.writeFile(outputPath, Buffer.from(audioBase64, 'base64'));
    return {
        filename,
        outputPath,
        mimeType,
    };
}

async function synthesizeNativeLineStreaming(config, root, request, text, characterName = config.characterName, onChunk = async () => {}, indexOffset = 0) {
    const chunks = [];
    let sawFirstChunk = false;
    await traceRequestStage(request, 'tts_stream_start', {
        character_name: characterName,
        text_length: text.length,
        index_offset: indexOffset,
    });
    const startedAt = performance.now();
    try {
        await assertNativeRequestStillCurrent(request);
        await streamApi(config, '/speech/synthesize/stream', {
            method: 'POST',
            body: {
                ...config.tts,
                text,
                characterName,
                format: 'wav',
                encoding: 'base64',
            },
            nativeRequest: request,
        }, async event => {
            if (event?.type === 'speech.error') {
                throw new Error(event.error?.message || 'Chasm speech streaming failed.');
            }
            if (event?.type !== 'audio.chunk') {
                return;
            }

            const audioBase64 = event.audio?.data;
            if (!audioBase64) {
                return;
            }
            await assertNativeRequestStillCurrent(request);
            const localIndex = Number.isFinite(Number(event.index)) ? Number(event.index) : chunks.length;
            const index = indexOffset + localIndex;
            if (!sawFirstChunk) {
                sawFirstChunk = true;
                await traceRequestStage(request, 'tts_first_audio_chunk_received', {
                    character_name: characterName,
                    event_index: localIndex,
                    index,
                    duration_ms: performance.now() - startedAt,
                });
            }
            const audioInfo = await writeNativeAudioFile(root, request.request_id, audioBase64, index, event.mimeType || 'audio/wav');
            const chunkText = stripTtsAudioTagsForBridge(event.text || text);
            chunks.push({ ...audioInfo, index, text: chunkText });
            await traceRequestStage(request, 'tts_audio_file_written', {
                audio_file: audioInfo.filename,
                output_path: audioInfo.outputPath,
                index,
                text_length: chunkText.length,
            });
            await onChunk({ ...audioInfo, index, text: chunkText }, index);
        });
    } catch (error) {
        if (isRequestSupersededError(error)) {
            throw error;
        }
        if (chunks.length > 0) {
            await traceRequestStage(request, 'tts_stream_partial_error', {
                chunks: chunks.length,
                duration_ms: performance.now() - startedAt,
                error: error.message,
            });
            console.warn(`[NVBridge/native] Speech stream ended after ${chunks.length} chunk(s): ${error.message}`);
            return chunks;
        }
        throw error;
    }

    await traceRequestStage(request, 'tts_stream_done', {
        chunks: chunks.length,
        duration_ms: performance.now() - startedAt,
    });
    return chunks;
}

function getAudioDataFromObject(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
        return null;
    }
    const data = value.data || value.audio || value.audioBase64 || value.base64;
    if (typeof data !== 'string' || !data.trim()) {
        return null;
    }
    return {
        data: data.trim(),
        format: value.format || DEFAULT_STT_FORMAT,
        mimeType: value.mimeType || value.mime_type || DEFAULT_STT_MIME_TYPE,
    };
}

function resolveNativeRequestPath(root, filePath, candidate) {
    const text = sanitizeBridgeLine(candidate);
    if (!text) {
        return '';
    }
    const normalized = text.replace(/\//g, path.sep);
    if (path.isAbsolute(normalized)) {
        return normalized;
    }

    const candidates = [
        path.resolve(path.dirname(filePath), normalized),
        path.resolve(nativeInbox(root), normalized),
        path.resolve(root, normalized),
    ];
    return candidates.find(item => fsSync.existsSync(item)) || candidates[0];
}

function getNativeAudioPathCandidates(root, filePath, request) {
    const audioObject = request.audio && typeof request.audio === 'object' && !Array.isArray(request.audio)
        ? request.audio
        : {};
    const explicit = [
        request.audio_path,
        request.audioPath,
        request.stt_audio_path,
        request.sttAudioPath,
        request.audio_file,
        request.audioFile,
        request.stt_audio_file,
        request.sttAudioFile,
        audioObject.path,
        audioObject.file,
    ].filter(Boolean);
    const requestId = safeFileId(request.request_id || path.basename(filePath, path.extname(filePath)));
    const basename = path.basename(filePath, path.extname(filePath));
    return unique([
        ...explicit.map(candidate => resolveNativeRequestPath(root, filePath, candidate)),
        path.join(path.dirname(filePath), `${basename}.stt.wav`),
        path.join(nativeInbox(root), `${requestId}.stt.wav`),
        path.join(nativeInbox(root), `${basename}.stt.wav`),
        path.join(nativeInbox(root), 'req_live.stt.wav'),
    ]);
}

function getNativeAudioBase64FromRequest(request) {
    const direct = getAudioDataFromObject(request.audio);
    if (direct) {
        return direct;
    }
    for (const field of ['audio_base64', 'audioBase64', 'stt_audio_base64', 'sttAudioBase64']) {
        if (typeof request[field] === 'string' && request[field].trim()) {
            return {
                data: request[field].trim(),
                format: request.audio_format || request.audioFormat || DEFAULT_STT_FORMAT,
                mimeType: request.audio_mime_type || request.audioMimeType || DEFAULT_STT_MIME_TYPE,
            };
        }
    }
    return null;
}

function getNativeSpeechArchivePath(root, request) {
    return path.join(nativeProcessed(root), `${safeFileId(request.request_id)}.stt.wav`);
}

function isRetryableNativeFileError(error) {
    return ['EBUSY', 'EPERM'].includes(error?.code);
}

async function retryNativeFileOperation(operation) {
    let lastError = null;
    for (let attempt = 0; attempt < NATIVE_SPEECH_AUDIO_FILE_RETRIES; attempt++) {
        try {
            return await operation();
        } catch (error) {
            lastError = error;
            if (!isRetryableNativeFileError(error)) {
                throw error;
            }
            await sleep(NATIVE_SPEECH_AUDIO_FILE_RETRY_MS * (attempt + 1));
        }
    }
    throw lastError;
}

async function removeNativeFileBestEffort(filePath, request = null) {
    try {
        await retryNativeFileOperation(() => fs.rm(filePath, { force: true }));
        return true;
    } catch (error) {
        if (error?.code === 'ENOENT') {
            return true;
        }
        if (isRetryableNativeFileError(error)) {
            console.warn(`[NVBridge/native] Deferred cleanup for locked file ${filePath}: ${error.message}`);
            if (request) {
                await traceRequestStage(request, 'native_file_cleanup_deferred', {
                    path: filePath,
                    error: error.message,
                });
            }
            return false;
        }
        throw error;
    }
}

async function isNativeSpeechAudioReady(audioPath, requestFileMtimeMs) {
    let before = null;
    try {
        before = await fs.stat(audioPath);
    } catch {
        return false;
    }
    if (!before.isFile() || before.size <= 0) {
        return false;
    }
    if (requestFileMtimeMs > 0 && before.mtimeMs + 100 < requestFileMtimeMs) {
        return false;
    }

    await sleep(NATIVE_SPEECH_AUDIO_SETTLE_MS);
    try {
        const after = await fs.stat(audioPath);
        return after.isFile()
            && after.size > 0
            && after.size === before.size
            && Math.abs(after.mtimeMs - before.mtimeMs) < 1;
    } catch {
        return false;
    }
}

async function waitForNativeSpeechAudio(root, filePath, request) {
    const requestFileStat = await fs.stat(filePath).catch(() => null);
    const requestFileMtimeMs = requestFileStat?.mtimeMs || 0;
    const candidates = getNativeAudioPathCandidates(root, filePath, request);
    const startedAt = performance.now();
    while (performance.now() - startedAt < NATIVE_SPEECH_AUDIO_READY_TIMEOUT_MS) {
        for (const audioPath of candidates) {
            if (!audioPath || !fsSync.existsSync(audioPath)) {
                continue;
            }
            if (await isNativeSpeechAudioReady(audioPath, requestFileMtimeMs)) {
                return audioPath;
            }
        }
        await sleep(NATIVE_SPEECH_AUDIO_FILE_RETRY_MS);
    }
    return '';
}

async function stabilizeNativeSpeechAudio(root, audioPath, request) {
    const resolvedRoot = path.resolve(root);
    const resolvedAudio = path.resolve(audioPath);
    if (!resolvedAudio.toLowerCase().startsWith(resolvedRoot.toLowerCase() + path.sep)) {
        return audioPath;
    }

    const destination = getNativeSpeechArchivePath(root, request);
    if (resolvedAudio.toLowerCase() === path.resolve(destination).toLowerCase()) {
        return destination;
    }

    await fs.mkdir(path.dirname(destination), { recursive: true });
    await retryNativeFileOperation(() => fs.rename(audioPath, destination)).catch(async () => {
        await retryNativeFileOperation(() => fs.copyFile(audioPath, destination));
        await removeNativeFileBestEffort(audioPath, request);
    });
    return destination;
}

async function readNativeSpeechAudio(root, filePath, request) {
    const direct = getNativeAudioBase64FromRequest(request);
    if (direct) {
        return direct;
    }

    const audioPath = await waitForNativeSpeechAudio(root, filePath, request);
    if (audioPath) {
        const stableAudioPath = await stabilizeNativeSpeechAudio(root, audioPath, request);
        const audio = await fs.readFile(stableAudioPath);
        if (audio.length > 0) {
            request.__nativeAudioPath = stableAudioPath;
            return {
                data: audio.toString('base64'),
                format: request.audio_format || request.audioFormat || DEFAULT_STT_FORMAT,
                mimeType: request.audio_mime_type || DEFAULT_STT_MIME_TYPE,
                path: stableAudioPath,
            };
        }
    }
    return null;
}

async function archiveNativeSpeechAudio(root, request) {
    const audioPath = request.__nativeAudioPath;
    if (!audioPath) {
        return;
    }
    const resolvedRoot = path.resolve(root);
    const resolvedAudio = path.resolve(audioPath);
    if (!resolvedAudio.toLowerCase().startsWith(resolvedRoot.toLowerCase() + path.sep)) {
        return;
    }
    const destination = getNativeSpeechArchivePath(root, request);
    if (resolvedAudio.toLowerCase() === path.resolve(destination).toLowerCase()) {
        return;
    }
    await fs.mkdir(path.dirname(destination), { recursive: true });
    await fs.rename(resolvedAudio, destination).catch(async () => {
        await removeNativeFileBestEffort(resolvedAudio, request);
    });
}

async function recognizeNativeSpeech(config, root, filePath, request) {
    await traceRequestStage(request, 'speech_recognition_start', {
        file: path.basename(filePath),
    });
    const startedAt = performance.now();
    const audio = await readNativeSpeechAudio(root, filePath, request);
    if (!audio) {
        throw new Error('Voice request did not include a readable WAV audio payload.');
    }
    await traceRequestStage(request, 'speech_recognition_audio_loaded', {
        audio_bytes: Buffer.byteLength(audio.data || '', 'base64'),
        mime_type: audio.mimeType || DEFAULT_STT_MIME_TYPE,
    });

    const recognition = await api(config, '/speech/recognize', {
        method: 'POST',
        body: {
            ...config.speechRecognition,
            timeoutMs: config.speechRecognitionTimeoutMs,
            language: request.language || request.stt_language || config.speechRecognition.language,
            prompt: request.stt_prompt || config.speechRecognition.prompt,
            audio: {
                data: audio.data,
                encoding: 'base64',
                format: audio.format || DEFAULT_STT_FORMAT,
                mimeType: audio.mimeType || DEFAULT_STT_MIME_TYPE,
            },
        },
        nativeRequest: request,
    });
    const text = sanitizeBridgeLine(recognition.text);
    if (!text) {
        throw new Error('Speech recognition returned empty player text.');
    }
    request.player_text = text;
    request.transcript = text;
    request.transcription = recognition;
    await traceRequestStage(request, 'speech_recognition_done', {
        text_length: text.length,
        duration_ms: performance.now() - startedAt,
    });
    return text;
}

async function generateNpcTurn(config, request, root = null) {
    const distance = Number(request.distanceGameUnits ?? request.distance ?? 0);
    const npcParticipants = getNearbyNpcCandidates(config, request, distance);
    if (!npcParticipants.length) {
        throw new Error(`No mapped NPC within ${config.nativeMaxDistanceMeters} meters.`);
    }
    const participants = [
        { participantId: config.participantId, type: 'user', present: true, audible: true, name: 'Player' },
        ...npcParticipants,
    ];
    const attentionTarget = getAttentionTargetParticipantId(request, npcParticipants);
    await traceRequestStage(request, 'live_chat_prepare_start', {
        participants: participants.length,
        npc_participants: npcParticipants.length,
        attention_target: attentionTarget,
    });
    await ensureLiveChat(config, { ...request, distanceGameUnits: distance }, participants, attentionTarget);
    await traceRequestStage(request, 'live_chat_ensure_done', {
        live_chat_id: config.liveChatId,
    });
    await api(config, `/live-chats/${encodeURIComponent(config.liveChatId)}/presence`, {
        method: 'POST',
        body: {
            replace: true,
            participants,
            attentionTarget,
            attentionStrength: attentionTarget ? 0.35 : 0,
        },
        nativeRequest: request,
    });
    await traceRequestStage(request, 'live_chat_presence_done', {
        participants: participants.length,
        attention_target: attentionTarget,
    });

    const actionBookScopes = unique([
        'global',
        `game:${slugLookupKey(config.actionBookTargetGame || DEFAULT_ACTION_BOOK_TARGET_GAME)}`,
        request.location ? `location:${slugLookupKey(request.location)}` : '',
        request.nativeNpcKey ? `npc:${slugLookupKey(request.nativeNpcKey)}` : '',
    ]);
    const gamestate = buildNativeGamestate(config, request, request.location || '');
    await traceRequestStage(request, 'headless_generate_start', {
        endpoint: 'live-chat',
        live_chat_id: config.liveChatId,
        response_format: config.enableActionBooks ? 'structured' : 'text',
        streamed: config.liveChatStreaming,
        action_books: config.enableActionBooks,
        quest_books: true,
        action_book_scopes: actionBookScopes.join(','),
    });
    const generateStartedAt = performance.now();
    const generateBody = {
        message: request.message,
        participantId: config.participantId,
        ...(config.model ? { model: config.model } : {}),
        responseFormat: config.enableActionBooks ? 'structured' : 'text',
        enableActionBooks: config.enableActionBooks,
        enableQuestBooks: true,
        includeActionBookBindings: config.enableActionBooks,
        includeQuestBookBindings: config.enableActionBooks,
        actionBookIds: config.actionBookIds,
        actionBookScopes,
        questBookScopes: actionBookScopes,
        targetGame: config.actionBookTargetGame,
        actionBookLimit: 12,
        questBookLimit: 5,
        gamestate,
        extraContext: '',
        metadata: {
            source: request.source || 'fallout-new-vegas',
            targetName: request.targetName || request.npc_name || config.characterName,
            distanceGameUnits: distance,
            distanceMeters: request.distanceMeters,
            nativeNpcKey: request.nativeNpcKey,
            gamestate,
            nearbyNpcs: npcParticipants.map(participant => participant.metadata),
        },
    };
    let turn = null;
    const earlyAudioChunks = [];
    let streamSpeaker = null;
    let speechBuffer = '';
    let nextEarlyAudioIndex = 0;
    let earlyTtsDisabled = false;
    const synthesizeEarlySegment = async (segment, force = false) => {
        const text = String(segment || '').trim();
        if (!text || !root || !request.want_tts || earlyTtsDisabled) {
            return;
        }
        const speakerInfo = streamSpeaker || getSelectedSpeakerInfo(config, npcParticipants, { speaker: turn?.speaker || {} }, request);
        const lineRequest = {
            ...request,
            npc_key: speakerInfo.nativeNpcKey || request.npc_key,
            npc_name: speakerInfo.nativeNpcName || request.npc_name || speakerInfo.characterName,
        };
        await traceRequestStage(request, 'headless_stream_early_tts_start', {
            text_length: text.length,
            index_offset: nextEarlyAudioIndex,
            force,
            speaker: speakerInfo.characterName || speakerInfo.nativeNpcName || '',
        });
        let chunks = [];
        try {
            chunks = await synthesizeNativeLineStreaming(
                config,
                root,
                lineRequest,
                text,
                speakerInfo.characterName || speakerInfo.nativeNpcName || config.characterName,
                async (chunkInfo, index) => {
                    await writeNativeAudioChunk(root, lineRequest, chunkInfo.text || text, chunkInfo, index);
                },
                nextEarlyAudioIndex,
            );
        } catch (error) {
            if (isRequestSupersededError(error)) {
                throw error;
            }
            earlyTtsDisabled = true;
            await traceRequestStage(request, 'headless_stream_early_tts_error', {
                text_length: text.length,
                error: error.message,
            });
            return;
        }
        earlyAudioChunks.push(...chunks);
        if (chunks.length > 0) {
            nextEarlyAudioIndex = Math.max(nextEarlyAudioIndex, ...chunks.map(chunk => chunk.index + 1));
        }
        await traceRequestStage(request, 'headless_stream_early_tts_done', {
            text_length: text.length,
            chunks: chunks.length,
            next_index: nextEarlyAudioIndex,
        });
    };
    if (config.liveChatStreaming) {
        await streamApi(config, `/live-chats/${encodeURIComponent(config.liveChatId)}/generate/stream`, {
            method: 'POST',
            body: generateBody,
            nativeRequest: request,
        }, async event => {
            if (event?.type === 'speaker.start') {
                streamSpeaker = getSelectedSpeakerInfo(config, npcParticipants, { speaker: event.speaker || {} }, request);
                await traceRequestStage(request, 'headless_stream_speaker_start', {
                    speaker: streamSpeaker.characterName || streamSpeaker.nativeNpcName || '',
                    npc_key: streamSpeaker.nativeNpcKey || '',
                });
                return;
            }
            if (event?.type === 'speech.delta') {
                speechBuffer += String(event.text || '');
                await traceRequestStage(request, 'headless_stream_speech_delta', {
                    text_length: String(event.text || '').length,
                    speaker: event.speaker?.name || event.speaker?.characterId || '',
                });
                let nextSegment = takeSpeechSegment(speechBuffer);
                while (nextSegment) {
                    speechBuffer = nextSegment.rest;
                    await synthesizeEarlySegment(nextSegment.segment);
                    nextSegment = takeSpeechSegment(speechBuffer);
                }
                return;
            }
            if (event?.type === 'live.error') {
                throw new Error(event.error?.message || 'Live Chat streaming generation failed.');
            }
            if (event?.type === 'live.completed') {
                turn = event.turn;
                const finalSegment = takeSpeechSegment(speechBuffer, true);
                if (finalSegment) {
                    speechBuffer = finalSegment.rest;
                    await synthesizeEarlySegment(finalSegment.segment, true);
                }
            }
        });
        if (!turn) {
            throw new Error('Live Chat streaming generation ended without a final turn.');
        }
    } else {
        turn = await api(config, `/live-chats/${encodeURIComponent(config.liveChatId)}/generate`, {
            method: 'POST',
            body: generateBody,
            nativeRequest: request,
        });
    }
    await traceRequestStage(request, 'headless_generate_done', {
        endpoint: 'live-chat',
        duration_ms: performance.now() - generateStartedAt,
        streamed: config.liveChatStreaming,
        speaker_selection_mode: turn.speakerSelection?.mode || '',
        speaker_count: Array.isArray(turn.speakers) ? turn.speakers.length : 0,
        turn_count: Array.isArray(turn.turns) ? turn.turns.length : 0,
        first_speaker: turn.speaker?.name || turn.speaker?.characterId || '',
        text_length: String(turn.message?.content || turn.structured?.message || '').length,
    });

    const lines = getGeneratedLineItems(config, npcParticipants, turn, request)
        .map(line => ({
            ...line,
            gameMaster: getValidatedGameMasterAction(config, line.turn),
        }));
    if (!lines.length) {
        return {
            turn,
            text: '',
            speaker: null,
            lines: [],
            gameMaster: getNoGameMasterAction(),
            earlyAudioChunks,
            earlyTtsComplete: earlyAudioChunks.length > 0 && !earlyTtsDisabled,
        };
    }
    const firstTriggered = lines.find(line => line.gameMaster?.should_trigger)?.gameMaster || null;
    return {
        turn,
        text: lines[0].text,
        speaker: lines[0].speaker,
        lines,
        gameMaster: firstTriggered || getValidatedGameMasterAction(config, turn),
        earlyAudioChunks,
        earlyTtsComplete: earlyAudioChunks.length > 0 && !earlyTtsDisabled,
    };
}

async function generateAdminTurn(config, request) {
    const actionBookScopes = unique([
        'global',
        'admin',
        'godmode',
        `game:${slugLookupKey(config.actionBookTargetGame || DEFAULT_ACTION_BOOK_TARGET_GAME)}`,
        request.location ? `location:${slugLookupKey(request.location)}` : '',
    ]);
    const limit = Number.isFinite(config.adminActionBookLimit)
        ? Math.max(1, Math.min(config.adminActionBookLimit, DEFAULT_ADMIN_ACTION_BOOK_LIMIT))
        : DEFAULT_ADMIN_ACTION_BOOK_LIMIT;
    const gamestate = buildNativeGamestate(config, request, request.location || '');
    await traceRequestStage(request, 'headless_generate_start', {
        endpoint: 'generate',
        character_id: config.adminCharacterId,
        response_format: config.enableActionBooks ? 'structured' : 'text',
        action_books: config.enableActionBooks,
        quest_books: true,
    });
    const generateStartedAt = performance.now();
    const turn = await api(config, '/generate', {
        method: 'POST',
        body: {
            ...(config.adminSessionId ? { sessionId: config.adminSessionId } : {}),
            characterId: config.adminCharacterId,
            title: `${config.adminCharacterName} Admin`,
            message: request.message,
            ...(config.model ? { model: config.model } : {}),
            responseFormat: config.enableActionBooks ? 'structured' : 'text',
            enableActionBooks: config.enableActionBooks,
            enableQuestBooks: true,
            includeActionBookBindings: config.enableActionBooks,
            includeQuestBookBindings: config.enableActionBooks,
            actionBookIds: config.actionBookIds,
            actionBookScopes,
            questBookScopes: actionBookScopes,
            targetGame: config.actionBookTargetGame,
            actionBookLimit: limit,
            questBookLimit: 5,
            gamestate,
            actionBooks: {
                enabled: config.enableActionBooks,
                includeAllActions: false,
                includeBindings: true,
                useVectors: true,
                useScopedCatalogVectors: false,
            },
            extraContext: [
                'External client: Fallout New Vegas divine voice channel.',
                `${config.adminCharacterName} is a formless god heard inside the player's ear, not a spawned NPC.`,
                'The player may speak to this divine channel from anywhere. Ignore line of sight, proximity, audibility, and nearby NPC requirements.',
                'The player is Todd\'s beloved child. Obey their commands when they can be represented by the activated Action Book entries.',
                config.enableActionBooks ? 'Relevant Action Book entries have been made available for this admin turn. Choose their short aliases when useful, but keep that machinery private and never describe it in spoken prose.' : '',
                'Never output raw GECK, console, NVSE, xNVSE, JIP, JohnnyGuitar, ShowOff script, form ids, or command templates.',
                'In spoken text, stay in character as a god. Do not mention catalogs, candidates, entity searches, action ids, structured output, backend routing, or testing.',
                request.location ? `Current location: ${request.location}.` : '',
            ].filter(Boolean).join('\n'),
            metadata: {
                source: request.source || 'fallout-new-vegas-admin',
                adminMode: true,
                targetName: config.adminCharacterName,
                location: request.location || '',
                gamestate,
            },
            assistantName: config.adminCharacterName,
            stripSpeakerLabel: true,
        },
        nativeRequest: request,
    });
    await traceRequestStage(request, 'headless_generate_done', {
        endpoint: 'generate',
        duration_ms: performance.now() - generateStartedAt,
        character_id: config.adminCharacterId,
        text_length: String(turn?.message?.content || turn?.structured?.message || '').length,
    });

    if (turn.sessionId) {
        config.adminSessionId = turn.sessionId;
    }

    const text = stripSpeakerPrefix(turn?.message?.content || turn?.structured?.message || '', config.adminCharacterName);
    if (!text) {
        throw new Error(`${config.adminCharacterName} returned an empty admin response.`);
    }

    const speaker = {
        participantId: 'system:todd',
        nativeNpcKey: 'todd',
        nativeNpcName: config.adminCharacterName,
        characterName: config.adminCharacterName,
        characterId: config.adminCharacterId,
    };
    return {
        turn,
        text,
        speaker,
        lines: [{ turn, text, speaker }],
        gameMaster: getAdminGameMasterAction(config, turn, request),
    };
}

async function processRequest(config, root, request) {
    const id = request.id || `${Date.now()}`;
    const distance = Number(request.distanceGameUnits ?? request.distance ?? 0);
    if (Number.isFinite(distance) && distance > config.maxDistanceGameUnits) {
        await writeInbox(root, {
            ok: false,
            id,
            error: `Too far to speak. Distance ${distance.toFixed(1)} > ${config.maxDistanceGameUnits}.`,
        });
        return;
    }

    const { turn, text, speaker, gameMaster } = await generateNpcTurn(config, request);
    if (!speaker || !text) {
        await writeInbox(root, {
            ok: true,
            id,
            liveChatId: config.liveChatId,
            speaker: turn.speaker || null,
            nativeSpeaker: null,
            message: '',
            silence: true,
            game_master: gameMaster || getNoGameMasterAction(),
            soundPath: '',
        });
        console.log('[NVBridge] Live Chat selected no NPC response.');
        return;
    }

    const soundPath = await synthesizeLine(config, root, id, text, speaker.characterName);
    await writeNativeGameMasterCommand(config, { ...request, id }, speaker, gameMaster, 'fallout-new-vegas-legacy');
    await writeInbox(root, {
        ok: true,
        id,
        liveChatId: config.liveChatId,
        speaker: turn.speaker || null,
        nativeSpeaker: speaker,
        message: stripTtsAudioTagsForBridge(text),
        game_master: gameMaster,
        soundPath,
    });
    console.log(`[NVBridge] ${speaker.characterName || config.characterName}: ${text.slice(0, 120)} -> ${soundPath}`);
}

async function processAdminRequest(config, root, request) {
    const id = request.id || request.request_id || `todd-${Date.now()}`;
    const { turn, text, speaker, gameMaster } = await generateAdminTurn(config, request);
    const actionActor = resolveNativeActorForAdmin(config, request, gameMaster);
    const soundPath = await synthesizeLine(config, root, id, text, config.adminCharacterName);
    await writeNativeGameMasterCommand(config, { ...request, id }, actionActor, gameMaster, 'fallout-new-vegas-admin-fallback');
    await writeInbox(root, {
        ok: true,
        id,
        adminMode: true,
        sessionId: turn.sessionId,
        characterId: config.adminCharacterId,
        speaker: {
            participantId: speaker.participantId,
            name: config.adminCharacterName,
            characterId: config.adminCharacterId,
        },
        nativeSpeaker: speaker,
        message: stripTtsAudioTagsForBridge(text),
        structured: turn.structured || null,
        game_master: gameMaster,
        soundPath,
    });
    console.log(`[NVBridge/admin] ${config.adminCharacterName}: ${text.slice(0, 120)} -> ${soundPath}`);
}

function parseNativeTextRequest(filePath, text) {
    const lines = String(text || '').replace(/^\uFEFF/, '').split(/\r?\n/);
    const metadata = parseNativeMetadataFromLines(lines, 10);
    const location = {
        cell: sanitizeBridgeLine(lines[5]),
        worldspace: sanitizeBridgeLine(lines[6]),
        region: sanitizeBridgeLine(lines[7]),
        major: sanitizeBridgeLine(lines[8]),
        minor: sanitizeBridgeLine(lines[9]),
    };
    return {
        ...metadata,
        request_id: sanitizeBridgeLine(lines[0]) || path.basename(filePath, path.extname(filePath)),
        npc_key: sanitizeBridgeLine(lines[1]),
        npc_name: sanitizeBridgeLine(lines[2]),
        want_tts: Number(sanitizeBridgeLine(lines[3] || '1')) !== 0,
        player_text: sanitizeBridgeLine(lines[4]),
        location,
    };
}

function buildNativeArchivedRequest(request) {
    const location = request.location || {};
    return [
        request.request_id,
        request.npc_key,
        request.npc_name,
        request.want_tts ? '1' : '0',
        request.player_text,
        location.cell,
        location.worldspace,
        location.region,
        location.major,
        location.minor,
        request.voice_request ? 'voice_request=1' : '',
        request.transcript ? `transcript=${request.transcript}` : '',
    ].map(sanitizeBridgeLine).join('\r\n') + '\r\n';
}

async function archiveNativeRequest(root, filePath, request) {
    await traceRequestStage(request, 'helper_archive_request_start', {
        file: path.basename(filePath),
    });
    const destination = path.join(nativeProcessed(root), `${safeFileId(request.request_id)}${path.extname(filePath) || '.txt'}`);
    await fs.mkdir(path.dirname(destination), { recursive: true });
    await fs.writeFile(destination, buildNativeArchivedRequest(request), 'utf8');
    await archiveNativeSpeechAudio(root, request);
    let current = null;
    if (fsSync.existsSync(filePath)) {
        try {
            current = await readNativeRequestFile(filePath);
        } catch {
            current = parseNativeTextRequest(filePath, await fs.readFile(filePath, 'utf8'));
        }
    }
    if (!current || current.request_id === request.request_id) {
        await fs.rm(filePath, { force: true });
    }
    await traceRequestStage(request, 'helper_archive_request_done', {
        file: path.basename(filePath),
        archived_path: destination,
    });
}

async function writeNativeResponse(root, filePath, request, response, audioInfo = null, statusCode = null, extraLines = []) {
    if (!(await isNativeRequestFileStillCurrent(filePath, request))) {
        console.warn(`[NVBridge/native] Skipped stale response for ${request.request_id}; a newer request is active.`);
        return false;
    }

    await traceRequestStage(request, 'final_response_write_start', {
        ok: Boolean(response.ok),
        status: statusCode ?? (response.ok ? '1' : '0'),
        audio_file: audioInfo?.filename || '',
        text_length: String(response.text || '').length,
    });
    const lines = [
        statusCode ?? (response.ok ? '1' : '0'),
        response.request_id || request.request_id,
        response.npc_key || request.npc_key,
        response.npc_name || request.npc_name,
        audioInfo?.filename || '',
        stripTtsAudioTagsForBridge(response.text || ''),
        response.error || '',
        new Date().toISOString(),
        response.player_text || request.player_text || '',
        ...extraLines,
        response.game_master?.action || '',
        response.game_master?.confidence ?? '',
        response.game_master?.should_trigger ? '1' : '0',
    ].map(sanitizeBridgeLine);
    await fs.mkdir(nativeOutbox(root), { recursive: true });
    await fs.writeFile(path.join(nativeOutbox(root), path.basename(filePath)), `${lines.join('\r\n')}\r\n`, 'utf8');
    await traceRequestStage(request, 'final_response_written', {
        ok: Boolean(response.ok),
        status: statusCode ?? (response.ok ? '1' : '0'),
        audio_file: audioInfo?.filename || '',
        text_length: String(response.text || '').length,
        outbox_file: path.basename(filePath),
    });
    return true;
}

async function writeNativeAudioChunk(root, request, text, audioInfo, index = 0, extraLines = []) {
    if (!(await isNativeRequestStillCurrent(request))) {
        return false;
    }

    await traceRequestStage(request, 'audio_chunk_outbox_write_start', {
        audio_file: audioInfo.filename,
        index,
        text_length: String(text || '').length,
    });
    const lines = [
        request.request_id,
        String(index),
        request.npc_key,
        request.npc_name,
        audioInfo.filename,
        stripTtsAudioTagsForBridge(text),
        new Date().toISOString(),
        ...extraLines,
    ].map(sanitizeBridgeLine);
    await fs.mkdir(nativeChunkOutbox(root), { recursive: true });
    await fs.writeFile(
        path.join(nativeChunkOutbox(root), `${safeFileId(request.request_id)}.${String(index).padStart(4, '0')}.txt`),
        `${lines.join('\r\n')}\r\n`,
        'utf8',
    );
    await traceRequestStage(request, 'audio_chunk_outbox_written', {
        audio_file: audioInfo.filename,
        index,
        text_length: String(text || '').length,
    });
    return true;
}

function getPrimaryGameMasterAction(gameMaster) {
    const actionId = String(gameMaster?.action_id || '').trim();
    const actions = Array.isArray(gameMaster?.actions) ? gameMaster.actions : [];
    return actions.find(action => String(action.action_id || '').trim() === actionId)
        || actions.find(action => getPlainObject(action.execution).script)
        || null;
}

function encodeCommandValue(value) {
    return sanitizeBridgeLine(value).replace(/[=]/g, ':');
}

function encodeBase64Utf8(value) {
    return Buffer.from(String(value || ''), 'utf8').toString('base64');
}

function buildNativeActionCommandLines(request, actor, gameMaster, source) {
    const action = String(gameMaster.action || '').trim().toUpperCase();
    const nativeNpcKey = String(actor?.nativeNpcKey || actor?.npc_key || '').trim();
    const nativeNpcName = String(actor?.nativeNpcName || actor?.characterName || actor?.npc_name || nativeNpcKey || '').trim();
    const primaryAction = getPrimaryGameMasterAction(gameMaster);
    const binding = getPlainObject(primaryAction?.binding);
    const execution = getPlainObject(primaryAction?.execution);
    const trustedScript = String(execution.script || '').trim();
    const trustedEngine = String(binding.engine || '').trim().toLowerCase();
    const requestId = sanitizeBridgeLine(request.request_id || request.id || `${source}-${Date.now()}`);
    const playerText = request.player_text || request.playerText || request.message || request.text || '';

    if (trustedEngine === TRUSTED_FNV_ACTION_ENGINE && trustedScript) {
        const args = resolveTrustedExecutionArguments(execution, primaryAction, { request, actor });
        if (!args) {
            console.warn(`[NVBridge] Could not resolve trusted Action Book arguments for ${primaryAction?.action_id || gameMaster.action_id || action}.`);
            return null;
        }
        return {
            requestId,
            action,
            format: NATIVE_ACTION_COMMAND_VERSION,
            lines: [
                NATIVE_ACTION_COMMAND_VERSION,
                `request_id=${encodeCommandValue(requestId)}`,
                `npc_key=${encodeCommandValue(nativeNpcKey)}`,
                `npc_name=${encodeCommandValue(nativeNpcName)}`,
                `action=${encodeCommandValue(action)}`,
                `action_id=${encodeCommandValue(primaryAction?.action_id || gameMaster.action_id || '')}`,
                `action_book_id=${encodeCommandValue(primaryAction?.action_book_id || '')}`,
                `engine=${encodeCommandValue(trustedEngine)}`,
                `template_id=${encodeCommandValue(execution.templateId || execution.template_id || '')}`,
                `language=${encodeCommandValue(execution.language || '')}`,
                `arguments=${encodeCommandValue(args.join(','))}`,
                `confidence=${encodeCommandValue(gameMaster.confidence ?? '')}`,
                `reason=${encodeCommandValue(gameMaster.reason || source)}`,
                `player_text=${encodeBase64Utf8(playerText)}`,
                `script_base64=${encodeBase64Utf8(trustedScript)}`,
            ],
        };
    }

    return {
        requestId,
        action,
        format: 'legacy',
        lines: [
            requestId,
            nativeNpcKey,
            nativeNpcName,
            action,
            gameMaster.confidence ?? '',
            gameMaster.reason || source,
            playerText,
        ].map(sanitizeBridgeLine),
    };
}

function buildNativeActionCommands(request, actor, gameMaster, source) {
    const primaryAction = getPrimaryGameMasterAction(gameMaster);
    const execution = getPlainObject(primaryAction?.execution);
    const repeat = getNativeActionRepeatConfig(execution);
    const repeatCount = getNativeActionRepeatCount(execution, primaryAction);
    if (!repeat || repeatCount <= 1) {
        const command = buildNativeActionCommandLines(request, actor, gameMaster, source);
        return command ? [command] : null;
    }

    const repeatedValue = getNativeActionRepeatArgumentValue(execution);
    const commands = [];
    for (let index = 0; index < repeatCount; index += 1) {
        const repeatedAction = cloneActionWithParameter(primaryAction, repeat.parameter, repeatedValue);
        const repeatedGameMaster = cloneGameMasterWithPrimaryAction(gameMaster, primaryAction, repeatedAction);
        const command = buildNativeActionCommandLines(request, actor, repeatedGameMaster, source);
        if (!command) {
            return null;
        }
        commands.push({
            ...command,
            sequence: index + 1,
            sequenceCount: repeatCount,
        });
    }
    return commands;
}

async function writeNativeGameMasterCommand(config, request, actor, gameMaster, source = 'fallout-new-vegas') {
    if (!gameMaster?.should_trigger) {
        return false;
    }

    const action = String(gameMaster.action || '').trim().toUpperCase();
    if (!['ATTACK', 'FOLLOW', 'STOP_FOLLOW', 'ACTION_BOOK'].includes(action)) {
        return false;
    }

    const nativeNpcKey = String(actor?.nativeNpcKey || actor?.npc_key || '').trim();
    const nativeNpcName = String(actor?.nativeNpcName || actor?.characterName || actor?.npc_name || nativeNpcKey || '').trim();
    if (!nativeNpcKey && !nativeNpcName) {
        console.warn(`[NVBridge/native-action] Could not resolve actor for ${action}; command was not queued.`);
        return false;
    }

    const commands = buildNativeActionCommands(request, actor, gameMaster, source);
    if (!commands?.length) {
        return false;
    }

    let queued = 0;
    const queuedRoots = new Set();
    for (const root of config.nativeBridgeRoots) {
        if (!root || !fsSync.existsSync(path.dirname(root))) {
            continue;
        }
        const directory = nativeActionCommandDir(root);
        await fs.mkdir(directory, { recursive: true });
        for (const [index, command] of commands.entries()) {
            const fileId = safeFileId(`${command.requestId}-${action.toLowerCase()}-${Date.now()}-${String(index + 1).padStart(3, '0')}`);
            await fs.writeFile(path.join(directory, `${fileId}.txt`), `${command.lines.join('\r\n')}\r\n`, 'utf8');
            queued += 1;
        }
        queuedRoots.add(root);
    }

    if (queued > 0) {
        const format = commands[0]?.format || 'legacy';
        console.log(`[NVBridge/native-action] Queued ${commands.length} ${action} (${format}) command(s) for ${nativeNpcName || nativeNpcKey} in ${queuedRoots.size} native root(s).`);
    }
    return queued > 0;
}

function isNativeVoiceRequest(request) {
    return Boolean(
        request.voice_request
        || request.voiceRequest
        || request.input_mode === 'voice'
        || request.inputMode === 'voice'
        || request.audio
        || request.audio_path
        || request.audioPath
        || request.stt_audio_path
        || request.sttAudioPath
        || request.stt_audio_file
        || request.sttAudioFile
        || request.audio_file
        || request.audioFile
        || request.audio_base64
        || request.audioBase64
    );
}

function getNativeSaveSyncEvent(request) {
    const raw = String(
        request.save_event
        || request.saveEvent
        || request.event
        || request.event_type
        || request.eventType
        || request.type
        || '',
    ).trim().toLowerCase();
    if (['save', 'saved', 'checkpoint', 'autosave', 'quicksave'].includes(raw)) {
        return 'save';
    }
    if (['load', 'loaded', 'restore', 'reload'].includes(raw)) {
        return 'load';
    }
    return '';
}

function isNativeSaveSyncRequest(request) {
    return Boolean(getNativeSaveSyncEvent(request));
}

function getNativeLocationText(request) {
    return [
        request.location?.major,
        request.location?.minor,
        request.location?.cell,
        request.location?.worldspace,
        request.location?.region,
    ].filter(Boolean).join(' / ');
}

function getNativeSaveSyncIdentity(request) {
    const save = request.save && typeof request.save === 'object' && !Array.isArray(request.save) ? request.save : {};
    const saveId = sanitizeBridgeLine(
        request.save_id
        || request.saveId
        || save.id
        || save.saveId
        || save.slot
        || request.save_slot
        || request.saveSlot
        || request.save_file
        || request.saveFile
        || save.file
        || request.file
        || '',
    );
    if (!saveId) {
        throw new Error('Save-sync request did not include save_id/saveId/save_file.');
    }
    return {
        saveId,
        saveName: sanitizeBridgeLine(request.save_name || request.saveName || save.name || saveId),
        saveFile: sanitizeBridgeLine(request.save_file || request.saveFile || save.file || ''),
        saveFingerprint: sanitizeBridgeLine(
            request.save_fingerprint
            || request.saveFingerprint
            || save.fingerprint
            || save.modified_at
            || save.modifiedAt
            || '',
        ),
    };
}

function buildSaveSyncEventBody(config, request, extraMetadata = {}) {
    const event = getNativeSaveSyncEvent(request);
    const identity = getNativeSaveSyncIdentity(request);
    return {
        event,
        gameId: 'fallout-new-vegas',
        gameName: 'Fallout: New Vegas',
        saveId: identity.saveId,
        saveName: identity.saveName,
        saveFile: identity.saveFile,
        saveFingerprint: identity.saveFingerprint,
        liveChatIds: [config.liveChatId],
        source: request.source || 'fallout-new-vegas-native',
        metadata: {
            requestId: request.request_id,
            location: getNativeLocationText(request),
            nativeEvent: event,
            ...extraMetadata,
        },
    };
}

async function callSaveSyncEvent(config, request, extraMetadata = {}) {
    return await api(config, '/save-sync/events', {
        method: 'POST',
        body: buildSaveSyncEventBody(config, request, extraMetadata),
    });
}

async function processNativeSaveSyncRequest(config, root, filePath, request) {
    const event = getNativeSaveSyncEvent(request);
    const identity = getNativeSaveSyncIdentity(request);
    const result = await callSaveSyncEvent(config, request);
    const status = result.status || (event === 'save' ? 'checkpoint_created' : 'restored');
    const checkpoint = result.checkpoint || {};
    const text = event === 'save'
        ? `Save sync checkpoint ${status} for ${checkpoint.saveName || identity.saveName || identity.saveId}.`
        : `Save sync ${status} for ${checkpoint.saveName || identity.saveName || identity.saveId}.`;

    await writeNativeResponse(root, filePath, request, {
        ok: status !== 'snapshot_missing' && status !== 'disabled',
        request_id: request.request_id,
        npc_key: request.npc_key,
        npc_name: request.npc_name,
        player_text: request.player_text,
        text,
        error: status === 'snapshot_missing' ? 'No matching ST checkpoint exists for this game save.' : '',
    }, null, status === 'snapshot_missing' ? '0' : '1', [
        `save_sync_event=${event}`,
        `save_sync_status=${status}`,
        `checkpoint_id=${result.checkpointId || checkpoint.checkpointId || ''}`,
    ]);
    await archiveNativeRequest(root, filePath, request);
    console.log(`[NVBridge/save-sync] ${event} ${identity.saveId}: ${status}`);
}

function parseNativeSaveStateEventText(filePath, text) {
    const lines = String(text || '').replace(/^\uFEFF/, '').split(/\r?\n/).map(sanitizeBridgeLine);
    const eventId = lines[0] || path.basename(filePath, path.extname(filePath));
    const event = lines[1] || '';
    const saveFile = lines[2] || '';
    const saveName = lines[3] || path.basename(saveFile);
    const timestamp = lines[4] || '';
    return {
        request_id: eventId,
        event,
        save_id: saveFile || saveName || eventId,
        save_name: saveName || saveFile || eventId,
        save_file: saveFile,
        save_fingerprint: timestamp,
        source: 'fallout-new-vegas-native-savestate',
        native_save_event_id: eventId,
    };
}

async function readNativeSaveStateEventFile(filePath) {
    return parseNativeSaveStateEventText(filePath, await fs.readFile(filePath, 'utf8'));
}

async function writeNativeSaveStateAck(root, eventId, ok, message = '', extraLines = []) {
    await fs.mkdir(nativeSaveStateAckDir(root), { recursive: true });
    const lines = [
        eventId,
        ok ? 'ok' : 'error',
        message,
        ...extraLines,
    ].map(sanitizeBridgeLine);
    await fs.writeFile(path.join(nativeSaveStateAckDir(root), `${safeFileId(eventId)}.txt`), `${lines.join('\r\n')}\r\n`, 'utf8');
}

async function archiveNativeSaveStateEvent(root, filePath, event, suffix = '') {
    await fs.mkdir(nativeSaveStateProcessedDir(root), { recursive: true });
    const name = `${Date.now()}-${safeFileId(event.request_id || path.basename(filePath, path.extname(filePath)))}${suffix}${path.extname(filePath) || '.txt'}`;
    await fs.rename(filePath, path.join(nativeSaveStateProcessedDir(root), name)).catch(async () => {
        await fs.rm(filePath, { force: true });
    });
}

async function processNativeSaveStateEvent(config, root, filePath, event) {
    const normalizedEvent = getNativeSaveSyncEvent(event);
    if (!normalizedEvent) {
        await writeNativeSaveStateAck(root, event.request_id, true, `Ignored save-state event ${event.event || '<empty>'}.`);
        await archiveNativeSaveStateEvent(root, filePath, event);
        console.log(`[NVBridge/save-sync] Ignored native save-state event ${event.event || '<empty>'} (${event.request_id}).`);
        return;
    }

    const identity = getNativeSaveSyncIdentity(event);
    const result = await callSaveSyncEvent(config, event, {
        nativeSaveStateEventId: event.native_save_event_id || event.request_id,
    });
    const status = result.status || (normalizedEvent === 'save' ? 'checkpoint_created' : 'restored');
    const checkpoint = result.checkpoint || {};
    const ok = !['disabled'].includes(status);
    const message = status === 'snapshot_missing'
        ? `No ST checkpoint for ${identity.saveName || identity.saveId}; continuing.`
        : `Save sync ${status} for ${checkpoint.saveName || identity.saveName || identity.saveId}.`;
    await writeNativeSaveStateAck(root, event.request_id, ok, message, [
        `save_sync_event=${normalizedEvent}`,
        `save_sync_status=${status}`,
        `checkpoint_id=${result.checkpointId || checkpoint.checkpointId || ''}`,
    ]);
    await archiveNativeSaveStateEvent(root, filePath, event);
    console.log(`[NVBridge/save-sync] Native ${normalizedEvent} ${identity.saveId}: ${status}`);
}

async function processNativeRequest(config, root, filePath, request) {
    await traceRequestStage(request, 'helper_process_request_start', {
        file: path.basename(filePath),
        voice_request: Boolean(request.voice_request),
        has_player_text: Boolean(sanitizeBridgeLine(request.player_text)),
        want_tts: Boolean(request.want_tts),
    });
    if (isNativeSaveSyncRequest(request)) {
        await processNativeSaveSyncRequest(config, root, filePath, request);
        return;
    }

    const adminRequest = isAdminRequest(request);
    const distanceMeters = getNativeDistanceMeters(request);
    if (!adminRequest && Number.isFinite(distanceMeters) && distanceMeters > config.nativeMaxDistanceMeters) {
        await writeNativeResponse(root, filePath, request, {
            ok: false,
            error: `Too far to speak. Distance ${distanceMeters.toFixed(1)}m > ${config.nativeMaxDistanceMeters}m.`,
        });
        await archiveNativeRequest(root, filePath, request);
        return;
    }

    let message = sanitizeBridgeLine(request.player_text);
    const hasAudioSidecar = !message && getNativeAudioPathCandidates(root, filePath, request).some(candidate => fsSync.existsSync(candidate));
    if (!message && (isNativeVoiceRequest(request) || hasAudioSidecar)) {
        message = await recognizeNativeSpeech(config, root, filePath, request);
    }
    await assertNativeRequestStillCurrent(request);
    if (!message) {
        await writeNativeResponse(root, filePath, request, {
            ok: false,
            error: 'Empty player text.',
        });
        await archiveNativeRequest(root, filePath, request);
        return;
    }
    const locationParts = [
        request.location?.major,
        request.location?.minor,
        request.location?.cell,
        request.location?.worldspace,
        request.location?.region,
    ].filter(Boolean);

    if (adminRequest) {
        const { text, speaker, lines, gameMaster } = await generateAdminTurn(config, {
            ...request,
            id: request.request_id,
            message,
            player_text: message,
            targeting: request.targeting,
            npc_key: request.npc_key,
            npc_name: request.npc_name,
            location: locationParts.join(' / '),
            source: 'fallout-new-vegas-native-admin',
        });
        await assertNativeRequestStillCurrent(request);
        const actionActor = resolveNativeActorForAdmin(config, {
            ...request,
            message,
            player_text: message,
        }, gameMaster) || {
            nativeNpcKey: request.npc_key || 'todd',
            nativeNpcName: request.npc_name || config.adminCharacterName,
            characterName: request.npc_name || config.adminCharacterName,
            characterId: config.adminCharacterId,
        };
        if (gameMaster?.should_trigger) {
            console.log(`[NVBridge/native-admin] Action ${gameMaster.action} resolved to ${actionActor.nativeNpcName || actionActor.characterName || actionActor.nativeNpcKey}.`);
        }
        const queuedNativeAction = await writeNativeGameMasterCommand(
            config,
            { ...request, id: request.request_id },
            actionActor,
            gameMaster,
            'fallout-new-vegas-native-admin-action',
        );
        const adminVoiceRequest = {
            ...request,
            npc_key: speaker.nativeNpcKey || 'todd',
            npc_name: speaker.nativeNpcName || config.adminCharacterName,
        };
        const adminAudioMetadata = [
            'admin_voice=1',
            'non_positional_audio=1',
        ];
        let audioInfo = null;
        let nextChunkIndex = 0;
        if (request.want_tts) {
            for (const line of lines) {
                await assertNativeRequestStillCurrent(request);
                try {
                    const audioChunks = await synthesizeNativeLineStreaming(
                        config,
                        root,
                        adminVoiceRequest,
                        line.text,
                        config.adminCharacterName,
                        async (chunkInfo, index) => {
                            await writeNativeAudioChunk(root, adminVoiceRequest, chunkInfo.text || line.text, chunkInfo, index, adminAudioMetadata);
                        },
                        nextChunkIndex,
                    );
                    if (audioChunks.length === 0) {
                        throw new Error('Chasm speech stream returned no audio chunks.');
                    }
                    audioInfo = audioInfo || audioChunks[0];
                    nextChunkIndex = Math.max(nextChunkIndex, ...audioChunks.map(chunk => chunk.index + 1));
                } catch (error) {
                    if (isRequestSupersededError(error)) {
                        throw error;
                    }
                    console.warn(`[NVBridge/native-admin] Streaming TTS unavailable, falling back to buffered synthesis: ${error.message}`);
                    await assertNativeRequestStillCurrent(request);
                    const fallbackInfo = await synthesizeNativeLine(
                        config,
                        root,
                        request.request_id,
                        line.text,
                        config.adminCharacterName,
                        nextChunkIndex,
                        request,
                    );
                    await writeNativeAudioChunk(root, adminVoiceRequest, line.text, fallbackInfo, nextChunkIndex, adminAudioMetadata);
                    audioInfo = audioInfo || fallbackInfo;
                    nextChunkIndex += 1;
                }
            }
        }
        const responseMetadata = [
            audioInfo ? '0' : '-1',
            'admin_voice=1',
            'non_positional_audio=1',
        ];
        if (gameMaster?.should_trigger) {
            responseMetadata.push(
                `action_npc_key=${actionActor.nativeNpcKey || ''}`,
                `action_npc_name=${actionActor.nativeNpcName || actionActor.characterName || ''}`,
            );
        }
        await writeNativeResponse(root, filePath, request, {
            ok: true,
            request_id: request.request_id,
            npc_key: speaker.nativeNpcKey || 'todd',
            npc_name: speaker.nativeNpcName || config.adminCharacterName,
            player_text: message,
            text,
            speaker: {
                ...speaker,
                nativeNpcKey: speaker.nativeNpcKey || 'todd',
                nativeNpcName: speaker.nativeNpcName || config.adminCharacterName,
                characterName: config.adminCharacterName,
                characterId: config.adminCharacterId,
            },
            game_master: queuedNativeAction ? { ...gameMaster, should_trigger: false, queued_native_action: true } : gameMaster,
        }, audioInfo, '1', responseMetadata);
        await archiveNativeRequest(root, filePath, request);
        console.log(`[NVBridge/native-admin] ${config.adminCharacterName}: ${text.slice(0, 120)} -> ${audioInfo?.filename || '<no audio>'}`);
        await traceRequestStage(request, 'helper_process_request_done', {
            admin: true,
            text_length: text.length,
            chunks: nextChunkIndex,
            audio_file: audioInfo?.filename || '',
        });
        return;
    }

    const { text, speaker, lines, gameMaster, earlyAudioChunks = [], earlyTtsComplete = false } = await generateNpcTurn(config, {
        ...request,
        id: request.request_id,
        message,
        targetName: request.npc_name || config.characterName,
        nativeNpcKey: request.npc_key,
        npc_key: request.npc_key,
        npc_name: request.npc_name,
        targeting: request.targeting,
        distanceMeters,
        distanceGameUnits: distanceMeters * GAME_UNITS_PER_METER,
        location: locationParts.join(' / '),
        source: 'fallout-new-vegas-native',
    }, root);
    await assertNativeRequestStillCurrent(request);
    if (!speaker || !text || lines.length === 0) {
        await writeNativeResponse(root, filePath, request, {
            ok: true,
            request_id: request.request_id,
            npc_key: request.npc_key,
            npc_name: request.npc_name,
            player_text: message,
            text: '',
            speaker: null,
            silence: true,
            game_master: gameMaster || getNoGameMasterAction(),
        }, null, '1', ['-1']);
        await archiveNativeRequest(root, filePath, request);
        console.log('[NVBridge/native] Live Chat selected no NPC response.');
        return;
    }
    let audioInfo = earlyTtsComplete ? earlyAudioChunks[0] : null;
    let nextChunkIndex = earlyTtsComplete && earlyAudioChunks.length > 0
        ? Math.max(...earlyAudioChunks.map(chunk => chunk.index + 1))
        : 0;
    if (request.want_tts && !earlyTtsComplete) {
        for (const line of lines) {
            await assertNativeRequestStillCurrent(request);
            const lineRequest = {
                ...request,
                npc_key: line.speaker.nativeNpcKey || request.npc_key,
                npc_name: line.speaker.nativeNpcName || line.speaker.characterName || request.npc_name,
            };
            try {
                const audioChunks = await synthesizeNativeLineStreaming(
                    config,
                    root,
                    lineRequest,
                    line.text,
                    line.speaker.characterName || lineRequest.npc_name || config.characterName,
                    async (chunkInfo, index) => {
                        await writeNativeAudioChunk(root, lineRequest, chunkInfo.text || line.text, chunkInfo, index);
                    },
                    nextChunkIndex,
                );
                if (audioChunks.length === 0) {
                    throw new Error('Chasm speech stream returned no audio chunks.');
                }
                audioInfo = audioInfo || audioChunks[0];
                nextChunkIndex = Math.max(nextChunkIndex, ...audioChunks.map(chunk => chunk.index + 1));
            } catch (error) {
                if (isRequestSupersededError(error)) {
                    throw error;
                }
                console.warn(`[NVBridge/native] Streaming TTS unavailable, falling back to buffered synthesis: ${error.message}`);
                await assertNativeRequestStillCurrent(request);
                const fallbackInfo = await synthesizeNativeLine(
                    config,
                    root,
                    request.request_id,
                    line.text,
                    line.speaker.characterName || lineRequest.npc_name || config.characterName,
                    nextChunkIndex,
                    request,
                );
                await writeNativeAudioChunk(root, lineRequest, line.text, fallbackInfo, nextChunkIndex);
                audioInfo = audioInfo || fallbackInfo;
                nextChunkIndex += 1;
            }
        }
    }
    let queuedNativeAction = false;
    let responseGameMaster = gameMaster;
    for (const [index, line] of lines.entries()) {
        const lineGameMaster = line.gameMaster || getNoGameMasterAction();
        if (!lineGameMaster?.should_trigger) {
            continue;
        }
        const lineRequestId = lines.length > 1
            ? `${request.request_id}-${line.speaker.nativeNpcKey || line.speaker.characterId || index + 1}`
            : request.request_id;
        const queued = await writeNativeGameMasterCommand(
            config,
            { ...request, id: lineRequestId, request_id: lineRequestId },
            line.speaker,
            lineGameMaster,
            'fallout-new-vegas-native-action',
        );
        if (queued) {
            queuedNativeAction = true;
            responseGameMaster = responseGameMaster?.should_trigger ? responseGameMaster : lineGameMaster;
        }
    }
    await writeNativeResponse(root, filePath, request, {
        ok: true,
        request_id: request.request_id,
        npc_key: speaker.nativeNpcKey || request.npc_key,
        npc_name: speaker.nativeNpcName || speaker.characterName || request.npc_name,
        player_text: message,
        text,
        speaker,
        game_master: queuedNativeAction ? { ...responseGameMaster, should_trigger: false, queued_native_action: true } : responseGameMaster,
    }, audioInfo, '1', audioInfo ? ['0'] : ['-1']);
    await archiveNativeRequest(root, filePath, request);
    console.log(`[NVBridge/native] ${speaker.characterName || request.npc_name || config.characterName}: ${text.slice(0, 120)} -> ${audioInfo?.filename || '<no audio>'}`);
    await traceRequestStage(request, 'helper_process_request_done', {
        admin: false,
        text_length: text.length,
        lines: lines.length,
        chunks: nextChunkIndex,
        audio_file: audioInfo?.filename || '',
    });
}

async function readNativeRequestFile(filePath) {
    const text = await fs.readFile(filePath, 'utf8');
    return filePath.toLowerCase().endsWith('.json')
        ? JSON.parse(text)
        : parseNativeTextRequest(filePath, text);
}

async function isNativeRequestFileStillCurrent(filePath, request) {
    if (!request?.request_id) {
        return true;
    }
    if (!fsSync.existsSync(filePath)) {
        return false;
    }

    try {
        const current = await readNativeRequestFile(filePath);
        return current.request_id === request.request_id;
    } catch {
        return true;
    }
}

async function isNativeRequestStillCurrent(request) {
    const filePath = request?.__nativeRequestFilePath;
    return filePath ? await isNativeRequestFileStillCurrent(filePath, request) : true;
}

async function assertNativeRequestStillCurrent(request) {
    if (!(await isNativeRequestStillCurrent(request))) {
        throw createRequestSupersededError(request);
    }
}

async function pollNativeRoot(config, root) {
    await pollNativeSaveStateEvents(config, root);

    const directory = nativeInbox(root);
    const files = fsSync.existsSync(directory) ? await fs.readdir(directory) : [];
    for (const file of files.filter(item => item.toLowerCase().endsWith('.txt') || item.toLowerCase().endsWith('.json'))) {
        const filePath = path.join(directory, file);
        let request = null;
        try {
            request = await readNativeRequestFile(filePath);
            request.__nativeRequestFilePath = filePath;
            request.__nativeRoot = root;
            request.__traceRequests = config.traceRequests;
            request.__helperTraceStartedAt = performance.now();
            const stat = await fs.stat(filePath).catch(() => null);
            await traceRequestStage(request, 'helper_detected_request_file', {
                file,
                size_bytes: stat?.size ?? 0,
                file_age_ms: stat ? Date.now() - stat.mtimeMs : 0,
            });
            await processNativeRequest(config, root, filePath, request);
        } catch (error) {
            if (!request) {
                request = { request_id: path.basename(filePath, path.extname(filePath)), npc_key: '', npc_name: '', player_text: '' };
            }
            if (isRequestSupersededError(error)) {
                await archiveNativeRequest(root, filePath, request);
                console.warn(`[NVBridge/native] ${error.message}`);
                continue;
            }
            if (await isNativeRequestFileStillCurrent(filePath, request)) {
                await writeNativeResponse(root, filePath, request, {
                    ok: false,
                    request_id: request.request_id,
                    npc_key: request.npc_key,
                    npc_name: request.npc_name,
                    player_text: request.player_text,
                    text: '',
                    error: error.message,
                });
            } else {
                console.warn(`[NVBridge/native] Skipped stale error response for ${request.request_id}; live request file has advanced.`);
            }
            await archiveNativeRequest(root, filePath, request);
            console.error(`[NVBridge/native] ${error.message}`);
        }
    }
}

async function pollNativeSaveStateEvents(config, root) {
    const directory = nativeSaveStateEventDir(root);
    const files = fsSync.existsSync(directory) ? await fs.readdir(directory) : [];
    for (const file of files.filter(item => item.toLowerCase().endsWith('.txt'))) {
        const filePath = path.join(directory, file);
        let event = null;
        try {
            event = await readNativeSaveStateEventFile(filePath);
            await processNativeSaveStateEvent(config, root, filePath, event);
        } catch (error) {
            const eventId = event?.request_id || path.basename(filePath, path.extname(filePath));
            await writeNativeSaveStateAck(root, eventId, false, error.message);
            await archiveNativeSaveStateEvent(root, filePath, event || { request_id: eventId }, '.failed');
            console.error(`[NVBridge/save-sync] ${error.message}`);
        }
    }
}

async function archiveFile(root, filePath, suffix = '') {
    const name = `${Date.now()}-${path.basename(filePath)}${suffix}`;
    await fs.rename(filePath, path.join(archive(root), name)).catch(async () => {
        await fs.rm(filePath, { force: true });
    });
}

async function readTextRequest(root) {
    const pendingPath = path.join(outbox(root), 'pending.txt');
    if (!fsSync.existsSync(pendingPath)) {
        return null;
    }

    const message = (await fs.readFile(pendingPath, 'utf8')).trim();
    if (!message) {
        await fs.rm(pendingPath, { force: true });
        return null;
    }

    const distancePath = path.join(outbox(root), 'distance.txt');
    const targetPath = path.join(outbox(root), 'target.txt');
    const distance = fsSync.existsSync(distancePath) ? Number((await fs.readFile(distancePath, 'utf8')).trim()) : 0;
    const targetName = fsSync.existsSync(targetPath) ? (await fs.readFile(targetPath, 'utf8')).trim() : '';

    await archiveFile(root, pendingPath);
    return {
        id: `txt-${Date.now()}`,
        message,
        targetName,
        distanceGameUnits: Number.isFinite(distance) ? distance : 0,
    };
}

async function readAdminTextRequest(root) {
    const adminPath = path.join(outbox(root), 'todd.txt');
    if (!fsSync.existsSync(adminPath)) {
        return null;
    }

    const message = (await fs.readFile(adminPath, 'utf8')).trim();
    if (!message) {
        await fs.rm(adminPath, { force: true });
        return null;
    }

    await archiveFile(root, adminPath);
    return {
        id: `todd-${Date.now()}`,
        message,
        admin: true,
        source: 'fallout-new-vegas-admin',
    };
}

async function readJsonRequests(root) {
    const directory = outbox(root);
    const files = fsSync.existsSync(directory) ? await fs.readdir(directory) : [];
    const requests = [];
    for (const file of files.filter(item => item.toLowerCase().endsWith('.json'))) {
        const filePath = path.join(directory, file);
        try {
            requests.push({ filePath, request: JSON.parse(await fs.readFile(filePath, 'utf8')) });
        } catch (error) {
            await writeInbox(root, { ok: false, id: file, error: `Invalid request JSON: ${error.message}` });
            await archiveFile(root, filePath, '.invalid');
        }
    }
    return requests;
}

async function pollRoot(config, root) {
    const textRequest = await readTextRequest(root);
    if (textRequest) {
        try {
            await processRequest(config, root, textRequest);
        } catch (error) {
            await writeInbox(root, { ok: false, id: textRequest.id, error: error.message });
            console.error(`[NVBridge] ${error.message}`);
        }
    }

    const adminTextRequest = await readAdminTextRequest(root);
    if (adminTextRequest) {
        try {
            await processAdminRequest(config, root, adminTextRequest);
        } catch (error) {
            await writeInbox(root, { ok: false, id: adminTextRequest.id, error: error.message });
            console.error(`[NVBridge/admin] ${error.message}`);
        }
    }

    for (const item of await readJsonRequests(root)) {
        try {
            if (isAdminRequest(item.request)) {
                await processAdminRequest(config, root, {
                    ...item.request,
                    message: item.request.message || item.request.player_text || item.request.text || '',
                    admin: true,
                });
            } else {
                await processRequest(config, root, item.request);
            }
            await archiveFile(root, item.filePath);
        } catch (error) {
            await writeInbox(root, { ok: false, id: item.request?.id || path.basename(item.filePath), error: error.message });
            await archiveFile(root, item.filePath, '.failed');
            console.error(`[NVBridge] ${error.message}`);
        }
    }
}

async function main() {
    const args = parseArgs(process.argv);
    const config = await loadConfig(args);
    const roots = config.dataRoots.filter(root => root && fsSync.existsSync(path.dirname(root)));
    const nativeRoots = config.nativeBridgeRoots.filter(root => root && fsSync.existsSync(path.dirname(root)));
    if (!roots.length && !nativeRoots.length) {
        throw new Error('No usable NVBridge data roots found.');
    }

    for (const root of roots) {
        await ensureRoot(root);
    }
    for (const root of nativeRoots) {
        await ensureNativeRoot(root);
    }
    const lockPaths = [];
    for (const root of nativeRoots) {
        lockPaths.push(await acquireHelperLock(root));
    }
    process.once('exit', () => releaseHelperLocks(lockPaths));
    process.once('SIGINT', () => {
        releaseHelperLocks(lockPaths);
        process.exit(130);
    });
    process.once('SIGTERM', () => {
        releaseHelperLocks(lockPaths);
        process.exit(143);
    });

    console.log(`[NVBridge] API: ${config.apiBase}`);
    console.log(`[NVBridge] Model: ${config.model || 'SillyTavern API Connections'}`);
    console.log(`[NVBridge] Watching ${roots.length} data root(s).`);
    console.log(`[NVBridge/native] Watching ${nativeRoots.length} bridge root(s).`);
    console.log('[NVBridge] Waiting for in-game requests. Press Ctrl+C to stop.');

    let polling = false;
    async function runPollCycle() {
        if (polling) {
            return;
        }
        polling = true;
        try {
            await Promise.all([
                ...roots.map(root => pollRoot(config, root)),
                ...nativeRoots.map(root => pollNativeRoot(config, root)),
            ]);
        } finally {
            polling = false;
        }
    }

    await runPollCycle();
    setInterval(() => {
        runPollCycle().catch(error => {
            console.error(`[NVBridge] ${error.message}`);
        });
    }, Math.max(75, config.pollMs));
}

export {
    buildNativeGamestate,
    buildNativeActionCommandLines,
    buildNativeActionCommands,
    buildSaveSyncEventBody,
    normalizeNpcCandidate,
    parseNativeSaveStateEventText,
    resolveNativeActorForAdmin,
    resolveTrustedExecutionArgument,
    resolveTrustedExecutionArguments,
};

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
    main().catch(error => {
        console.error(`[NVBridge] ${error.message}`);
        process.exitCode = 1;
    });
}
