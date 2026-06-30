#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <windows.h>
#include <dsound.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#include "common/ITypes.h"
#include "nvse/containers.h"
#include "nvse/GameTypes.h"
#include "nvse/nvse_version.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameData.h"
#include "nvse/GameExtraData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/GameOSDepend.h"
#include "nvse/GameUI.h"

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
NVSEInterface* g_nvse = nullptr;
NVSEMessagingInterface* g_messaging = nullptr;
NVSEScriptInterface* g_scriptInterface = nullptr;
NVSESerializationInterface* g_serialization = nullptr;
extern const _FormHeap_Free FormHeap_Free;

namespace
{
namespace fs = std::filesystem;

constexpr char kPluginName[] = "FNVBridgeNative";
constexpr UInt32 kPluginVersion = 1;
constexpr UInt32 kMinimumNvseVersion = MAKE_NEW_VEGAS_VERSION(6, 4, 5);
constexpr char kRequestId[] = "req_live";
constexpr char kInputCallbackRelativePath[] = "fnv_bridge/input_callback.txt";
constexpr int kTextInputWidth = 700;
constexpr int kTextInputHeight = 220;
constexpr int kTextInputMaxLength = 280;
constexpr int kControlMargin = 14;
constexpr float kDefaultSubtitleSeconds = 3.5f;
constexpr float kMaxSubtitleSeconds = 9.0f;
constexpr double kMaxAudibleDistance = 2400.0;
constexpr float kGameUnitsPerMeter = 70.0f;
constexpr float kVoiceMinDistanceMeters = 96.0f / kGameUnitsPerMeter;
constexpr float kVoiceMaxDistanceMeters = static_cast<float>(kMaxAudibleDistance / kGameUnitsPerMeter);
constexpr float kChatNpcSearchRadiusMeters = 10.0f;
constexpr float kGroupChatNearbyRadiusMeters = 10.0f;
constexpr float kGamestateNearbyRadiusMeters = 30.0f;
constexpr SHORT kChatVirtualKey = VK_RETURN;
constexpr SHORT kVoiceChatVirtualKey = VK_MENU;
constexpr SHORT kAdminChatVirtualKey = 'O';
constexpr SHORT kAdminVoiceChatVirtualKey = 'H';
constexpr char kAdminNpcKey[] = "todd";
constexpr char kAdminNpcName[] = "Todd";
constexpr char kBridgeDialoguePluginName[] = "NVBridge.esp";
constexpr char kBridgeDialogueQuestEditorId[] = "NVBridgeQuest";
constexpr char kBridgeConversationPackageEditorId[] = "NVBConversationPackage";
constexpr UInt32 kBridgeDialogueTopicLocalFormId = 0x00001000;
constexpr UInt32 kBridgeConversationPackageLocalFormId = 0x00002002;
constexpr UInt32 kFaceGenPhonemeCount = 16;
constexpr DWORD kSpeechEnvelopeWindowMs = 40;
constexpr DWORD kSpeechAnimationUpdateIntervalMs = 50;
constexpr DWORD kSpeechBindingValidationIntervalMs = 500;
constexpr DWORD kSpeechAnimationTailMs = 120;
constexpr DWORD kSpeechBindingRetryMs = 500;
constexpr DWORD kConversationReleaseDelayMs = 5000;
constexpr DWORD kConversationFaceUpdateIntervalMs = 900;
constexpr DWORD kConversationModeFaceRefreshIntervalMs = 1000;
constexpr DWORD kConversationLookRefreshIntervalMs = 1500;
constexpr DWORD kConversationPackageRefreshIntervalMs = 5000;
constexpr DWORD kTextInputEmptySubmitGraceMs = 250;
constexpr DWORD kTextInputInvisibleRecoveryMs = 1000;
constexpr DWORD kModLocalFormRetryMs = 30000;
constexpr DWORD kDirectSoundIdleReleaseDelayMs = 5000;
constexpr DWORD kVoicePlaybackSampleRate = 44100;
constexpr WORD kVoicePlaybackChannels = 1;
constexpr WORD kVoicePlaybackBitsPerSample = 16;
constexpr WORD kWaveFormatPcm = 0x0001;
constexpr WORD kWaveFormatIeeeFloat = 0x0003;
constexpr WORD kWaveFormatExtensible = 0xFFFE;
constexpr double kVoicePlaybackHeadroom = 0.84;
constexpr double kVoicePlaybackFadeMs = 3.0;
constexpr float kSpeechSilenceThreshold = 0.035f;
constexpr float kSpeechMaxWeight = 0.95f;
constexpr float kSpeechMinWeight = 0.18f;
constexpr float kConversationFaceTurnThresholdDegrees = 20.0f;
constexpr float kConversationModeReleaseDistanceMeters = 10.0f;
constexpr DWORD kVoiceCaptureSampleRate = 16000;
constexpr WORD kVoiceCaptureBitsPerSample = 16;
constexpr WORD kVoiceCaptureChannels = 1;
constexpr DWORD kVoiceCaptureBufferMs = 200;
constexpr size_t kVoiceCaptureBufferCount = 4;
constexpr DWORD kVoiceCaptureMinimumMs = 180;
constexpr DWORD kDebugConfigPollMs = 1000;
constexpr DWORD kRuntimeHeartbeatIntervalMs = 100;
constexpr DWORD kStreamedSpeechEndPaddingMs = 40;
constexpr DWORD kDefaultStreamingChunkOverlapMs = 40;

// Phase 3 single-buffer streaming: capacity (seconds) of the one DirectSound buffer
// that holds a whole NPC utterance, written incrementally as mini-chunks arrive and
// played continuously. 30s covers any line plus opener/remainder gaps; a new
// utterance (new request id) starts a fresh buffer.
constexpr DWORD kStreamingVoiceMaxSeconds = 30;
// Lead (ms of audio) to keep the write cursor ahead of the play cursor when
// recovering from an underrun (e.g. the opener->remainder gap), so freshly written
// audio is never placed behind the play cursor (which would skip it).
constexpr DWORD kStreamingVoiceLeadMs = 60;
// Grace after the last append before declaring a played-out utterance done (guards
// against stopping during a brief inter-chunk gap).
constexpr DWORD kStreamingVoiceEndGraceMs = 250;
// Caption chunking (DISPLAY ONLY): default ms of speech per character, used to time
// the reveal of each on-screen caption segment against playback (~75ms/char ≈ 13
// cps). Tunable via native_debug.cfg `caption_ms_per_char`. Never affects audio.
constexpr DWORD kDefaultCaptionMsPerChar = 75;
// Coalesce the 200ms streaming mini-chunks into lip-sync windows of this size, so the
// face animation is (re)started ~once per window instead of ~5x/sec per mini-chunk
// (which restarted the FaceGen animation constantly -> lag, jank, and crashes). ~one
// StartSpeechAnimation per window, close to the old per-sentence cadence.
constexpr DWORD kLipSyncWindowMs = 800;
constexpr DWORD kMaxStreamingChunkOverlapMs = 250;
constexpr unsigned long long kRuntimeHeartbeatHistoryMaxBytes = 8ull * 1024ull * 1024ull;
constexpr DWORD kStackBootstrapCooldownMs = 15000;
constexpr DWORD kSaveStateSyncTimeoutMs = 8000;
constexpr DWORD kSaveStateAckPollMs = 100;
constexpr DWORD kSaveStateSyncHudCooldownMs = 1500;
constexpr char kDefaultFollowPackageEditorId[] = "DefaultFollowPlayerFar";
constexpr char kNativeActionCommandVersion2[] = "NVBRIDGE_ACTION_V2";
constexpr char kTrustedFNVActionEngine[] = "fallout-new-vegas:xnvse";
constexpr size_t kMaxTrustedExecutionScriptBytes = 256ull * 1024ull;

struct ResponsePayload
{
    int statusCode = 0;
    bool ok = false;
    bool isFinal = true;
    std::string requestId;
    std::string npcKey;
    std::string npcName;
    std::string playerText;
    std::string audioFile;
    std::string text;
    std::string error;
    int audioChunkIndex = -1;
    std::string actionNpcKey;
    std::string actionNpcName;
    bool nonPositionalAudio = false;
    std::string gameMasterAction;
    std::string actionId;
    std::string actionBookId;
    std::string executionEngine;
    std::string executionTemplateId;
    std::string executionLanguage;
    std::string executionScript;
    std::vector<std::string> executionArguments;
    double gameMasterConfidence = 0.0;
    bool gameMasterShouldTrigger = false;
};

enum class TrustedExecutionArgumentType
{
    Ref,
    Form,
    Number,
    String
};

struct TrustedExecutionArgument
{
    TrustedExecutionArgumentType type = TrustedExecutionArgumentType::Ref;
    TESObjectREFR* ref = nullptr;
    TESForm* form = nullptr;
    double number = 0.0;
    std::string text;
};

struct SpeakerSnapshot
{
    UInt32 refId = 0;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    bool valid = false;
};

struct LocationSnapshot
{
    std::string major;
    std::string minor;
    std::string cell;
    std::string worldspace;
    std::string region;
};

struct ActiveSound
{
    IDirectSoundBuffer8* buffer = nullptr;
    IDirectSound3DBuffer* buffer3d = nullptr;
    SpeakerSnapshot speaker;
};

struct QueuedAudioChunk
{
    std::string requestId;
    fs::path wavPath;
    std::string audioFile;
    std::string speakerKey;
    std::string speakerName;
    std::string subtitleText;
    std::string publishedAtIso;
    int chunkIndex = -1;
    bool nonPositional = false;
    int captionMaxChars = -1; // display-only caption split hint; -1 = unset (whole line)
};

// Phase 3: a chunk awaiting its lip-sync trigger. Audio is written into the single
// streaming buffer ahead of playback, but `StartSpeechAnimation` is wall-clock /
// amplitude-envelope driven and must fire when the chunk's audio actually plays, so
// we hold each chunk's PCM + metadata and fire it at `startMs` into playback.
struct PendingStreamLipSync
{
    DWORD startMs = 0;     // playback offset (ms from stream start) when this plays
    DWORD durationMs = 0;
    std::vector<BYTE> pcm; // chunk PCM (for the amplitude envelope)
    WAVEFORMATEX format{};
    std::string requestId;
    std::string audioFile;
    std::string subtitleText;
    SpeakerSnapshot speaker;
};

struct ResolvedNpcTarget
{
    TESObjectREFR* ref = nullptr;
    std::string npcKey;
    std::string npcName;
    std::string voiceTypeKey;
    std::string voiceTypeName;
    double distanceSquared = DBL_MAX;
    bool underCrosshair = false;
};

struct NearbyNpcCandidate
{
    TESObjectREFR* ref = nullptr;
    std::string npcKey;
    std::string npcName;
    std::string voiceTypeKey;
    std::string voiceTypeName;
    double distanceSquared = DBL_MAX;
    bool underCrosshair = false;
};

struct FaceGenKeyframeMultiple32
{
    void* vtbl = nullptr;
    UInt32 type = 0;
    float unk0C = 0.0f;
    float* values = nullptr;
    UInt32 count = 0;
    UInt8 isUpdated = 0;
    UInt8 pad1D = 0;
    UInt16 pad1E = 0;
};
static_assert(sizeof(FaceGenKeyframeMultiple32) == 0x18);

struct FaceGenNiNodeRuntime
{
    BYTE pad000[0x0DC];
    void* spAnimationData = nullptr;
    BYTE pad0E0[0x026];
    UInt8 bAnimationUpdate = 0;
    UInt8 bRotatedLastUpdate = 0;
    UInt8 bApplyRotationToParent = 0;
    BYTE pad109[3]{};
    float fLastTime = -1.0f;
    UInt8 bUsingLoResHead = 0;
    UInt8 bIAmPlayerCharacter = 0;
    UInt8 bIAmInDialouge = 0;
    UInt8 pad113 = 0;
    void* pActor = nullptr;
};
static_assert(offsetof(FaceGenNiNodeRuntime, spAnimationData) == 0x0DC);
static_assert(offsetof(FaceGenNiNodeRuntime, bAnimationUpdate) == 0x106);
static_assert(offsetof(FaceGenNiNodeRuntime, bIAmInDialouge) == 0x112);

struct FaceAnimationBinding
{
    UInt32 speakerRefId = 0;
    FaceGenNiNodeRuntime* faceNode = nullptr;
    void* animationData = nullptr;
    FaceGenKeyframeMultiple32* phonemeKeyframe = nullptr;
    FaceGenKeyframeMultiple32* modifierKeyframe = nullptr;
};

struct VisemeCue
{
    DWORD startMs = 0;
    DWORD endMs = 0;
    UInt32 phonemeId = 0;
    float emphasis = 1.0f;
};

struct ActiveSpeechAnimation
{
    bool active = false;
    std::string requestId;
    SpeakerSnapshot speaker;
    DWORD startedTick = 0;
    DWORD durationMs = 0;
    DWORD envelopeWindowMs = kSpeechEnvelopeWindowMs;
    DWORD lastBindingAttemptTick = 0;
    DWORD lastBindingValidationTick = 0;
    DWORD lastWeightsUpdateTick = 0;
    bool loggedBindingFailure = false;
    bool firstWeightsAppliedLogged = false;
    FaceAnimationBinding binding;
    std::vector<float> envelope;
    std::vector<VisemeCue> visemes;
    std::array<float, kFaceGenPhonemeCount> lastWeights{};
    std::array<float, kFaceGenPhonemeCount> originalWeights{};
    UInt32 originalPhonemeCount = 0;
    UInt8 originalPhonemeIsUpdated = 0;
    UInt8 originalFaceAnimationUpdate = 0;
    UInt8 originalFaceInDialogue = 0;
    bool originalBindingStateCaptured = false;
};

struct ConversationHoldState
{
    bool active = false;
    bool scriptPackageApplied = false;
    bool conversationIssued = false;
    bool preserveFurnitureState = false;
    bool lookApplied = false;
    bool conversationModeApplied = false;
    bool originalRestrainedKnown = false;
    bool originalRestrained = false;
    bool restrainedApplied = false;
    bool noMovePackageApplied = false;
    std::string npcKey;
    std::string npcName;
    SpeakerSnapshot speaker;
    DWORD releaseTick = 0;
    DWORD lastFaceUpdateTick = 0;
    DWORD lastBodyFaceUpdateTick = 0;
    DWORD lastPackageCheckTick = 0;
    float lastAppliedFacingDegrees = FLT_MAX;
};

struct ModLocalFormCacheEntry
{
    TESForm* form = nullptr;
    UInt32 runtimeFormId = 0;
    UInt8 modIndex = 0xFF;
    DWORD nextRetryTick = 0;
    bool modMissingLogged = false;
    bool formMissingLogged = false;
};

struct VoiceCaptureBuffer
{
    WAVEHDR header{};
    std::vector<BYTE> storage;
};

struct VoiceCaptureState
{
    bool keyDownLastFrame = false;
    bool adminKeyDownLastFrame = false;
    bool active = false;
    bool transcribing = false;
    bool adminMode = false;
    std::string npcKey;
    std::string npcName;
    SpeakerSnapshot speaker;
    HWAVEIN waveIn = nullptr;
    DWORD startedTick = 0;
    DWORD subtitleRefreshTick = 0;
    std::vector<BYTE> capturedPcm;
    std::vector<VoiceCaptureBuffer> buffers;
};

struct RuntimeState
{
    bool keyDownLastFrame = false;
    bool adminKeyDownLastFrame = false;
    bool awaitingInput = false;
    bool bridgeTextInputOwned = false;
    bool awaitingReply = false;
    bool awaitingVoiceReply = false;
    bool inputMenuSeenVisible = false;
    bool inputEnterDownLastFrame = false;
    bool inputEscapeDownLastFrame = false;
    DWORD inputEmptyEnterCancelTick = 0;
    DWORD inputStartedTick = 0;
    DWORD staleTextInputCloseRetryTick = 0;
    bool gameWindowFocusedLastFrame = false;
    DWORD ignoreHotkeysUntilTick = 0;
    bool loadedIntoGame = false;
    std::string pendingNpcKey;
    std::string pendingNpcName;
    SpeakerSnapshot pendingSpeaker;
    IDirectSound8* directSound = nullptr;
    IDirectSoundBuffer* primaryBuffer = nullptr;
    IDirectSound3DListener* listener3d = nullptr;
    DWORD directSoundIdleSinceTick = 0;
    DWORD replyStartedTick = 0;
    DWORD lastBridgeActivityTick = 0;
    bool sawBridgeActivity = false;
    std::string activeRequestId;
    int lastAudioChunkIndex = -1;
    bool subtitleShownForReply = false;
    bool dialogSubtitleActive = false;
    DWORD dialogSubtitleHideTick = 0;
    std::string replySubtitleText;
    std::string lastNpcKey;
    std::string lastNpcName;
    SpeakerSnapshot lastNpcSpeaker;
    std::unordered_map<std::string, SpeakerSnapshot> npcSpeakersByKey;
    DWORD activeSpeechUntilTick = 0;
    bool streamedAudioSeenForReply = false;
    std::string traceRequestId;
    ULONGLONG traceStartedTick = 0;
    ActiveSpeechAnimation speechAnimation;
    std::vector<ActiveSound> activeSounds;
    std::deque<QueuedAudioChunk> pendingAudioChunks;
    // --- Phase 3 single-buffer streaming voice (gated on DebugConfig.singleBufferStreaming) ---
    IDirectSoundBuffer8* streamBuffer = nullptr;
    IDirectSound3DBuffer* streamBuffer3d = nullptr;
    bool streamActive = false;       // a streaming buffer exists for the current utterance
    bool streamStarted = false;      // Play() has been called
    DWORD streamCapacityBytes = 0;   // buffer size
    DWORD streamWriteCursor = 0;     // next byte offset to write real audio at
    DWORD streamPlayStartTick = 0;   // tick when Play() began (lip-sync scheduling)
    DWORD streamLastAppendTick = 0;  // tick of the last chunk write (end grace)
    DWORD streamCumulativeMs = 0;    // total audio ms appended (next chunk's startMs)
    WAVEFORMATEX streamFormat{};     // buffer format (from the first chunk)
    std::string streamRequestId;     // utterance this buffer serves
    SpeakerSnapshot streamSpeaker;
    bool streamNonPositional = false;          // 2D (player-centered) vs 3D positional
    std::deque<PendingStreamLipSync> streamLipSyncQueue;
    // Phase 3 caption chunking (display only): the NPC's full line split into
    // <= captionMaxChars segments, revealed in sync with the streaming playback.
    int captionMaxChars = -1;                  // -1 unset; 0 = whole line in one caption
    std::vector<std::string> captionSegments;
    std::vector<DWORD> captionSegmentStartChar; // char offset of each segment (for play-cursor sync)
    int captionCurrentIndex = -1;              // segment currently on screen
    std::string captionSourceText;             // the full line the segments came from
    DWORD captionTotalMsLocked = 0;            // total audio ms locked once synthesis settles (stable basis)
    DWORD captionLastShowTick = 0;             // last (re)show of the current caption (refresh timer)
    // Phase 3 lip-sync coalescing: accumulate mini-chunks into ~kLipSyncWindowMs
    // windows so the face animation isn't restarted per 200ms chunk.
    std::vector<BYTE> lipSyncAccumPcm;
    DWORD lipSyncAccumMs = 0;
    DWORD lipSyncAccumStartMs = 0;
    SpeakerSnapshot lipSyncAccumSpeaker;
    std::unordered_set<std::string> movementActionRequestIds;
    ConversationHoldState conversationHold;
    std::string lastPlaybackDiagnostics;
    DWORD lastDebugConfigPollTick = 0;
    DWORD lastRuntimeHeartbeatTick = 0;
    ULONGLONG runtimeHeartbeatFrame = 0;
    bool voiceBootstrapSubtitleActive = false;
    DWORD voiceBootstrapStatusPollTick = 0;
    DWORD voiceBootstrapSubtitleRefreshTick = 0;
    std::string voiceBootstrapMessage;
    bool saveStateSyncPending = false;
    std::string saveStateSyncEventId;
    std::string saveStateSyncType;
    std::string pendingLoadSavePath;
    DWORD saveStateSyncLastPollTick = 0;
    DWORD saveStateSyncHudMessageTick = 0;
    DWORD saveStateSyncStartedTick = 0;
    DWORD stackBootstrapAttemptTick = 0;
    bool stackBootstrapAttempted = false;
    VoiceCaptureState voiceCapture;
};

struct DebugConfig
{
    bool runtimeHeartbeatEnabled = true;
    bool speechAnimationEnabled = true;
    bool speechWritePhonemeValues = true;
    bool speechWriteFaceFlags = true;
    bool speechClearBindingOnStop = true;
    bool subtitlesEnabled = true;
    bool listenerUpdatesEnabled = true;
    bool directSound3dEnabled = true;
    bool directSoundSoftwareBufferEnabled = true;
    bool drainQueuedChunksAfterFinal = true;
    // Phase 3: play streamed TTS through ONE continuous DirectSound buffer per
    // utterance (seamless, no per-chunk buffer churn) instead of the per-chunk
    // static-buffer queue. Toggle off to fall back to the static path for A/B.
    bool singleBufferStreaming = true;
    bool requestTracingEnabled = false;
    bool conversationModeEnabled = true;
    bool autoStartStack = true;
    float speechWeightScale = 1.0f;
    float conversationModeReleaseDistanceMeters = kConversationModeReleaseDistanceMeters;
    DWORD speechAnimationUpdateIntervalMs = kSpeechAnimationUpdateIntervalMs;
    DWORD speechBindingValidationIntervalMs = kSpeechBindingValidationIntervalMs;
    DWORD conversationModeFaceRefreshIntervalMs = kConversationModeFaceRefreshIntervalMs;
    DWORD conversationLookRefreshIntervalMs = kConversationLookRefreshIntervalMs;
    DWORD runtimeHeartbeatIntervalMs = kRuntimeHeartbeatIntervalMs;
    DWORD streamingChunkOverlapMs = kDefaultStreamingChunkOverlapMs;
    DWORD captionMsPerChar = kDefaultCaptionMsPerChar;
    DWORD stackBootstrapCooldownMs = kStackBootstrapCooldownMs;
    std::string stackLauncherPath;
    std::string bridgeRootPath;
};

RuntimeState g_state;
Script* g_openTextInputScript = nullptr;
Script* g_closeTextInputScript = nullptr;
Script* g_startCombatScript = nullptr;
Script* g_setPlayerTeammateScript = nullptr;
Script* g_startConversationScript = nullptr;
Script* g_startLookScript = nullptr;
Script* g_stopLookScript = nullptr;
Script* g_evaluatePackageScript = nullptr;
Script* g_isCurrentPackageScript = nullptr;
Script* g_addScriptPackageScript = nullptr;
Script* g_removeScriptPackageScript = nullptr;
Script* g_getRestrainedScript = nullptr;
Script* g_setRestrainedScript = nullptr;
Script* g_clearRestrainedScript = nullptr;
Script* g_setAngleScript = nullptr;
Script* g_faceObjectScript = nullptr;
bool g_faceObjectScriptAttempted = false;
Script* g_applyNoMovePackageScript = nullptr;
bool g_applyNoMovePackageScriptAttempted = false;
Script* g_runBatchScript = nullptr;
Script* g_consoleCommandScript = nullptr;
std::unordered_map<std::string, Script*> g_trustedExecutionScripts;
std::unordered_map<std::string, ModLocalFormCacheEntry> g_modLocalFormCache;
DebugConfig g_debugConfig;
fs::file_time_type g_debugConfigWriteTime{};
bool g_debugConfigLoaded = false;

constexpr UInt32 kInterfaceManagerSingletonAddress = 0x011D8A80;
constexpr UInt32 kPlayerSingletonAddress = 0x011DEA3C;
constexpr UInt32 kOSGlobalsAddress = 0x011DEA0C;
constexpr UInt32 kDataHandlerSingletonAddress = 0x011C3F2C;
constexpr UInt32 kFormsMapAddress = 0x011C54C0;
constexpr UInt32 kQueueUIMessageAddress = 0x007052F0;
constexpr UInt32 kMenuVisibilityArrayAddress = 0x011F308F;
constexpr UInt32 kTileMenuArrayAddress = 0x011F3508;
constexpr UInt32 kCreateFormInstanceAddress = 0x00465110;
constexpr UInt32 kTempMenuByTypeAddress = 0x00707990;
constexpr UInt32 kHudUpdateVisibilityStateAddress = 0x00771700;
constexpr UInt32 kTraitNameToIdAddress = 0x00A01860;

using QueueUiMessageFn = bool (*)(const char*, UInt32, const char*, const char*, float, bool);
QueueUiMessageFn g_queueUiMessage = reinterpret_cast<QueueUiMessageFn>(kQueueUIMessageAddress);
using CreateFormInstanceFn = TESForm* (*)(UInt8);
CreateFormInstanceFn g_createFormInstance = reinterpret_cast<CreateFormInstanceFn>(kCreateFormInstanceAddress);
using TempMenuByTypeFn = Menu * (*)(UInt32);
TempMenuByTypeFn g_tempMenuByType = reinterpret_cast<TempMenuByTypeFn>(kTempMenuByTypeAddress);
using HudUpdateVisibilityStateFn = void (*)(signed int);
HudUpdateVisibilityStateFn g_hudUpdateVisibilityState = reinterpret_cast<HudUpdateVisibilityStateFn>(kHudUpdateVisibilityStateAddress);
using TraitNameToIdFn = UInt32 (*)(const char*);
TraitNameToIdFn g_traitNameToId = reinterpret_cast<TraitNameToIdFn>(kTraitNameToIdAddress);

fs::path BridgeDir();
fs::path VoiceBootstrapStatusPath();
fs::path SaveStateControlDir();
fs::path SaveStateEventsDir();
fs::path SaveStateAcksDir();
fs::path NativeActionCommandDir();
fs::path SaveStateEventPath(const std::string& eventId);
fs::path SaveStateAckPath(const std::string& eventId);
void EnsureBridgeDirectories();
void MaybeRequestBridgeStackStartup(const char* reason, bool force = false);
std::string GenerateSaveStateEventId();
bool DispatchSaveStateEvent(const std::string& eventType, const std::string& savePath, bool waitForAck);
void PollSaveStateSyncAck();
std::string Trim(std::string value);
std::string ToLowerAscii(std::string value);
void ShowGeneralSubtitle(const std::string& speaker, const std::string& text, float seconds);
void ShowHudMessage(const std::string& message);
void ClearDialogSubtitle();
bool ShowDialogSubtitle(const std::string& speaker, const std::string& text, float seconds);
std::string ToUiAscii(std::string_view value);
void InterruptBridgeReplyAndPlayback(const char* reason);
bool HasPendingChunkFiles();
void ClearOutboxArtifacts(const char* reason);
bool HasQueuedOrPlayingReply();
void ClearIdleOutboxArtifacts(const char* reason);
PlayerCharacter* GetPlayer();
TESObjectREFR* ResolveSpeakerRef(const SpeakerSnapshot& speaker);
void RememberNpcTarget(const std::string& npcKey, const std::string& npcName, const SpeakerSnapshot& speaker);
std::optional<SpeakerSnapshot> ResolveSpeakerSnapshotForNpc(const std::string& npcKey, const std::string& npcName);
SpeakerSnapshot CaptureSpeakerSnapshot(TESObjectREFR* ref);
LocationSnapshot CapturePlayerLocation();
bool WriteRequest(const std::string& npcKey, const std::string& npcName, const std::string& text, const LocationSnapshot& location, const std::string& metadataJson = "", bool clearSpeechSidecar = true);
bool WriteVoiceRequest(const std::string& npcKey, const std::string& npcName, const SpeakerSnapshot& speaker, const std::vector<BYTE>& wavBytes, const LocationSnapshot& location, bool adminMode = false);
DataHandler* GetDataHandler();
std::string FormIdHex(UInt32 formId);
std::optional<UInt32> ParseTrustedFormId(const std::string& rawValue);
std::string DescribeScriptResult(const NVSEArrayVarInterface::Element& result);
std::vector<std::string> BuildRunBatchScriptPathCandidates(const fs::path& scriptPath);
bool ExecuteConsoleCommand(TESObjectREFR* callingRef, const std::string& command);
bool IsActorUsingPackage(TESObjectREFR* actorRef, TESForm* packageForm, bool* outKnown = nullptr);
bool IsActorUsingBridgeConversationPackage(TESObjectREFR* actorRef, bool* outKnown = nullptr);
bool EvaluateActorPackage(TESObjectREFR* actorRef);
bool AddActorScriptPackage(TESObjectREFR* actorRef, TESForm* packageForm, const char* packageEditorId, const char* traceStage);
bool RemoveActorScriptPackage(TESObjectREFR* actorRef, const char* traceStage);
bool SetActorPlayerTeammate(TESObjectREFR* actorRef, bool enabled = true, const char* traceStage = "game_master_follow_teammate");
TESForm* ResolveDefaultFollowPackage();
double DistanceSquared3D(const TESObjectREFR* left, const TESObjectREFR* right);
UInt32 MakeWorldCellKey(SInt32 x, SInt32 y);
bool ShouldPreserveActorConversationAnimation(TESObjectREFR* speakerRef);
void StopSpeechAnimation();
void UpdateSpeechAnimation();
// Phase 3 single-buffer streaming voice (defined later; declared here for the
// reply-teardown sites that run before the definitions).
void StopStreamingVoice(const char* reason);
void UpdateStreamingVoice();
void DrainChunksToStreamingVoice();
void ShutdownDirectSound();
void UpdateConversationHold();
void ReleaseConversationHold(const char* reason);
void UpdateVoiceBootstrapStatus();
void UpdateVoiceCaptureHotkey();
void PollVoiceCaptureBuffers();
void PollNativeActionCommands();
void AbortVoiceCapture(const char* reason, bool releaseHold = true);
void FinishVoiceCaptureAndSubmit();
bool GameWindowHasFocus();
void ResetTextInputKeyWatcher();
void ClearTextInputKeyWatcher();
void LoadDebugConfigIfNeeded(bool force = false);
void WriteRuntimeHeartbeatIfNeeded(bool force = false);
void TraceRequestEvent(const std::string& requestId, const std::string& stage,
    const std::vector<std::pair<std::string, std::string>>& stringFields = {},
    const std::vector<std::pair<std::string, double>>& numberFields = {},
    const std::vector<std::pair<std::string, bool>>& boolFields = {});
bool IsLiveNearbyActorRef(const TESObjectREFR* anchorRef, TESObjectREFR* ref);
void CollectNearbyMappedNpcAround(const TESObjectREFR* anchorRef, TESObjectREFR* ref, double maxDistanceSquared, bool underCrosshair, std::vector<NearbyNpcCandidate>& candidates);
std::pair<std::string, std::string> ResolveRefVoiceTypeMetadata(TESObjectREFR* ref);

TESForm* LookupFormByIdRuntime(UInt32 refId)
{
    auto* formsMap = *reinterpret_cast<NiTPointerMap<TESForm>**>(kFormsMapAddress);
    return formsMap ? formsMap->Lookup(refId) : nullptr;
}

void LogLine(const char* fmt, ...)
{
    char buffer[2048]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    if (!g_nvse)
    {
        return;
    }

    EnsureBridgeDirectories();
    std::ofstream out(BridgeDir() / "native_plugin.log", std::ios::binary | std::ios::app);
    out << buffer << "\r\n";
}

fs::path RuntimeDir()
{
    return fs::path(g_nvse->GetRuntimeDirectory());
}

fs::path DataDir()
{
    return RuntimeDir() / "Data";
}

fs::path DefaultBridgeDir()
{
    return DataDir() / "NVBridge";
}

fs::path ResolveConfiguredRuntimePath(const std::string& rawPath)
{
    if (rawPath.empty())
    {
        return {};
    }

    fs::path resolved(rawPath);
    if (resolved.is_relative())
    {
        resolved = RuntimeDir() / resolved;
    }

    return resolved.lexically_normal();
}

fs::path BridgeDir()
{
    if (!g_debugConfig.bridgeRootPath.empty())
    {
        const fs::path configured = ResolveConfiguredRuntimePath(g_debugConfig.bridgeRootPath);
        if (!configured.empty())
        {
            return configured;
        }
    }

    return DefaultBridgeDir();
}

fs::path BridgeStorageRootDir()
{
    const fs::path bridgeDir = BridgeDir();
    const fs::path parent = bridgeDir.parent_path();
    if (!parent.empty())
    {
        return parent;
    }

    return DataDir();
}

fs::path SaveStateControlDir()
{
    return BridgeDir() / "control";
}

fs::path SaveStateEventsDir()
{
    return SaveStateControlDir() / "events";
}

fs::path SaveStateAcksDir()
{
    return SaveStateControlDir() / "acks";
}

fs::path NativeActionCommandDir()
{
    return SaveStateControlDir() / "actions";
}

std::string SafeEventStem(std::string value)
{
    for (char& ch : value)
    {
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
        {
            ch = '_';
        }
    }
    return value;
}

fs::path SaveStateEventPath(const std::string& eventId)
{
    return SaveStateEventsDir() / (SafeEventStem(eventId) + ".txt");
}

fs::path SaveStateAckPath(const std::string& eventId)
{
    return SaveStateAcksDir() / (SafeEventStem(eventId) + ".txt");
}

fs::path InboxPath()
{
    return BridgeDir() / "inbox" / "req_live.txt";
}

fs::path SttInboxAudioPath()
{
    return BridgeDir() / "inbox" / "req_live.stt.wav";
}

fs::path UiSubmitPath()
{
    return BridgeDir() / "ui_submit.txt";
}

fs::path OutboxPath()
{
    return BridgeDir() / "outbox" / "req_live.txt";
}

// A plain folder OUTSIDE MO2's virtual filesystem (usvfs). Files under the game
// Data / mods / overwrite tree are virtualized, so every file open from INSIDE the
// game costs ~tens of ms (the streamed-chunk in-game lag — the helper, which runs
// outside MO2, writes each file in ~2ms but the plugin reads it in ~80ms). The
// streamed chunk commands + audio live here for native-speed reads; the helper
// writes the identical path (%LOCALAPPDATA%\NVBridgeStream).
fs::path StreamStorageDir()
{
    char buf[MAX_PATH] = { 0 };
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        return fs::path(buf) / "NVBridgeStream";
    }
    return DefaultBridgeDir() / "stream";
}

fs::path OutboxChunkDir()
{
    return StreamStorageDir() / "chunks";
}

fs::path UserFunctionDir()
{
    return DataDir() / "NVSE" / "user_defined_functions" / "fnv_bridge";
}

fs::path ScriptRunnerDir()
{
    return DataDir() / "NVSE" / "Plugins" / "scripts";
}

fs::path InputCallbackScriptPath()
{
    return UserFunctionDir() / "input_callback.txt";
}

fs::path AudioDir()
{
    // Native-speed folder outside usvfs (see StreamStorageDir). Was under the game's
    // Sound/Voice tree (virtualized), which made per-chunk WAV reads ~tens of ms.
    return StreamStorageDir() / "audio";
}

fs::path DiagnosticsPath()
{
    return BridgeDir() / "native_state.txt";
}

fs::path ScriptRunnerTracePath()
{
    return BridgeDir() / "script_runner_trace.txt";
}

fs::path DebugConfigPath()
{
    return DefaultBridgeDir() / "native_debug.cfg";
}

fs::path RuntimeHeartbeatPath()
{
    return BridgeDir() / "runtime_heartbeat.json";
}

fs::path RuntimeHeartbeatHistoryPath()
{
    return BridgeDir() / "runtime_heartbeat_history.jsonl";
}

fs::path DefaultStackLauncherPath()
{
    std::error_code ec;

    const fs::path devPath = fs::path("C:/sillytavern-clean/tools/fnv-bridge/start_stack_if_needed.ps1");
    if (fs::exists(devPath, ec))
    {
        return devPath;
    }

    ec.clear();
    const fs::path packagedPath = RuntimeDir() / "Tools" / "FNVBridge" / "start_stack_if_needed.ps1";
    if (fs::exists(packagedPath, ec))
    {
        return packagedPath;
    }

    return devPath;
}

fs::path ResolveStackLauncherPath(const std::string& configured)
{
    fs::path launcher = configured.empty() ? DefaultStackLauncherPath() : fs::path(configured);
    if (launcher.empty())
    {
        return {};
    }

    if (launcher.is_relative())
    {
        launcher = RuntimeDir() / launcher;
    }

    return launcher.lexically_normal();
}

fs::path GetPowerShellPath()
{
    wchar_t systemDir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        return fs::path("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe");
    }

    return fs::path(systemDir) / "WindowsPowerShell" / "v1.0" / "powershell.exe";
}

fs::path GetCmdPath()
{
    wchar_t systemDir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        return fs::path("C:/Windows/System32/cmd.exe");
    }

    return fs::path(systemDir) / "cmd.exe";
}

bool LaunchDetachedProcess(const fs::path& application, const std::wstring& commandLine, const fs::path& workingDir)
{
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION processInfo{};
    std::wstring mutableCommandLine = commandLine;

    BOOL ok = CreateProcessW(
        application.wstring().c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS | CREATE_NO_WINDOW,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.wstring().c_str(),
        &startupInfo,
        &processInfo);

    if (!ok)
    {
        LogLine("Bridge stack launch failed for %S (error %lu).", application.wstring().c_str(), GetLastError());
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

bool LaunchStackLauncherPath(const fs::path& launcherPath)
{
    std::error_code ec;
    if (launcherPath.empty() || !fs::exists(launcherPath, ec))
    {
        LogLine("Bridge stack launcher not found: %s", launcherPath.string().c_str());
        return false;
    }

    const std::string extension = ToLowerAscii(launcherPath.extension().string());
    const fs::path workingDir = launcherPath.parent_path().empty() ? RuntimeDir() : launcherPath.parent_path();

    if (extension == ".ps1")
    {
        const fs::path powershell = GetPowerShellPath();
        const std::wstring launcher = launcherPath.wstring();
        std::wstring commandLine = L"\"" + powershell.wstring() + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + launcher + L"\"";
        return LaunchDetachedProcess(powershell, commandLine, workingDir);
    }

    if (extension == ".bat" || extension == ".cmd")
    {
        const fs::path cmd = GetCmdPath();
        const std::wstring launcher = launcherPath.wstring();
        std::wstring commandLine = L"\"" + cmd.wstring() + L"\" /c \"" + launcher + L"\"";
        return LaunchDetachedProcess(cmd, commandLine, workingDir);
    }

    const std::wstring executable = launcherPath.wstring();
    std::wstring commandLine = L"\"" + executable + L"\"";
    return LaunchDetachedProcess(launcherPath, commandLine, workingDir);
}

void MaybeRequestBridgeStackStartup(const char* reason, bool force)
{
    LoadDebugConfigIfNeeded(false);
    if (!g_debugConfig.autoStartStack)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (!force
        && g_state.stackBootstrapAttempted
        && g_state.stackBootstrapAttemptTick
        && (now - g_state.stackBootstrapAttemptTick) < g_debugConfig.stackBootstrapCooldownMs)
    {
        return;
    }

    const fs::path launcherPath = ResolveStackLauncherPath(g_debugConfig.stackLauncherPath);
    g_state.stackBootstrapAttempted = true;
    g_state.stackBootstrapAttemptTick = now;

    if (LaunchStackLauncherPath(launcherPath))
    {
        LogLine("Requested bridge stack startup (%s) via %s.", reason ? reason : "unknown", launcherPath.string().c_str());
    }
}

fs::path VoiceBootstrapStatusPath()
{
    return BridgeDir() / "voice_bootstrap_status.json";
}

fs::path TraceDir()
{
    return BridgeDir() / "traces";
}

fs::path RequestTracePath(std::string_view requestId)
{
    std::string safe(requestId);
    for (char& ch : safe)
    {
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
        {
            ch = '_';
        }
    }
    return TraceDir() / (safe + ".jsonl");
}

std::string NowIsoUtc()
{
    SYSTEMTIME systemTime{};
    GetSystemTime(&systemTime);
    char buffer[40]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(systemTime.wYear),
        static_cast<unsigned>(systemTime.wMonth),
        static_cast<unsigned>(systemTime.wDay),
        static_cast<unsigned>(systemTime.wHour),
        static_cast<unsigned>(systemTime.wMinute),
        static_cast<unsigned>(systemTime.wSecond),
        static_cast<unsigned>(systemTime.wMilliseconds));
    return buffer;
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                char hex[8]{};
                std::snprintf(hex, sizeof(hex), "\\u%04X", static_cast<unsigned>(ch));
                out << hex;
            }
            else
            {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    out << '"';
    return out.str();
}

void EnsureTraceContext(const std::string& requestId)
{
    if (requestId.empty())
    {
        return;
    }

    if (g_state.traceRequestId != requestId)
    {
        g_state.traceRequestId = requestId;
        g_state.traceStartedTick = GetTickCount64();
        EnsureBridgeDirectories();
        std::error_code ec;
        fs::remove(RequestTracePath(requestId), ec);
    }
}

double TraceElapsedMs(const std::string& requestId)
{
    if (requestId.empty())
    {
        return 0.0;
    }

    EnsureTraceContext(requestId);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG delta = (now >= g_state.traceStartedTick) ? (now - g_state.traceStartedTick) : 0;
    return static_cast<double>(delta);
}

void TraceRequestEvent(const std::string& requestId, const std::string& stage,
    const std::vector<std::pair<std::string, std::string>>& stringFields,
    const std::vector<std::pair<std::string, double>>& numberFields,
    const std::vector<std::pair<std::string, bool>>& boolFields)
{
    if (!g_debugConfig.requestTracingEnabled)
    {
        return;
    }

    if (requestId.empty() || stage.empty())
    {
        return;
    }

    EnsureTraceContext(requestId);
    EnsureBridgeDirectories();
    std::ofstream out(RequestTracePath(requestId), std::ios::binary | std::ios::app);
    if (!out)
    {
        return;
    }

    out << "{";
    out << "\"request_id\":" << JsonEscape(requestId);
    out << ",\"stage\":" << JsonEscape(stage);
    out << ",\"at\":" << JsonEscape(NowIsoUtc());
    out << ",\"elapsed_ms\":" << std::fixed << std::setprecision(3) << TraceElapsedMs(requestId);
    out.unsetf(std::ios::floatfield);
    out << std::setprecision(6);

    for (const auto& [name, value] : stringFields)
    {
        out << ",\"" << name << "\":" << JsonEscape(value);
    }
    for (const auto& [name, value] : numberFields)
    {
        out << ",\"" << name << "\":" << std::fixed << std::setprecision(3) << value;
        out.unsetf(std::ios::floatfield);
        out << std::setprecision(6);
    }
    for (const auto& [name, value] : boolFields)
    {
        out << ",\"" << name << "\":" << (value ? "true" : "false");
    }

    out << "}\n";
}

std::string GenerateRequestId()
{
    static UInt32 sequence = 0;
    std::ostringstream out;
    out << "req_" << static_cast<unsigned long long>(GetTickCount64()) << "_" << ++sequence;
    return out.str();
}

std::string GenerateSaveStateEventId()
{
    static UInt32 sequence = 0;
    std::ostringstream out;
    out << "saveevt_" << static_cast<unsigned long long>(GetTickCount64()) << "_" << ++sequence;
    return out.str();
}

bool DispatchSaveStateEvent(const std::string& eventType, const std::string& savePath, bool waitForAck)
{
    if (eventType.empty())
    {
        return false;
    }

    EnsureBridgeDirectories();
    const std::string eventId = GenerateSaveStateEventId();
    const fs::path eventPath = SaveStateEventPath(eventId);
    const fs::path ackPath = SaveStateAckPath(eventId);
    const std::string saveName = savePath.empty() ? "" : fs::path(savePath).filename().string();

    std::error_code ec;
    fs::remove(eventPath, ec);
    fs::remove(ackPath, ec);

    std::ofstream out(eventPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LogLine("Failed to open save-state event path for %s.", eventType.c_str());
        return false;
    }

    out << eventId << "\n";
    out << eventType << "\n";
    out << savePath << "\n";
    out << saveName << "\n";
    out << NowIsoUtc() << "\n";
    out.close();
    if (!out)
    {
        LogLine("Failed while writing save-state event %s.", eventType.c_str());
        return false;
    }

    if (waitForAck)
    {
        g_state.saveStateSyncPending = true;
        g_state.saveStateSyncEventId = eventId;
        g_state.saveStateSyncType = eventType;
        g_state.saveStateSyncLastPollTick = 0;
        g_state.saveStateSyncHudMessageTick = 0;
        g_state.saveStateSyncStartedTick = GetTickCount();
    }

    LogLine("Queued save-state event %s id=%s for %s.", eventType.c_str(), eventId.c_str(), savePath.c_str());
    return true;
}

void PollSaveStateSyncAck()
{
    if (!g_state.saveStateSyncPending || g_state.saveStateSyncEventId.empty())
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (g_state.saveStateSyncLastPollTick && (now - g_state.saveStateSyncLastPollTick) < kSaveStateAckPollMs)
    {
        return;
    }

    g_state.saveStateSyncLastPollTick = now;
    const fs::path ackPath = SaveStateAckPath(g_state.saveStateSyncEventId);
    if (!fs::exists(ackPath))
    {
        if (g_state.saveStateSyncStartedTick
            && (now - g_state.saveStateSyncStartedTick) >= kSaveStateSyncTimeoutMs)
        {
            LogLine("Save-state sync %s id=%s timed out after %lu ms; clearing pending state.",
                g_state.saveStateSyncType.c_str(),
                g_state.saveStateSyncEventId.c_str(),
                static_cast<unsigned long>(now - g_state.saveStateSyncStartedTick));
            g_state.saveStateSyncPending = false;
            g_state.saveStateSyncEventId.clear();
            g_state.saveStateSyncType.clear();
            g_state.saveStateSyncLastPollTick = 0;
            g_state.saveStateSyncHudMessageTick = 0;
            g_state.saveStateSyncStartedTick = 0;
        }
        return;
    }

    std::ifstream in(ackPath, std::ios::binary);
    if (!in)
    {
        return;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }

    const std::string status = lines.size() > 1 ? ToLowerAscii(Trim(lines[1])) : "";
    const std::string message = lines.size() > 2 ? Trim(lines[2]) : "";
    const bool ok = status == "ok";

    std::error_code ec;
    fs::remove(ackPath, ec);

    LogLine("Save-state sync %s completed with status=%s message=%s",
        g_state.saveStateSyncType.c_str(),
        status.c_str(),
        message.c_str());

    g_state.saveStateSyncPending = false;
    g_state.saveStateSyncEventId.clear();
    g_state.saveStateSyncType.clear();
    g_state.saveStateSyncLastPollTick = 0;
    g_state.saveStateSyncHudMessageTick = 0;
    g_state.saveStateSyncStartedTick = 0;
    g_state.saveStateSyncStartedTick = 0;

    if (!ok)
    {
        ShowHudMessage(message.empty() ? "Bridge save sync failed." : message);
    }
}

void EnsureBridgeDirectories()
{
    fs::create_directories(BridgeDir() / "inbox");
    fs::create_directories(BridgeDir() / "outbox");
    fs::create_directories(OutboxChunkDir());
    fs::create_directories(BridgeDir() / "processed");
    fs::create_directories(SaveStateEventsDir());
    fs::create_directories(SaveStateAcksDir());
    fs::create_directories(NativeActionCommandDir());
    fs::create_directories(TraceDir());
    fs::create_directories(AudioDir());
    fs::create_directories(UserFunctionDir());
    fs::create_directories(ScriptRunnerDir());
}

void WriteDiagnostics(const std::string& body)
{
    EnsureBridgeDirectories();
    std::ofstream out(DiagnosticsPath(), std::ios::binary | std::ios::trunc);
    out << body;
    if (!body.empty() && body.back() != '\n')
    {
        out << "\n";
    }
    if (!g_state.traceRequestId.empty())
    {
        out << "trace_request_id=" << g_state.traceRequestId << "\n";
        out << "trace_file=" << RequestTracePath(g_state.traceRequestId).string() << "\n";
    }
}

void EnsureInputCallbackScript()
{
    EnsureBridgeDirectories();

    constexpr char kCallbackScript[] =
        "string_var sTextInput\n"
        "\n"
        "begin Function {sTextInput}\n"
        "    WriteStringToFile \"Data/NVBridge/ui_submit.txt\" 0 \"%z\" sTextInput\n"
        "    sv_Destruct sTextInput\n"
        "end\n";

    const fs::path path = InputCallbackScriptPath();
    bool shouldWrite = true;

    if (fs::exists(path))
    {
        std::ifstream in(path, std::ios::binary);
        const std::string current((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        shouldWrite = current != kCallbackScript;
    }

    if (!shouldWrite)
    {
        return;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << kCallbackScript;
}

void WriteTextFileIfChanged(const fs::path& path, const std::string& body)
{
    bool shouldWrite = true;
    if (fs::exists(path))
    {
        std::ifstream in(path, std::ios::binary);
        const std::string current((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        shouldWrite = current != body;
    }

    if (!shouldWrite)
    {
        return;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

void EnsureDialoguePlaybackScripts()
{
    EnsureBridgeDirectories();

    const std::string femaleGoodspringsScript =
        "ref rTarget\n"
        "let rTarget := \"00000014\"\n"
        "GetSelf.SayTo rTarget vcg02_greeting 1 1\n";

    const std::string maleOldGoodspringsScript =
        "ref rTarget\n"
        "let rTarget := \"00000014\"\n"
        "GetSelf.SayTo rTarget vfreeformg_vfreeformgoodsp 1 1\n";

    WriteTextFileIfChanged(ScriptRunnerDir() / "sayto_female_goodsprings.txt", femaleGoodspringsScript);
    WriteTextFileIfChanged(ScriptRunnerDir() / "sayto_maleold_goodsprings.txt", maleOldGoodspringsScript);
}

std::string DescribeScriptResult(const NVSEArrayVarInterface::Element& result)
{
    switch (result.GetType())
    {
    case NVSEArrayVarInterface::Element::kType_Numeric:
    {
        std::ostringstream out;
        out << result.GetNumber();
        return out.str();
    }
    case NVSEArrayVarInterface::Element::kType_Form:
        return FormIdHex(result.GetFormID());
    case NVSEArrayVarInterface::Element::kType_String:
        return result.GetString() ? result.GetString() : "";
    case NVSEArrayVarInterface::Element::kType_Array:
    {
        std::ostringstream out;
        out << result.GetArrayID();
        return out.str();
    }
    default:
        return "invalid";
    }
}

std::vector<std::string> BuildRunBatchScriptPathCandidates(const fs::path& scriptPath)
{
    std::vector<std::string> candidates;
    auto appendCandidate = [&candidates](const fs::path& candidatePath) {
        if (candidatePath.empty())
        {
            return;
        }

        fs::path preferredPath = candidatePath;
        const std::string preferred = preferredPath.make_preferred().string();
        if (!preferred.empty() && std::find(candidates.begin(), candidates.end(), preferred) == candidates.end())
        {
            candidates.push_back(preferred);
        }

        std::string forward = preferred;
        std::replace(forward.begin(), forward.end(), '\\', '/');
        if (!forward.empty() && std::find(candidates.begin(), candidates.end(), forward) == candidates.end())
        {
            candidates.push_back(forward);
        }
    };

    appendCandidate(scriptPath.filename());

    std::error_code ec;
    const fs::path absoluteScriptPath = fs::absolute(scriptPath, ec);
    if (ec)
    {
        return candidates;
    }

    const fs::path runtimeDir = RuntimeDir();
    if (!runtimeDir.empty())
    {
        const fs::path relativeToRuntime = absoluteScriptPath.lexically_relative(runtimeDir);
        const bool escapesRuntime = !relativeToRuntime.empty()
            && relativeToRuntime.begin() != relativeToRuntime.end()
            && (*relativeToRuntime.begin() == ".." || *relativeToRuntime.begin() == ".");
        if (!relativeToRuntime.empty() && !escapesRuntime)
        {
            appendCandidate(relativeToRuntime);
        }
    }

    const fs::path dataDir = DataDir();
    if (!dataDir.empty())
    {
        const fs::path relativeToData = absoluteScriptPath.lexically_relative(dataDir);
        const bool escapesData = !relativeToData.empty()
            && relativeToData.begin() != relativeToData.end()
            && (*relativeToData.begin() == ".." || *relativeToData.begin() == ".");
        if (!relativeToData.empty() && !escapesData)
        {
            appendCandidate(relativeToData);
        }
    }

    return candidates;
}

bool EnsureOpenTextInputScript()
{
    if (g_openTextInputScript)
    {
        return true;
    }

    if (!g_scriptInterface)
    {
        LogLine("Script interface unavailable; cannot open in-game text input.");
        return false;
    }

    EnsureInputCallbackScript();

    constexpr char kLauncherScript[] = R"(
string_var sTitle
ref rCallback

Begin Function { sTitle }
    let rCallback := GetUDFFromFile "fnv_bridge/input_callback.txt"
    if rCallback
        ShowTextInputMenu rCallback 700 220 "%z" sTitle
        SetTextInputExtendedProps 0 0 1 280 2
    endif
End
)";

    g_openTextInputScript = g_scriptInterface->CompileScript(kLauncherScript);
    if (!g_openTextInputScript)
    {
        LogLine("Failed to compile TextEditMenu launcher script.");
        return false;
    }

    return true;
}

bool EnsureCloseTextInputScript()
{
    if (g_closeTextInputScript)
    {
        return true;
    }

    if (!g_scriptInterface)
    {
        LogLine("Script interface unavailable; cannot compile TextEditMenu close helper.");
        return false;
    }

    constexpr char kCloseScript[] = R"(
Begin Function {}
    CloseActiveMenu
End
)";

    g_closeTextInputScript = g_scriptInterface->CompileScript(kCloseScript);
    if (!g_closeTextInputScript)
    {
        LogLine("Failed to compile TextEditMenu close helper script.");
        return false;
    }

    return true;
}

bool EnsureStartCombatScript()
{
    if (g_startCombatScript)
    {
        return true;
    }

    if (!g_scriptInterface)
    {
        LogLine("Script interface unavailable; cannot trigger NPC combat.");
        return false;
    }

    constexpr char kStartCombatScript[] = R"(
ref rActor
ref rTarget

Begin Function { rActor, rTarget }
    if rActor && rTarget
        rActor.StartCombat rTarget
    endif
End
)";

    g_startCombatScript = g_scriptInterface->CompileScript(kStartCombatScript);
    if (!g_startCombatScript)
    {
        LogLine("Failed to compile StartCombat helper script.");
        return false;
    }

    return true;
}

bool EnsurePlayerTeammateScript()
{
    if (g_setPlayerTeammateScript)
    {
        return true;
    }

    if (!g_scriptInterface)
    {
        LogLine("Script interface unavailable; cannot set NPC teammate state.");
        return false;
    }

    constexpr char kSetPlayerTeammateScript[] = R"(
ref rActor
int iTeammate
float fIssued

Begin Function { rActor, iTeammate }
    let fIssued := 0
    if rActor
        rActor.SetPlayerTeammate iTeammate
        if iTeammate
            rActor.SetRestrained 0
            rActor.EvaluatePackage
        endif
        let fIssued := 1
    endif
    SetFunctionValue fIssued
End
)";

    g_setPlayerTeammateScript = g_scriptInterface->CompileScript(kSetPlayerTeammateScript);
    if (!g_setPlayerTeammateScript)
    {
        LogLine("Failed to compile SetPlayerTeammate helper script.");
        return false;
    }

    return true;
}

bool EnsureConversationHoldScripts()
{
    if (g_startConversationScript && g_startLookScript && g_stopLookScript && g_evaluatePackageScript && g_isCurrentPackageScript && g_addScriptPackageScript && g_removeScriptPackageScript && g_getRestrainedScript && g_setRestrainedScript && g_clearRestrainedScript && g_setAngleScript)
    {
        return true;
    }

    if (!g_scriptInterface)
    {
        LogLine("Script interface unavailable; cannot compile conversation hold helpers.");
        return false;
    }

    if (!g_getRestrainedScript)
    {
        constexpr char kGetRestrainedScript[] = R"(
ref rActor
float fRestrained

Begin Function { rActor }
    let fRestrained := 0
    if rActor
        let fRestrained := rActor.GetRestrained
    endif
    SetFunctionValue fRestrained
End
)";

        g_getRestrainedScript = g_scriptInterface->CompileScript(kGetRestrainedScript);
        if (!g_getRestrainedScript)
        {
            LogLine("Failed to compile GetRestrained helper script.");
            return false;
        }
    }

    if (!g_startConversationScript)
    {
        constexpr char kStartConversationScript[] = R"(
ref rActor
ref rTarget
ref rTopic
ref rSpeakerLoc
ref rTargetLoc
float fIssued

Begin Function { rActor, rTarget, rTopic, rSpeakerLoc, rTargetLoc }
    let fIssued := 0
    if rActor && rTarget && rTopic
        rActor.StartConversation rTarget rTopic rSpeakerLoc rTargetLoc 1 1
        let fIssued := 1
    endif
    SetFunctionValue fIssued
End
)";

        g_startConversationScript = g_scriptInterface->CompileScript(kStartConversationScript);
        if (!g_startConversationScript)
        {
            LogLine("Failed to compile StartConversation helper script.");
            return false;
        }
    }

    if (!g_evaluatePackageScript)
    {
        constexpr char kEvaluatePackageScript[] = R"(
ref rActor
float fIssued

Begin Function { rActor }
    let fIssued := 0
    if rActor
        rActor.EvaluatePackage
        let fIssued := 1
    endif
    SetFunctionValue fIssued
End
)";

        g_evaluatePackageScript = g_scriptInterface->CompileScript(kEvaluatePackageScript);
        if (!g_evaluatePackageScript)
        {
            LogLine("Failed to compile EvaluatePackage helper script.");
            return false;
        }
    }

    if (!g_addScriptPackageScript)
    {
        constexpr char kAddScriptPackageScript[] = R"(
ref rActor
ref rPackage
float fIssued

Begin Function { rActor, rPackage }
    let fIssued := 0
    if rActor && rPackage
        rActor.AddScriptPackage rPackage
        let fIssued := 1
    endif
    SetFunctionValue fIssued
End
)";

        g_addScriptPackageScript = g_scriptInterface->CompileScript(kAddScriptPackageScript);
        if (!g_addScriptPackageScript)
        {
            LogLine("Failed to compile AddScriptPackage helper script.");
            return false;
        }
    }

    if (!g_removeScriptPackageScript)
    {
        constexpr char kRemoveScriptPackageScript[] = R"(
ref rActor
float fIssued

Begin Function { rActor }
    let fIssued := 0
    if rActor
        rActor.RemoveScriptPackage
        let fIssued := 1
    endif
    SetFunctionValue fIssued
End
)";

        g_removeScriptPackageScript = g_scriptInterface->CompileScript(kRemoveScriptPackageScript);
        if (!g_removeScriptPackageScript)
        {
            LogLine("Failed to compile RemoveScriptPackage helper script.");
            return false;
        }
    }

    if (!g_isCurrentPackageScript)
    {
        constexpr char kIsCurrentPackageScript[] = R"(
ref rActor
ref rPackage
float fMatch

Begin Function { rActor, rPackage }
    let fMatch := 0
    if rActor && rPackage
        if rActor.GetCurrentPackage == rPackage
            let fMatch := 1
        endif
    endif
    SetFunctionValue fMatch
End
)";

        g_isCurrentPackageScript = g_scriptInterface->CompileScript(kIsCurrentPackageScript);
        if (!g_isCurrentPackageScript)
        {
            LogLine("Failed to compile GetCurrentPackage helper script.");
            return false;
        }
    }

    if (!g_startLookScript)
    {
        constexpr char kStartLookScript[] = R"(
ref rActor
ref rTarget

Begin Function { rActor, rTarget }
    if rActor && rTarget
        rActor.Look rTarget 1
    endif
End
)";

        g_startLookScript = g_scriptInterface->CompileScript(kStartLookScript);
        if (!g_startLookScript)
        {
            LogLine("Failed to compile Look helper script.");
            return false;
        }
    }

    if (!g_stopLookScript)
    {
        constexpr char kStopLookScript[] = R"(
ref rActor

Begin Function { rActor }
    if rActor
        rActor.StopLook
    endif
End
)";

        g_stopLookScript = g_scriptInterface->CompileScript(kStopLookScript);
        if (!g_stopLookScript)
        {
            LogLine("Failed to compile StopLook helper script.");
            return false;
        }
    }

    if (!g_setRestrainedScript)
    {
        constexpr char kSetRestrainedScript[] = R"(
ref rActor

Begin Function { rActor }
    if rActor
        rActor.SetRestrained 1
    endif
End
)";

        g_setRestrainedScript = g_scriptInterface->CompileScript(kSetRestrainedScript);
        if (!g_setRestrainedScript)
        {
            LogLine("Failed to compile SetRestrained helper script.");
            return false;
        }
    }

    if (!g_clearRestrainedScript)
    {
        constexpr char kClearRestrainedScript[] = R"(
ref rActor

Begin Function { rActor }
    if rActor
        rActor.SetRestrained 0
        rActor.EvaluatePackage
    endif
End
)";

        g_clearRestrainedScript = g_scriptInterface->CompileScript(kClearRestrainedScript);
        if (!g_clearRestrainedScript)
        {
            LogLine("Failed to compile clear-Restrained helper script.");
            return false;
        }
    }

    if (!g_setAngleScript)
    {
        constexpr char kSetAngleScript[] = R"(
ref rActor
float fAngle

Begin Function { rActor, fAngle }
    if rActor
        rActor.SetAngle Z fAngle
    endif
End
)";

        g_setAngleScript = g_scriptInterface->CompileScript(kSetAngleScript);
        if (!g_setAngleScript)
        {
            LogLine("Failed to compile SetAngle helper script.");
            return false;
        }
    }

    if (!g_faceObjectScript && !g_faceObjectScriptAttempted)
    {
        g_faceObjectScriptAttempted = true;
        constexpr char kFaceObjectScript[] = R"(
ref rActor
ref rTarget

Begin Function { rActor, rTarget }
    if rActor && rTarget
        rActor.FaceObject rTarget
    endif
End
)";

        g_faceObjectScript = g_scriptInterface->CompileScript(kFaceObjectScript);
        if (!g_faceObjectScript)
        {
            LogLine("Failed to compile FaceObject helper script; body facing will rely on head look only.");
        }
    }

    if (!g_applyNoMovePackageScript && !g_applyNoMovePackageScriptAttempted)
    {
        g_applyNoMovePackageScriptAttempted = true;
        constexpr char kApplyNoMovePackageScript[] = R"(
ref rActor
float fIssued

Begin Function { rActor }
    let fIssued := 0
    if rActor
        rActor.AddScriptPackage DefaultSandboxNoMoveCurrentLocation200
        rActor.EvaluatePackage
        let fIssued := 1
    endif
    SetFunctionValue fIssued
End
)";

        g_applyNoMovePackageScript = g_scriptInterface->CompileScript(kApplyNoMovePackageScript);
        if (!g_applyNoMovePackageScript)
        {
            LogLine("Failed to compile no-move package helper script; conversation mode will use restrained fallback.");
        }
    }

    return true;
}

bool EnsureConsoleCommandHelper()
{
    if (g_consoleCommandScript)
    {
        return true;
    }

    if (!g_scriptInterface)
    {
        LogLine("Script interface unavailable; cannot compile Console helper.");
        return false;
    }

    constexpr char kConsoleCommandHelper[] = R"(
string_var sCommand

Begin Function { sCommand }
    if eval sCommand != ""
        Console sCommand
    endif
End
)";

    g_consoleCommandScript = g_scriptInterface->CompileScript(kConsoleCommandHelper);
    if (!g_consoleCommandScript)
    {
        LogLine("Failed to compile Console helper script.");
        return false;
    }

    return true;
}

std::string Trim(std::string value)
{
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string ReplaceAll(std::string value, char from, char to)
{
    std::replace(value.begin(), value.end(), from, to);
    return value;
}

std::string SanitizeLine(std::string_view value)
{
    std::string text(value);
    text = ReplaceAll(text, '\r', ' ');
    text = ReplaceAll(text, '\n', ' ');
    text = ReplaceAll(text, '\t', ' ');
    return Trim(text);
}

template <typename T>
void WriteLittleEndian(std::ostream& out, T value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

std::vector<BYTE> BuildWaveBytesFromPcm(const std::vector<BYTE>& pcmBytes)
{
    const DWORD sampleRate = kVoiceCaptureSampleRate;
    const WORD channels = kVoiceCaptureChannels;
    const WORD bitsPerSample = kVoiceCaptureBitsPerSample;
    const WORD blockAlign = static_cast<WORD>(channels * bitsPerSample / 8);
    const DWORD byteRate = sampleRate * blockAlign;
    const DWORD dataSize = static_cast<DWORD>(pcmBytes.size());
    const DWORD riffSize = 36u + dataSize;

    std::ostringstream out(std::ios::binary);
    out.write("RIFF", 4);
    WriteLittleEndian(out, riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    WriteLittleEndian<DWORD>(out, 16u);
    WriteLittleEndian<WORD>(out, 1u);
    WriteLittleEndian(out, channels);
    WriteLittleEndian(out, static_cast<DWORD>(sampleRate));
    WriteLittleEndian(out, byteRate);
    WriteLittleEndian(out, blockAlign);
    WriteLittleEndian(out, bitsPerSample);
    out.write("data", 4);
    WriteLittleEndian(out, dataSize);
    if (!pcmBytes.empty())
    {
        out.write(reinterpret_cast<const char*>(pcmBytes.data()), static_cast<std::streamsize>(pcmBytes.size()));
    }

    const std::string bytes = out.str();
    return std::vector<BYTE>(bytes.begin(), bytes.end());
}

std::string ToUpperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> SplitCommaList(const std::string& value)
{
    std::vector<std::string> items;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        item = Trim(item);
        if (!item.empty())
        {
            items.push_back(item);
        }
    }
    return items;
}

std::map<std::string, std::string> ParseKeyValueLines(const std::vector<std::string>& lines, size_t startIndex)
{
    std::map<std::string, std::string> fields;
    for (size_t index = startIndex; index < lines.size(); ++index)
    {
        const std::string& line = lines[index];
        const size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            continue;
        }
        const std::string key = ToLowerAscii(Trim(line.substr(0, separator)));
        const std::string value = Trim(line.substr(separator + 1));
        if (!key.empty())
        {
            fields[key] = value;
        }
    }
    return fields;
}

std::string GetField(const std::map<std::string, std::string>& fields, const char* key)
{
    const auto it = fields.find(key);
    return it == fields.end() ? std::string() : it->second;
}

int Base64Value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z')
    {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0' + 52;
    }
    if (ch == '+')
    {
        return 62;
    }
    if (ch == '/')
    {
        return 63;
    }
    return -1;
}

std::optional<std::string> DecodeBase64String(const std::string& input, size_t maxBytes)
{
    std::string output;
    output.reserve(input.size() * 3 / 4);

    int value = 0;
    int valueBits = -8;
    for (unsigned char ch : input)
    {
        if (std::isspace(ch))
        {
            continue;
        }
        if (ch == '=')
        {
            break;
        }

        const int decoded = Base64Value(ch);
        if (decoded < 0)
        {
            return std::nullopt;
        }

        value = (value << 6) | decoded;
        valueBits += 6;
        if (valueBits >= 0)
        {
            output.push_back(static_cast<char>((value >> valueBits) & 0xFF));
            if (output.size() > maxBytes)
            {
                return std::nullopt;
            }
            valueBits -= 8;
        }
    }

    return output;
}

std::string HashString64Hex(const std::string& value)
{
    unsigned long long hash = 1469598103934665603ull;
    for (unsigned char ch : value)
    {
        hash ^= static_cast<unsigned long long>(ch);
        hash *= 1099511628211ull;
    }

    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool ParseConfigBool(std::string value, bool fallback)
{
    value = ToUpperAscii(Trim(std::move(value)));
    if (value == "1" || value == "TRUE" || value == "YES" || value == "ON")
    {
        return true;
    }
    if (value == "0" || value == "FALSE" || value == "NO" || value == "OFF")
    {
        return false;
    }
    return fallback;
}

bool IsIntegerToken(const std::string& value)
{
    if (value.empty())
    {
        return false;
    }

    size_t index = 0;
    if (value[index] == '-' || value[index] == '+')
    {
        ++index;
    }

    if (index >= value.size())
    {
        return false;
    }

    for (; index < value.size(); ++index)
    {
        if (!std::isdigit(static_cast<unsigned char>(value[index])))
        {
            return false;
        }
    }

    return true;
}

void ApplyResponseMetadata(ResponsePayload& payload, const std::string& rawKey, const std::string& value)
{
    const std::string key = ToLowerAscii(Trim(rawKey));
    if (key == "action_npc_key")
    {
        payload.actionNpcKey = Trim(value);
        return;
    }
    if (key == "action_npc_name")
    {
        payload.actionNpcName = Trim(value);
        return;
    }
    if (key == "admin_voice" || key == "non_positional_audio")
    {
        payload.nonPositionalAudio = ParseConfigBool(value, payload.nonPositionalAudio);
    }
}

bool IsNonPositionalChunkMetadata(const std::string& rawKey, const std::string& value, bool fallback)
{
    const std::string key = ToLowerAscii(Trim(rawKey));
    if (key == "admin_voice" || key == "non_positional_audio")
    {
        return ParseConfigBool(value, fallback);
    }
    return fallback;
}

void WriteDefaultDebugConfigIfMissing()
{
    std::error_code ec;
    if (fs::exists(DebugConfigPath(), ec))
    {
        return;
    }

    EnsureBridgeDirectories();
    std::ofstream out(DebugConfigPath(), std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return;
    }

    out
        << "# FNV bridge native debug config\r\n"
        << "runtime_heartbeat=1\r\n"
        << "speech_animation=1\r\n"
        << "speech_write_phoneme_values=1\r\n"
        << "speech_write_face_flags=1\r\n"
        << "speech_clear_binding_on_stop=1\r\n"
        << "subtitles=1\r\n"
        << "listener_updates=1\r\n"
        << "directsound_3d=1\r\n"
        << "directsound_software_buffer=1\r\n"
        << "drain_queued_chunks_after_final=1\r\n"
        << "single_buffer_streaming=1\r\n"
        << "request_tracing=0\r\n"
        << "speech_weight_scale=1.00\r\n"
        << "speech_animation_update_interval_ms=50\r\n"
        << "speech_binding_validation_interval_ms=500\r\n"
        << "conversation_mode=1\r\n"
        << "conversation_mode_release_distance_m=10.00\r\n"
        << "conversation_face_player_interval_ms=1000\r\n"
        << "conversation_look_refresh_interval_ms=1500\r\n"
        << "runtime_heartbeat_interval_ms=100\r\n"
        << "streaming_chunk_overlap_ms=40\r\n"
        << "caption_ms_per_char=75\r\n"
        << "autostart_stack=1\r\n"
        << "stack_bootstrap_cooldown_ms=15000\r\n"
        << "stack_launcher_path=" << DefaultStackLauncherPath().string() << "\r\n"
        << "# bridge_root_path=C:/Users/your-user/AppData/Local/ModOrganizer/New Vegas/overwrite/NVBridge\r\n";
}

void LoadDebugConfigIfNeeded(bool force)
{
    const DWORD now = GetTickCount();
    if (!force && g_state.lastDebugConfigPollTick && (now - g_state.lastDebugConfigPollTick) < kDebugConfigPollMs)
    {
        return;
    }
    g_state.lastDebugConfigPollTick = now;

    WriteDefaultDebugConfigIfMissing();

    std::error_code ec;
    const bool exists = fs::exists(DebugConfigPath(), ec);
    if (!exists)
    {
        return;
    }

    const auto writeTime = fs::last_write_time(DebugConfigPath(), ec);
    if (!force && g_debugConfigLoaded && !ec && writeTime == g_debugConfigWriteTime)
    {
        return;
    }

    DebugConfig config{};
    std::ifstream in(DebugConfigPath(), std::ios::binary);
    if (!in)
    {
        return;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        line = Trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }

        const std::string key = ToLowerAscii(Trim(line.substr(0, equals)));
        const std::string value = Trim(line.substr(equals + 1));
        if (key == "runtime_heartbeat")
        {
            config.runtimeHeartbeatEnabled = ParseConfigBool(value, config.runtimeHeartbeatEnabled);
        }
        else if (key == "speech_animation")
        {
            config.speechAnimationEnabled = ParseConfigBool(value, config.speechAnimationEnabled);
        }
        else if (key == "speech_write_phoneme_values")
        {
            config.speechWritePhonemeValues = ParseConfigBool(value, config.speechWritePhonemeValues);
        }
        else if (key == "speech_write_face_flags")
        {
            config.speechWriteFaceFlags = ParseConfigBool(value, config.speechWriteFaceFlags);
        }
        else if (key == "speech_clear_binding_on_stop")
        {
            config.speechClearBindingOnStop = ParseConfigBool(value, config.speechClearBindingOnStop);
        }
        else if (key == "subtitles")
        {
            config.subtitlesEnabled = ParseConfigBool(value, config.subtitlesEnabled);
        }
        else if (key == "listener_updates")
        {
            config.listenerUpdatesEnabled = ParseConfigBool(value, config.listenerUpdatesEnabled);
        }
        else if (key == "directsound_3d")
        {
            config.directSound3dEnabled = ParseConfigBool(value, config.directSound3dEnabled);
        }
        else if (key == "directsound_software_buffer")
        {
            config.directSoundSoftwareBufferEnabled = ParseConfigBool(value, config.directSoundSoftwareBufferEnabled);
        }
        else if (key == "drain_queued_chunks_after_final")
        {
            config.drainQueuedChunksAfterFinal = ParseConfigBool(value, config.drainQueuedChunksAfterFinal);
        }
        else if (key == "single_buffer_streaming")
        {
            config.singleBufferStreaming = ParseConfigBool(value, config.singleBufferStreaming);
        }
        else if (key == "request_tracing")
        {
            config.requestTracingEnabled = ParseConfigBool(value, config.requestTracingEnabled);
        }
        else if (key == "conversation_mode")
        {
            config.conversationModeEnabled = ParseConfigBool(value, config.conversationModeEnabled);
        }
        else if (key == "speech_weight_scale")
        {
            const float scale = static_cast<float>(std::atof(value.c_str()));
            if (scale >= 0.10f && scale <= 3.00f)
            {
                config.speechWeightScale = scale;
            }
        }
        else if (key == "conversation_mode_release_distance_m")
        {
            const float distanceMeters = static_cast<float>(std::atof(value.c_str()));
            if (distanceMeters >= 1.0f && distanceMeters <= 100.0f)
            {
                config.conversationModeReleaseDistanceMeters = distanceMeters;
            }
        }
        else if (key == "speech_animation_update_interval_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 16 && interval <= 250)
            {
                config.speechAnimationUpdateIntervalMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "speech_binding_validation_interval_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 100 && interval <= 2000)
            {
                config.speechBindingValidationIntervalMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "conversation_face_player_interval_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 50 && interval <= 5000)
            {
                config.conversationModeFaceRefreshIntervalMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "conversation_look_refresh_interval_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 250 && interval <= 30000)
            {
                config.conversationLookRefreshIntervalMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "runtime_heartbeat_interval_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 10 && interval <= 5000)
            {
                config.runtimeHeartbeatIntervalMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "streaming_chunk_overlap_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 0 && interval <= static_cast<int>(kMaxStreamingChunkOverlapMs))
            {
                config.streamingChunkOverlapMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "caption_ms_per_char")
        {
            const int v = std::atoi(value.c_str());
            if (v >= 10 && v <= 500)
            {
                config.captionMsPerChar = static_cast<DWORD>(v);
            }
        }
        else if (key == "autostart_stack")
        {
            config.autoStartStack = ParseConfigBool(value, config.autoStartStack);
        }
        else if (key == "stack_bootstrap_cooldown_ms")
        {
            const int interval = std::atoi(value.c_str());
            if (interval >= 1000 && interval <= 600000)
            {
                config.stackBootstrapCooldownMs = static_cast<DWORD>(interval);
            }
        }
        else if (key == "stack_launcher_path")
        {
            config.stackLauncherPath = value;
        }
        else if (key == "bridge_root_path")
        {
            config.bridgeRootPath = value;
        }
    }

    if (config.stackLauncherPath.empty())
    {
        config.stackLauncherPath = DefaultStackLauncherPath().string();
    }

    g_debugConfig = config;
    g_debugConfigLoaded = true;
    if (!ec)
    {
        g_debugConfigWriteTime = writeTime;
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "debug_config_loaded",
            {},
            {
                { "speech_weight_scale", static_cast<double>(g_debugConfig.speechWeightScale) },
                { "conversation_mode_release_distance_m", static_cast<double>(g_debugConfig.conversationModeReleaseDistanceMeters) },
                { "speech_animation_update_interval_ms", static_cast<double>(g_debugConfig.speechAnimationUpdateIntervalMs) },
                { "speech_binding_validation_interval_ms", static_cast<double>(g_debugConfig.speechBindingValidationIntervalMs) },
                { "conversation_face_player_interval_ms", static_cast<double>(g_debugConfig.conversationModeFaceRefreshIntervalMs) },
                { "conversation_look_refresh_interval_ms", static_cast<double>(g_debugConfig.conversationLookRefreshIntervalMs) },
                { "runtime_heartbeat_interval_ms", static_cast<double>(g_debugConfig.runtimeHeartbeatIntervalMs) },
                { "streaming_chunk_overlap_ms", static_cast<double>(g_debugConfig.streamingChunkOverlapMs) },
                { "stack_bootstrap_cooldown_ms", static_cast<double>(g_debugConfig.stackBootstrapCooldownMs) },
            },
            {
                { "runtime_heartbeat", g_debugConfig.runtimeHeartbeatEnabled },
                { "speech_animation", g_debugConfig.speechAnimationEnabled },
                { "speech_write_phoneme_values", g_debugConfig.speechWritePhonemeValues },
                { "speech_write_face_flags", g_debugConfig.speechWriteFaceFlags },
                { "speech_clear_binding_on_stop", g_debugConfig.speechClearBindingOnStop },
                { "subtitles", g_debugConfig.subtitlesEnabled },
                { "listener_updates", g_debugConfig.listenerUpdatesEnabled },
                { "directsound_3d", g_debugConfig.directSound3dEnabled },
                { "directsound_software_buffer", g_debugConfig.directSoundSoftwareBufferEnabled },
                { "drain_queued_chunks_after_final", g_debugConfig.drainQueuedChunksAfterFinal },
                { "single_buffer_streaming", g_debugConfig.singleBufferStreaming },
                { "request_tracing", g_debugConfig.requestTracingEnabled },
                { "conversation_mode", g_debugConfig.conversationModeEnabled },
                { "autostart_stack", g_debugConfig.autoStartStack },
                { "has_bridge_root_override", !g_debugConfig.bridgeRootPath.empty() },
            });
    }
}

void WriteRuntimeHeartbeatIfNeeded(bool force)
{
    LoadDebugConfigIfNeeded(false);
    if (!g_debugConfig.runtimeHeartbeatEnabled)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (!force && g_state.lastRuntimeHeartbeatTick
        && (now - g_state.lastRuntimeHeartbeatTick) < g_debugConfig.runtimeHeartbeatIntervalMs)
    {
        return;
    }
    g_state.lastRuntimeHeartbeatTick = now;
    ++g_state.runtimeHeartbeatFrame;

    EnsureBridgeDirectories();
    std::ofstream out(RuntimeHeartbeatPath(), std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return;
    }

    const DWORD speechRemainingMs = g_state.activeSpeechUntilTick && now < g_state.activeSpeechUntilTick
        ? (g_state.activeSpeechUntilTick - now)
        : 0;

    const PlayerCharacter* player = GetPlayer();
    SpeakerSnapshot lastNpcSnapshot = g_state.lastNpcSpeaker;
    bool lastNpcResolved = false;
    if (TESObjectREFR* resolvedLastNpc = ResolveSpeakerRef(g_state.lastNpcSpeaker))
    {
        lastNpcSnapshot = CaptureSpeakerSnapshot(resolvedLastNpc);
        lastNpcResolved = true;
    }

    double playerToLastNpcDistanceMeters = -1.0;
    if (player && lastNpcSnapshot.valid)
    {
        const double dx = static_cast<double>(player->posX) - static_cast<double>(lastNpcSnapshot.posX);
        const double dy = static_cast<double>(player->posY) - static_cast<double>(lastNpcSnapshot.posY);
        const double dz = static_cast<double>(player->posZ) - static_cast<double>(lastNpcSnapshot.posZ);
        playerToLastNpcDistanceMeters = std::sqrt(dx * dx + dy * dy + dz * dz) / kGameUnitsPerMeter;
    }

    std::ostringstream payload;
    payload << "{\n";
    payload << "  \"updated_at\": " << JsonEscape(NowIsoUtc()) << ",\n";
    payload << "  \"frame\": " << g_state.runtimeHeartbeatFrame << ",\n";
    payload << "  \"trace_request_id\": " << JsonEscape(g_state.traceRequestId) << ",\n";
    payload << "  \"active_request_id\": " << JsonEscape(g_state.activeRequestId) << ",\n";
    payload << "  \"awaiting_input\": " << (g_state.awaitingInput ? "true" : "false") << ",\n";
    payload << "  \"bridge_text_input_owned\": " << (g_state.bridgeTextInputOwned ? "true" : "false") << ",\n";
    payload << "  \"awaiting_reply\": " << (g_state.awaitingReply ? "true" : "false") << ",\n";
    payload << "  \"game_window_has_focus\": " << (GameWindowHasFocus() ? "true" : "false") << ",\n";
    payload << "  \"pending_audio_chunks\": " << g_state.pendingAudioChunks.size() << ",\n";
    payload << "  \"active_sounds\": " << g_state.activeSounds.size() << ",\n";
    payload << "  \"streamed_audio_seen_for_reply\": " << (g_state.streamedAudioSeenForReply ? "true" : "false") << ",\n";
    payload << "  \"last_audio_chunk_index\": " << g_state.lastAudioChunkIndex << ",\n";
    payload << "  \"speech_remaining_ms\": " << speechRemainingMs << ",\n";
    payload << "  \"player\": {\n";
    payload << "    \"present\": " << (player ? "true" : "false") << ",\n";
    payload << "    \"ref_id\": " << (player ? player->refID : 0) << ",\n";
    payload << "    \"pos_x\": " << (player ? player->posX : 0.0f) << ",\n";
    payload << "    \"pos_y\": " << (player ? player->posY : 0.0f) << ",\n";
    payload << "    \"pos_z\": " << (player ? player->posZ : 0.0f) << "\n";
    payload << "  },\n";
    payload << "  \"last_npc\": {\n";
    payload << "    \"npc_key\": " << JsonEscape(g_state.lastNpcKey) << ",\n";
    payload << "    \"npc_name\": " << JsonEscape(g_state.lastNpcName) << ",\n";
    payload << "    \"ref_id\": " << lastNpcSnapshot.refId << ",\n";
    payload << "    \"snapshot_valid\": " << (lastNpcSnapshot.valid ? "true" : "false") << ",\n";
    payload << "    \"resolved\": " << (lastNpcResolved ? "true" : "false") << ",\n";
    payload << "    \"pos_x\": " << lastNpcSnapshot.posX << ",\n";
    payload << "    \"pos_y\": " << lastNpcSnapshot.posY << ",\n";
    payload << "    \"pos_z\": " << lastNpcSnapshot.posZ << ",\n";
    payload << "    \"distance_to_player_m\": " << playerToLastNpcDistanceMeters << "\n";
    payload << "  },\n";
    payload << "  \"speech_animation\": {\n";
    payload << "    \"active\": " << (g_state.speechAnimation.active ? "true" : "false") << ",\n";
    payload << "    \"request_id\": " << JsonEscape(g_state.speechAnimation.requestId) << ",\n";
    payload << "    \"speaker_ref_id\": " << g_state.speechAnimation.speaker.refId << ",\n";
    payload << "    \"duration_ms\": " << g_state.speechAnimation.durationMs << ",\n";
    payload << "    \"binding_resolved\": " << (g_state.speechAnimation.binding.phonemeKeyframe ? "true" : "false") << "\n";
    payload << "  },\n";
    payload << "  \"conversation_hold\": {\n";
    payload << "    \"active\": " << (g_state.conversationHold.active ? "true" : "false") << ",\n";
    payload << "    \"speaker_ref_id\": " << g_state.conversationHold.speaker.refId << ",\n";
    payload << "    \"release_tick\": " << g_state.conversationHold.releaseTick << ",\n";
    payload << "    \"script_package_applied\": " << (g_state.conversationHold.scriptPackageApplied ? "true" : "false") << "\n";
    payload << "  },\n";
    payload << "  \"voice_capture\": {\n";
    payload << "    \"active\": " << (g_state.voiceCapture.active ? "true" : "false") << ",\n";
    payload << "    \"transcribing\": " << (g_state.voiceCapture.transcribing ? "true" : "false") << ",\n";
    payload << "    \"admin_mode\": " << (g_state.voiceCapture.adminMode ? "true" : "false") << ",\n";
    payload << "    \"key_down_last_frame\": " << (g_state.voiceCapture.keyDownLastFrame ? "true" : "false") << ",\n";
    payload << "    \"admin_key_down_last_frame\": " << (g_state.voiceCapture.adminKeyDownLastFrame ? "true" : "false") << ",\n";
    payload << "    \"started_tick\": " << g_state.voiceCapture.startedTick << ",\n";
    payload << "    \"subtitle_refresh_tick\": " << g_state.voiceCapture.subtitleRefreshTick << ",\n";
    payload << "    \"captured_pcm_bytes\": " << g_state.voiceCapture.capturedPcm.size() << ",\n";
    payload << "    \"npc_key\": " << JsonEscape(g_state.voiceCapture.npcKey) << ",\n";
    payload << "    \"npc_name\": " << JsonEscape(g_state.voiceCapture.npcName) << ",\n";
    payload << "    \"speaker_ref_id\": " << g_state.voiceCapture.speaker.refId << "\n";
    payload << "  },\n";
    payload << "  \"direct_sound\": {\n";
    payload << "    \"device\": " << (g_state.directSound ? "true" : "false") << ",\n";
    payload << "    \"primary_buffer\": " << (g_state.primaryBuffer ? "true" : "false") << ",\n";
    payload << "    \"listener\": " << (g_state.listener3d ? "true" : "false") << "\n";
    payload << "  },\n";
    payload << "  \"debug_config\": {\n";
    payload << "    \"runtime_heartbeat\": " << (g_debugConfig.runtimeHeartbeatEnabled ? "true" : "false") << ",\n";
    payload << "    \"speech_animation\": " << (g_debugConfig.speechAnimationEnabled ? "true" : "false") << ",\n";
    payload << "    \"speech_write_phoneme_values\": " << (g_debugConfig.speechWritePhonemeValues ? "true" : "false") << ",\n";
    payload << "    \"speech_write_face_flags\": " << (g_debugConfig.speechWriteFaceFlags ? "true" : "false") << ",\n";
    payload << "    \"speech_clear_binding_on_stop\": " << (g_debugConfig.speechClearBindingOnStop ? "true" : "false") << ",\n";
    payload << "    \"subtitles\": " << (g_debugConfig.subtitlesEnabled ? "true" : "false") << ",\n";
    payload << "    \"listener_updates\": " << (g_debugConfig.listenerUpdatesEnabled ? "true" : "false") << ",\n";
    payload << "    \"directsound_3d\": " << (g_debugConfig.directSound3dEnabled ? "true" : "false") << ",\n";
    payload << "    \"directsound_software_buffer\": " << (g_debugConfig.directSoundSoftwareBufferEnabled ? "true" : "false") << ",\n";
    payload << "    \"drain_queued_chunks_after_final\": " << (g_debugConfig.drainQueuedChunksAfterFinal ? "true" : "false") << ",\n";
    payload << "    \"speech_weight_scale\": " << std::fixed << std::setprecision(2) << g_debugConfig.speechWeightScale << ",\n";
    payload << "    \"runtime_heartbeat_interval_ms\": " << g_debugConfig.runtimeHeartbeatIntervalMs << ",\n";
    payload << "    \"streaming_chunk_overlap_ms\": " << g_debugConfig.streamingChunkOverlapMs << ",\n";
    payload << "    \"caption_ms_per_char\": " << g_debugConfig.captionMsPerChar << ",\n";
    payload << "    \"bridge_root_path\": " << JsonEscape(g_debugConfig.bridgeRootPath) << "\n";
    payload << "  },\n";
    payload << "  \"last_playback_diagnostics\": " << JsonEscape(g_state.lastPlaybackDiagnostics) << "\n";
    payload << "}\n";

    const std::string snapshot = payload.str();
    out << snapshot;

    std::error_code ec;
    if (fs::exists(RuntimeHeartbeatHistoryPath(), ec) && fs::file_size(RuntimeHeartbeatHistoryPath(), ec) > kRuntimeHeartbeatHistoryMaxBytes)
    {
        fs::remove(RuntimeHeartbeatHistoryPath(), ec);
    }

    std::ofstream history(RuntimeHeartbeatHistoryPath(), std::ios::binary | std::ios::app);
    if (history)
    {
        std::string compact = snapshot;
        compact.erase(std::remove(compact.begin(), compact.end(), '\r'), compact.end());
        std::replace(compact.begin(), compact.end(), '\n', ' ');
        history << compact << "\n";
    }
}

fs::path ReplaceExtension(const fs::path& path, std::string_view extension)
{
    fs::path result = path;
    result.replace_extension(extension);
    return result;
}

std::string GetFormNameSafe(TESForm* form);
std::string GetStringValueSafe(String& value);

std::string FormIdHex(UInt32 formId)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << formId;
    return out.str();
}

std::string ModLocalFormCacheKey(const char* modName, UInt32 localFormId)
{
    std::ostringstream out;
    out << (modName ? modName : "")
        << ':'
        << FormIdHex(localFormId);
    return out.str();
}

TESForm* ResolveModLocalForm(const char* modName, UInt32 localFormId)
{
    if (!modName || !*modName)
    {
        return nullptr;
    }

    const DWORD now = GetTickCount();
    ModLocalFormCacheEntry& cache = g_modLocalFormCache[ModLocalFormCacheKey(modName, localFormId)];
    if (cache.form)
    {
        return cache.form;
    }
    if (cache.nextRetryTick && now < cache.nextRetryTick)
    {
        return nullptr;
    }

    DataHandler* dataHandler = GetDataHandler();
    if (!dataHandler)
    {
        if (!cache.formMissingLogged)
        {
            LogLine("DataHandler unavailable while resolving %s:%s.", modName, FormIdHex(localFormId).c_str());
            cache.formMissingLogged = true;
        }
        cache.nextRetryTick = now + kModLocalFormRetryMs;
        return nullptr;
    }

    UInt8 modIndex = 0xFF;
    for (UInt32 i = 0; i < dataHandler->modList.loadedModCount; ++i)
    {
        ModInfo* modInfo = dataHandler->modList.loadedMods[i];
        if (modInfo && _stricmp(modInfo->name, modName) == 0)
        {
            modIndex = modInfo->modIndex;
            break;
        }
    }

    if (modIndex == 0xFF)
    {
        if (!cache.modMissingLogged)
        {
            LogLine("Mod %s is not loaded; cannot resolve local form %s.", modName, FormIdHex(localFormId).c_str());
            cache.modMissingLogged = true;
        }
        cache.nextRetryTick = now + kModLocalFormRetryMs;
        return nullptr;
    }

    const UInt32 runtimeFormId = (static_cast<UInt32>(modIndex) << 24) | (localFormId & 0x00FFFFFF);
    TESForm* form = LookupFormByIdRuntime(runtimeFormId);
    if (!form)
    {
        if (!cache.formMissingLogged)
        {
            LogLine("Failed to resolve runtime form %08X for %s:%s.", runtimeFormId, modName, FormIdHex(localFormId).c_str());
            cache.formMissingLogged = true;
        }
        cache.modIndex = modIndex;
        cache.runtimeFormId = runtimeFormId;
        cache.nextRetryTick = now + kModLocalFormRetryMs;
        return nullptr;
    }

    cache.form = form;
    cache.modIndex = modIndex;
    cache.runtimeFormId = runtimeFormId;
    cache.nextRetryTick = 0;
    cache.modMissingLogged = false;
    cache.formMissingLogged = false;
    return form;
}

std::string Slugify(std::string_view value)
{
    std::string result;
    bool lastUnderscore = false;
    for (unsigned char ch : std::string(value))
    {
        if (std::isalnum(ch))
        {
            result.push_back(static_cast<char>(std::tolower(ch)));
            lastUnderscore = false;
            continue;
        }

        if (!lastUnderscore)
        {
            result.push_back('_');
            lastUnderscore = true;
        }
    }

    while (!result.empty() && result.front() == '_')
    {
        result.erase(result.begin());
    }
    while (!result.empty() && result.back() == '_')
    {
        result.pop_back();
    }
    return result;
}

bool StartsWithInsensitive(std::string_view value, std::string_view prefix)
{
    if (value.size() < prefix.size())
    {
        return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
            return false;
        }
    }

    return true;
}

std::string RemoveDuplicateSuffix(std::string value)
{
    const std::string marker = "DUPLICATE";
    const size_t pos = value.find(marker);
    if (pos != std::string::npos)
    {
        value.erase(pos);
    }
    while (!value.empty() && std::isdigit(static_cast<unsigned char>(value.back())))
    {
        value.pop_back();
    }
    return value;
}

std::string HumanizeIdentifier(std::string value)
{
    value = RemoveDuplicateSuffix(value);
    value = ReplaceAll(value, '_', ' ');

    std::string out;
    out.reserve(value.size() + 8);
    for (size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (i > 0)
        {
            const unsigned char prev = static_cast<unsigned char>(value[i - 1]);
            if (std::isupper(ch) && (std::islower(prev) || std::isdigit(prev)))
            {
                out.push_back(' ');
            }
        }
        out.push_back(static_cast<char>(ch));
    }

    out = Trim(out);
    if (StartsWithInsensitive(out, "GS ") || StartsWithInsensitive(out, "GS"))
    {
        out.erase(0, 2);
        out = Trim(out);
    }
    if (out.size() >= 8 && StartsWithInsensitive(out, "Interior"))
    {
        out.erase(0, 8);
        out = Trim(out);
    }
    if (out.size() >= 8 && out.size() >= 8 && StartsWithInsensitive(out.substr(out.size() - 8), "Interior"))
    {
        out.erase(out.size() - 8);
        out = Trim(out);
    }

    return out;
}

std::string InferMajorLocationFromCellIdentifier(const std::string& rawCell)
{
    const std::string slug = Slugify(rawCell);
    if (StartsWithInsensitive(rawCell, "GS")
        || slug.find("goodsprings") != std::string::npos
        || slug.find("prospector_saloon") != std::string::npos
        || slug.find("prospectorsaloon") != std::string::npos
        || slug.find("doc_mitchell") != std::string::npos
        || slug.find("docmitchell") != std::string::npos
        || slug.find("general_store") != std::string::npos
        || slug.find("generalstore") != std::string::npos
        || slug.find("schoolhouse") != std::string::npos
        || slug.find("goodsprings_source") != std::string::npos
        || slug.find("cemetery") != std::string::npos)
    {
        return "Goodsprings";
    }
    return "";
}

std::string InferMinorLocationFromCellIdentifier(const std::string& rawCell)
{
    const std::string slug = Slugify(rawCell);
    if (slug.find("prospector_saloon") != std::string::npos || slug.find("prospectorsaloon") != std::string::npos)
    {
        return "Prospector Saloon";
    }
    if (slug.find("doc_mitchell") != std::string::npos || slug.find("docmitchell") != std::string::npos)
    {
        return "Doc Mitchell's House";
    }
    if (slug.find("general_store") != std::string::npos || slug.find("generalstore") != std::string::npos)
    {
        return "Chet's General Store";
    }
    if (slug.find("schoolhouse") != std::string::npos)
    {
        return "Goodsprings Schoolhouse";
    }

    const std::string humanized = HumanizeIdentifier(rawCell);
    if (!humanized.empty() && Slugify(humanized) != "wilderness")
    {
        return humanized;
    }
    return "";
}

UInt32 MakeWorldCellKey(SInt32 x, SInt32 y)
{
    return (static_cast<UInt32>(x) << 16) + ((static_cast<UInt32>(y) << 16) >> 16);
}

std::optional<std::pair<SInt32, SInt32>> GetWorldCellCoordinates(const TESObjectCELL* cell)
{
    if (!cell || !cell->cellData)
    {
        return std::nullopt;
    }

    return std::make_pair(
        static_cast<SInt32>(cell->cellData->x),
        static_cast<SInt32>(cell->cellData->y));
}

std::string GetLoadDoorDestinationName(TESObjectREFR* ref)
{
    if (!ref || !ref->baseForm || ref->baseForm->typeID != kFormType_TESObjectDOOR)
    {
        return "";
    }

    const std::string sourceDoorName = GetFormNameSafe(ref->baseForm);
    if (!sourceDoorName.empty())
    {
        return sourceDoorName;
    }

    BSExtraData* extra = ref->extraDataList.GetByType(kExtraData_Teleport);
    ExtraTeleport* teleport = extra ? reinterpret_cast<ExtraTeleport*>(extra) : nullptr;
    if (!teleport || !teleport->data || !teleport->data->linkedDoor)
    {
        return "";
    }

    TESObjectREFR* linkedDoor = teleport->data->linkedDoor;
    const std::string linkedDoorName = GetFormNameSafe(linkedDoor->baseForm);
    if (!linkedDoorName.empty())
    {
        return linkedDoorName;
    }

    if (linkedDoor->parentCell)
    {
        const std::string cellName = GetFormNameSafe(linkedDoor->parentCell);
        if (!cellName.empty())
        {
            return cellName;
        }
    }

    return GetFormNameSafe(linkedDoor);
}

std::string GetNaturalMinorLocationName(TESObjectREFR* ref)
{
    const std::string doorDestination = GetLoadDoorDestinationName(ref);
    if (!doorDestination.empty())
    {
        return doorDestination;
    }

    return GetFormNameSafe(ref);
}

HWND GetGameWindow()
{
    auto globals = reinterpret_cast<OSGlobals**>(kOSGlobalsAddress);
    if (globals && *globals)
    {
        return (*globals)->window;
    }
    return nullptr;
}

bool GameWindowHasFocus()
{
    HWND hwnd = GetGameWindow();
    if (!hwnd)
    {
        return false;
    }

    HWND foreground = GetForegroundWindow();
    if (!foreground)
    {
        return false;
    }

    if (foreground == hwnd)
    {
        return true;
    }

    DWORD gamePid = 0;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(hwnd, &gamePid);
    GetWindowThreadProcessId(foreground, &foregroundPid);
    return gamePid != 0 && gamePid == foregroundPid;
}

TESObjectREFR* GetCrosshairRef()
{
    auto* ui = *reinterpret_cast<InterfaceManager**>(kInterfaceManagerSingletonAddress);
    return ui ? ui->crosshairRef : nullptr;
}

PlayerCharacter* GetPlayer()
{
    return *reinterpret_cast<PlayerCharacter**>(kPlayerSingletonAddress);
}

DataHandler* GetDataHandler()
{
    return *reinterpret_cast<DataHandler**>(kDataHandlerSingletonAddress);
}

bool IsMenuVisible(UInt32 menuType)
{
    if (menuType < kMenuType_Min || menuType > kMenuType_Max)
    {
        return false;
    }

    auto* menuVisibility = reinterpret_cast<UInt8*>(kMenuVisibilityArrayAddress);
    return menuVisibility[menuType] != 0;
}

bool IsMenuAllocated(UInt32 menuType)
{
    auto* menuArray = reinterpret_cast<NiTArray<TileMenu*>*>(kTileMenuArrayAddress);
    if (!menuArray || menuType < kMenuType_Min || menuType > kMenuType_Max)
    {
        return false;
    }

    return menuArray->Get(menuType - kMenuType_Min) != nullptr;
}

bool IsTextInputMenuActive()
{
    return IsMenuVisible(kMenuType_TextEdit) || IsMenuAllocated(kMenuType_TextEdit);
}

std::optional<std::pair<std::string, std::string>> ResolveMappedNpcImpl(TESObjectREFR* ref, bool logUnmapped)
{
    if (!ref)
    {
        return std::nullopt;
    }

    const auto makeMatch = [](std::string_view key, std::string_view display) {
        return std::make_pair(std::string(key), std::string(display));
    };
    const auto makeRefScopedMatch = [ref](std::string_view baseKey, std::string_view fallbackDisplay) {
        std::ostringstream key;
        key << std::string(baseKey);
        if (ref)
        {
            key << "__ref_" << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << ref->refID;
        }

        std::string display = std::string(fallbackDisplay);
        if (ref)
        {
            if (char* refName = ref->GetName())
            {
                const std::string trimmed = Trim(refName);
                if (!trimmed.empty())
                {
                    display = trimmed;
                }
            }
            else if (ref->baseForm && ref->baseForm != ref)
            {
                if (char* baseName = ref->baseForm->GetName())
                {
                    const std::string trimmed = Trim(baseName);
                    if (!trimmed.empty())
                    {
                        display = trimmed;
                    }
                }
            }
        }

        return std::make_pair(key.str(), display);
    };
    const UInt32 refLocalFormId = ref
        ? (ref->refID & 0x00FFFFFF)
        : 0;
    const UInt32 baseLocalFormId = (ref && ref->baseForm)
        ? (ref->baseForm->refID & 0x00FFFFFF)
        : 0;
    TESNPC* baseNpc = (ref && ref->baseForm && ref->baseForm->typeID == kFormType_TESNPC)
        ? static_cast<TESNPC*>(ref->baseForm)
        : nullptr;
    const UInt32 templateLocalFormId = (baseNpc && baseNpc->copyFrom)
        ? (baseNpc->copyFrom->refID & 0x00FFFFFF)
        : 0;
    const UInt32 cellLocalFormId = (ref && ref->parentCell)
        ? (ref->parentCell->refID & 0x00FFFFFF)
        : 0;
    const std::string cellNameSlug = Slugify(GetFormNameSafe(ref ? ref->parentCell : nullptr));
    const auto resolveVoiceTypeSlug = [baseNpc]() {
        if (!baseNpc)
        {
            return std::string();
        }

        BGSVoiceType* voiceType = baseNpc->baseData.GetVoiceType();
        if (!voiceType)
        {
            voiceType = baseNpc->baseData.voiceType;
        }

        return Slugify(GetFormNameSafe(voiceType));
    };
    const std::string voiceTypeSlug = resolveVoiceTypeSlug();
    const auto makePowderGangerVariantMatch = [&makeRefScopedMatch, &voiceTypeSlug]() {
        std::string baseKey = "powder_ganger";
        if (!voiceTypeSlug.empty())
        {
            baseKey += "_";
            baseKey += voiceTypeSlug;
        }
        return makeRefScopedMatch(baseKey, "Powder Ganger");
    };
    const auto matchesAnyLocalFormId = [refLocalFormId, baseLocalFormId, templateLocalFormId](std::initializer_list<UInt32> formIds) {
        for (const UInt32 formId : formIds)
        {
            if (formId == 0)
            {
                continue;
            }

            if (refLocalFormId == formId || baseLocalFormId == formId || templateLocalFormId == formId)
            {
                return true;
            }
        }

        return false;
    };
    const auto endsWith = [](const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size()
            && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    const auto isLikelyEditorRefName = [&endsWith](const std::string& value) {
        const std::string trimmed = Trim(value);
        if (trimmed.empty())
        {
            return false;
        }

        if (trimmed.find_first_of(" \t\r\n") != std::string::npos)
        {
            return false;
        }

        const std::string slug = Slugify(trimmed);
        if (slug.empty())
        {
            return false;
        }

        return endsWith(slug, "ref")
            || endsWith(slug, "marker")
            || endsWith(slug, "trigger")
            || endsWith(slug, "template");
    };

    std::vector<std::string> candidates;
    if (char* name = ref->GetName())
    {
        const std::string trimmed = Trim(name);
        if (!isLikelyEditorRefName(trimmed))
        {
            candidates.push_back(Slugify(trimmed));
        }
    }
    if (ref->baseForm && ref->baseForm != ref)
    {
        if (char* name = ref->baseForm->GetName())
        {
            candidates.push_back(Slugify(name));
        }
    }
    if (baseNpc && baseNpc->copyFrom)
    {
        if (char* templateName = baseNpc->copyFrom->GetName())
        {
            candidates.push_back(Slugify(templateName));
        }
    }
    const auto resolveVisibleName = [ref, baseNpc, &isLikelyEditorRefName]() -> std::string {
        if (ref)
        {
            if (char* refName = ref->GetName())
            {
                const std::string trimmed = Trim(refName);
                if (!trimmed.empty() && !isLikelyEditorRefName(trimmed))
                {
                    return trimmed;
                }
            }
        }

        if (ref && ref->baseForm && ref->baseForm != ref)
        {
            if (char* baseName = ref->baseForm->GetName())
            {
                const std::string trimmed = Trim(baseName);
                if (!trimmed.empty())
                {
                    return trimmed;
                }
            }
        }

        if (baseNpc && baseNpc->copyFrom)
        {
            if (char* templateName = baseNpc->copyFrom->GetName())
            {
                const std::string trimmed = Trim(templateName);
                if (!trimmed.empty())
                {
                    return trimmed;
                }
            }
        }

        return std::string();
    };
    const std::string visibleName = resolveVisibleName();
    const std::string visibleNameSlug = Slugify(visibleName);

    const auto containsAny = [&candidates](std::initializer_list<std::string_view> needles) {
        for (const auto& candidate : candidates)
        {
            for (const auto needle : needles)
            {
                if (candidate.find(needle) != std::string::npos)
                {
                    return true;
                }
            }
        }
        return false;
    };

    const auto isFemaleNpc = [ref]() -> bool {
        if (!ref || !ref->baseForm || ref->baseForm->typeID != kFormType_TESNPC)
        {
            return false;
        }

        auto* npc = static_cast<TESNPC*>(ref->baseForm);
        return npc && npc->baseData.IsFemale();
    };
    const auto hasPowderGangerFaction = [baseNpc]() -> bool {
        if (!baseNpc)
        {
            return false;
        }

        for (tList<TESActorBaseData::FactionListData>::Iterator iter = baseNpc->baseData.factionList.Begin(); !iter.End(); ++iter)
        {
            TESActorBaseData::FactionListData* data = iter.Get();
            if (!data || !data->faction)
            {
                continue;
            }

            const std::string factionName = Slugify(GetFormNameSafe(data->faction));
            if (factionName.find("powder_ganger") != std::string::npos
                || factionName.find("powdergang") != std::string::npos
                || factionName.find("convict") != std::string::npos)
            {
                return true;
            }
        }

        return false;
    };
    const auto isAnonymousPrisonNpc = [ref, cellLocalFormId, cellNameSlug, voiceTypeSlug]() -> bool {
        if (!ref || !ref->parentCell || !ref->baseForm || ref->baseForm->typeID != kFormType_TESNPC)
        {
            return false;
        }

        const bool hasVisibleName = (ref->GetName() && *ref->GetName())
            || (ref->baseForm->GetName() && *ref->baseForm->GetName());
        if (hasVisibleName)
        {
            return false;
        }

        if (cellLocalFormId == 0x0008D0D5)
        {
            return true;
        }

        const bool prisonCellByName = cellNameSlug.find("ncrprison") != std::string::npos
            || cellNameSlug.find("visitors_center") != std::string::npos
            || cellNameSlug.find("ncr_correctional_facility") != std::string::npos
            || cellNameSlug.find("correctional_facility") != std::string::npos;
        if (!prisonCellByName)
        {
            return false;
        }

        return voiceTypeSlug.find("powder") != std::string::npos
            || voiceTypeSlug.find("convict") != std::string::npos
            || voiceTypeSlug.find("raider") != std::string::npos
            || voiceTypeSlug.empty();
    };

    switch (baseLocalFormId)
    {
    case 0x0008D371: // Powder Ganger (melee)
    case 0x0008D372: // Powder Ganger (guns)
    case 0x0008F115: // Powder Ganger NCR CF (guns)
    case 0x00090B87: // Powder Ganger NCR CF (melee)
    case 0x0015F310: // Powder Ganger (Goodsprings)
    case 0x000A5AD5: // Powder Ganger bodyguard 01
    case 0x000A5AD8: // Powder Ganger bodyguard 02
    case 0x000E3696: // Powder Ganger bodyguard 03
    case 0x00000963: // Anonymous NCRCF Powder Ganger variant
    case 0x00001097: // Anonymous NCRCF Powder Ganger variant
        return makePowderGangerVariantMatch();
    default:
        break;
    }
    switch (templateLocalFormId)
    {
    case 0x0008D371: // Powder Ganger (melee)
    case 0x0008D372: // Powder Ganger (guns)
    case 0x0008F115: // Powder Ganger NCR CF (guns)
    case 0x00090B87: // Powder Ganger NCR CF (melee)
    case 0x0015F310: // Powder Ganger (Goodsprings)
    case 0x000A5AD5: // Powder Ganger bodyguard 01
    case 0x000A5AD8: // Powder Ganger bodyguard 02
    case 0x000E3696: // Powder Ganger bodyguard 03
    case 0x00000963: // Anonymous NCRCF Powder Ganger variant
    case 0x00001097: // Anonymous NCRCF Powder Ganger variant
        return makePowderGangerVariantMatch();
    default:
        break;
    }

    // Named NCRCF actors must resolve by exact ID, otherwise nearby generic convicts
    // can be misclassified as Eddie or other prison uniques via loose name heuristics.
    if (matchesAnyLocalFormId({ 0x000D7036, 0x0008D0E9 })) return makeMatch("eddie", "Eddie");
    if (matchesAnyLocalFormId({ 0x000D6F51, 0x0008F13A })) return makeMatch("dawes", "Dawes");
    if (matchesAnyLocalFormId({ 0x000D71B7, 0x000D71B6 })) return makeMatch("hannigan", "Hannigan");
    if (matchesAnyLocalFormId({ 0x000CEF3C, 0x000CEF3B })) return makeMatch("carter", "Carter");
    if (matchesAnyLocalFormId({ 0x0008D501, 0x0008D0E7 })) return makeMatch("meyers", "Meyers");
    if (matchesAnyLocalFormId({ 0x000D7037, 0x0008D0EB })) return makeMatch("scrambler", "Scrambler");
    if (matchesAnyLocalFormId({ 0x000E32A3, 0x000E32A2 })) return makeMatch("chavez", "Chavez");

    if (containsAny({ "easy_pete", "easypete" })) return makeMatch("easy_pete", "Easy Pete");
    if (containsAny({ "sunny_smiles", "sunnysmiles" })) return makeMatch("sunny_smiles", "Sunny Smiles");
    if (containsAny({ "doc_mitchell", "docmitchell", "mitchell" })) return makeMatch("doc_mitchell", "Doc Mitchell");
    if (containsAny({ "trudy" })) return makeMatch("trudy", "Trudy");
    if (containsAny({ "chet" })) return makeMatch("chet", "Chet");
    if (containsAny({ "victor" })) return makeMatch("victor", "Victor");
    if (containsAny({ "ringo" })) return makeMatch("ringo", "Ringo");
    if (containsAny({ "cheyenne" })) return makeMatch("cheyenne", "Cheyenne");
    if (containsAny({ "goodsprings_settler", "goodspringssettler" })) return makeRefScopedMatch(isFemaleNpc() ? "goodsprings_settler_female" : "goodsprings_settler_male", "Goodsprings Settler");
    if (containsAny({ "powder_ganger", "powderganger", "powder_gangers", "powdergangers", "powder_ganger_bodyguard", "powdergangerbodyguard", "escaped_convict", "escapedconvict", "convict" })) return makePowderGangerVariantMatch();
    if (hasPowderGangerFaction()) return makePowderGangerVariantMatch();
    if (isAnonymousPrisonNpc()) return makePowderGangerVariantMatch();
    if (baseNpc && !visibleNameSlug.empty()) return makeMatch(visibleNameSlug, visibleName);

    if (logUnmapped && !candidates.empty())
    {
        LogLine("Unmapped target: ref=%08X base=%08X name=%s",
            ref->refID,
            ref->baseForm ? ref->baseForm->refID : 0,
            candidates.front().c_str());
    }

    return std::nullopt;
}

void ConsiderNearestMappedNpc(PlayerCharacter* player, TESObjectREFR* ref, double maxDistanceSquared, bool underCrosshair, ResolvedNpcTarget& best)
{
    if (!player || !ref || ref == player)
    {
        return;
    }

    if (!ref->baseForm)
    {
        return;
    }

    const UInt8 baseType = ref->baseForm->typeID;
    if (baseType != kFormType_TESNPC && baseType != kFormType_TESCreature)
    {
        return;
    }

    if (!IsLiveNearbyActorRef(player, ref))
    {
        return;
    }

    const auto resolved = ResolveMappedNpcImpl(ref, false);
    if (!resolved.has_value())
    {
        return;
    }

    const double distanceSquared = DistanceSquared3D(player, ref);
    if (distanceSquared > maxDistanceSquared)
    {
        return;
    }

    if (best.ref)
    {
        if (underCrosshair && !best.underCrosshair)
        {
            // Keep the actor under the crosshair as the direct-talk target.
        }
        else if (!underCrosshair && best.underCrosshair)
        {
            return;
        }
        else if (distanceSquared >= best.distanceSquared)
        {
            return;
        }
    }

    best.ref = ref;
    best.npcKey = resolved->first;
    best.npcName = resolved->second;
    const auto voiceType = ResolveRefVoiceTypeMetadata(ref);
    best.voiceTypeKey = voiceType.first;
    best.voiceTypeName = voiceType.second;
    best.distanceSquared = distanceSquared;
    best.underCrosshair = underCrosshair;
}

bool IsLiveNearbyActorRef(const TESObjectREFR* anchorRef, TESObjectREFR* ref)
{
    if (!anchorRef || !ref || ref == anchorRef)
    {
        return false;
    }

    if (ref->IsDeleted())
    {
        return false;
    }

    if (!ref->GetInSameCellOrWorld(const_cast<TESObjectREFR*>(anchorRef)))
    {
        return false;
    }

    // The cell object list can retain actor references that are not actually loaded into
    // the active scene. Nearby chat should only consider actors that currently have live 3D.
    if (!ref->GetNiNode())
    {
        return false;
    }

    const UInt8 baseType = ref->baseForm ? ref->baseForm->typeID : 0;
    if (baseType == kFormType_TESNPC || baseType == kFormType_TESCreature)
    {
        const auto* mobile = static_cast<const MobileObject*>(ref);
        if (!mobile->baseProcess)
        {
            return false;
        }
    }

    return true;
}

void CollectNearbyMappedNpcAround(const TESObjectREFR* anchorRef, TESObjectREFR* ref, double maxDistanceSquared, bool underCrosshair, std::vector<NearbyNpcCandidate>& candidates)
{
    if (!anchorRef || !ref || ref == anchorRef)
    {
        return;
    }

    if (!ref->baseForm)
    {
        return;
    }

    const UInt8 baseType = ref->baseForm->typeID;
    if (baseType != kFormType_TESNPC && baseType != kFormType_TESCreature)
    {
        return;
    }

    if (!IsLiveNearbyActorRef(anchorRef, ref))
    {
        return;
    }

    const double distanceSquared = DistanceSquared3D(anchorRef, ref);
    if (distanceSquared > maxDistanceSquared)
    {
        return;
    }

    const auto resolved = ResolveMappedNpcImpl(ref, false);
    if (!resolved.has_value())
    {
        TESNPC* baseNpc = (ref->baseForm && ref->baseForm->typeID == kFormType_TESNPC)
            ? static_cast<TESNPC*>(ref->baseForm)
            : nullptr;
        const UInt32 templateRefId = (baseNpc && baseNpc->copyFrom) ? baseNpc->copyFrom->refID : 0;
        BGSVoiceType* voiceType = baseNpc ? baseNpc->baseData.GetVoiceType() : nullptr;
        if (!voiceType && baseNpc)
        {
            voiceType = baseNpc->baseData.voiceType;
        }
        const char* refName = ref->GetName();
        const char* baseName = (ref->baseForm && ref->baseForm != ref) ? ref->baseForm->GetName() : nullptr;
        LogLine("Nearby unmapped actor: ref=%08X base=%08X template=%08X cell=%08X dist=%.2fm voice=%s name=%s base_name=%s",
            ref->refID,
            ref->baseForm ? ref->baseForm->refID : 0,
            templateRefId,
            ref->parentCell ? ref->parentCell->refID : 0,
            std::sqrt(distanceSquared) / kGameUnitsPerMeter,
            voiceType ? GetFormNameSafe(voiceType).c_str() : "",
            refName ? refName : "",
            baseName ? baseName : "");
        return;
    }

    auto existing = std::find_if(candidates.begin(), candidates.end(), [&](const NearbyNpcCandidate& candidate) {
        return candidate.ref == ref || (candidate.ref && ref && candidate.ref->refID == ref->refID);
        });
    if (existing != candidates.end())
    {
        existing->underCrosshair = existing->underCrosshair || underCrosshair;
        if (distanceSquared < existing->distanceSquared)
        {
            existing->distanceSquared = distanceSquared;
            existing->npcKey = resolved->first;
            existing->npcName = resolved->second;
            const auto voiceType = ResolveRefVoiceTypeMetadata(ref);
            existing->voiceTypeKey = voiceType.first;
            existing->voiceTypeName = voiceType.second;
        }
        return;
    }

    const auto voiceType = ResolveRefVoiceTypeMetadata(ref);
    candidates.push_back({
        ref,
        resolved->first,
        resolved->second,
        voiceType.first,
        voiceType.second,
        distanceSquared,
        underCrosshair,
        });
}

std::vector<NearbyNpcCandidate> FindNearbyMappedNpcsAround(const TESObjectREFR* anchorRef, float maxDistanceMeters)
{
    std::vector<NearbyNpcCandidate> candidates;
    if (!anchorRef)
    {
        return candidates;
    }

    const TESObjectCELL* anchorCell = anchorRef->parentCell;
    if (!anchorCell)
    {
        return candidates;
    }

    const double maxDistanceSquared = std::pow(static_cast<double>(maxDistanceMeters * kGameUnitsPerMeter), 2.0);
    TESObjectREFR* crosshairTarget = GetCrosshairRef();
    if (crosshairTarget)
    {
        CollectNearbyMappedNpcAround(anchorRef, crosshairTarget, maxDistanceSquared, true, candidates);
    }

    auto scanCell = [&](TESObjectCELL* cell) {
        if (!cell)
        {
            return;
        }

        for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
        {
            TESObjectREFR* ref = *iter;
            CollectNearbyMappedNpcAround(anchorRef, ref, maxDistanceSquared, ref == crosshairTarget, candidates);
        }
    };

    scanCell(const_cast<TESObjectCELL*>(anchorCell));

    TESWorldSpace* worldSpace = anchorCell->worldSpace;
    const auto anchorCellCoordinates = GetWorldCellCoordinates(anchorCell);
    if (worldSpace && worldSpace->cellMap && anchorCellCoordinates.has_value())
    {
        const SInt32 centerX = anchorCellCoordinates->first;
        const SInt32 centerY = anchorCellCoordinates->second;
        for (SInt32 y = centerY - 1; y <= centerY + 1; ++y)
        {
            for (SInt32 x = centerX - 1; x <= centerX + 1; ++x)
            {
                if (x == centerX && y == centerY)
                {
                    continue;
                }
                scanCell(worldSpace->cellMap->Lookup(MakeWorldCellKey(x, y)));
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const NearbyNpcCandidate& left, const NearbyNpcCandidate& right) {
        if (left.underCrosshair != right.underCrosshair)
        {
            return left.underCrosshair && !right.underCrosshair;
        }
        if (left.distanceSquared != right.distanceSquared)
        {
            return left.distanceSquared < right.distanceSquared;
        }
        return _stricmp(left.npcName.c_str(), right.npcName.c_str()) < 0;
        });

    return candidates;
}

std::vector<NearbyNpcCandidate> FindNearbyMappedNpcsForGroupChat(PlayerCharacter* player, float maxDistanceMeters)
{
    return FindNearbyMappedNpcsAround(player, maxDistanceMeters);
}

void MergeNearbyNpcCandidates(std::vector<NearbyNpcCandidate>& target, const std::vector<NearbyNpcCandidate>& source)
{
    for (const NearbyNpcCandidate& candidate : source)
    {
        auto existing = std::find_if(target.begin(), target.end(), [&](const NearbyNpcCandidate& current) {
            return current.ref == candidate.ref || (current.ref && candidate.ref && current.ref->refID == candidate.ref->refID);
            });

        if (existing == target.end())
        {
            target.push_back(candidate);
            continue;
        }

        existing->underCrosshair = existing->underCrosshair || candidate.underCrosshair;
        if (candidate.distanceSquared < existing->distanceSquared)
        {
            existing->distanceSquared = candidate.distanceSquared;
            existing->npcKey = candidate.npcKey;
            existing->npcName = candidate.npcName;
        }
    }
}

std::string BuildTextRequestMetadata(PlayerCharacter* player, const SpeakerSnapshot* preferredSpeaker = nullptr)
{
    std::vector<NearbyNpcCandidate> nearbyCandidates = FindNearbyMappedNpcsForGroupChat(player, kGamestateNearbyRadiusMeters);
    std::string preferredVoiceTypeKey;
    std::string preferredVoiceTypeName;
    if (preferredSpeaker && preferredSpeaker->valid)
    {
        TESObjectREFR* speakerRef = ResolveSpeakerRef(*preferredSpeaker);
        if (speakerRef)
        {
            const auto preferredVoiceType = ResolveRefVoiceTypeMetadata(speakerRef);
            preferredVoiceTypeKey = preferredVoiceType.first;
            preferredVoiceTypeName = preferredVoiceType.second;
        }
    }

    std::sort(nearbyCandidates.begin(), nearbyCandidates.end(), [](const NearbyNpcCandidate& left, const NearbyNpcCandidate& right) {
        if (left.underCrosshair != right.underCrosshair)
        {
            return left.underCrosshair && !right.underCrosshair;
        }
        if (left.distanceSquared != right.distanceSquared)
        {
            return left.distanceSquared < right.distanceSquared;
        }
        return _stricmp(left.npcName.c_str(), right.npcName.c_str()) < 0;
        });

    if (nearbyCandidates.empty())
    {
        return {};
    }

    auto focusIt = std::find_if(nearbyCandidates.begin(), nearbyCandidates.end(), [](const NearbyNpcCandidate& candidate) {
        return candidate.underCrosshair;
        });

    std::ostringstream out;
    out << "{";
    if (!preferredVoiceTypeKey.empty())
    {
        out << "\"voice_type_key\":" << JsonEscape(preferredVoiceTypeKey);
        if (!preferredVoiceTypeName.empty())
        {
            out << ",\"voice_type_name\":" << JsonEscape(preferredVoiceTypeName);
        }
        out << ",";
    }
    out << "\"targeting\":{";
    if (focusIt != nearbyCandidates.end())
    {
        out << "\"focus_npc_key\":" << JsonEscape(focusIt->npcKey);
        out << ",\"focus_npc_name\":" << JsonEscape(focusIt->npcName);
        if (!focusIt->voiceTypeKey.empty())
        {
            out << ",\"focus_voice_type_key\":" << JsonEscape(focusIt->voiceTypeKey);
            if (!focusIt->voiceTypeName.empty())
            {
                out << ",\"focus_voice_type_name\":" << JsonEscape(focusIt->voiceTypeName);
            }
        }
        out << ",";
    }

    out << "\"nearby_npcs\":[";
    for (size_t index = 0; index < nearbyCandidates.size(); ++index)
    {
        if (index > 0)
        {
            out << ",";
        }

        const NearbyNpcCandidate& candidate = nearbyCandidates[index];
        const double distanceMeters = std::sqrt(candidate.distanceSquared) / kGameUnitsPerMeter;
        out << "{";
        out << "\"npc_key\":" << JsonEscape(candidate.npcKey);
        out << ",\"npc_name\":" << JsonEscape(candidate.npcName);
        out << ",\"ref_id\":" << (candidate.ref ? candidate.ref->refID : 0);
        out << ",\"pos_x\":" << std::fixed << std::setprecision(2) << (candidate.ref ? candidate.ref->posX : 0.0f);
        out << ",\"pos_y\":" << std::fixed << std::setprecision(2) << (candidate.ref ? candidate.ref->posY : 0.0f);
        out << ",\"pos_z\":" << std::fixed << std::setprecision(2) << (candidate.ref ? candidate.ref->posZ : 0.0f);
        if (!candidate.voiceTypeKey.empty())
        {
            out << ",\"voice_type_key\":" << JsonEscape(candidate.voiceTypeKey);
            if (!candidate.voiceTypeName.empty())
            {
                out << ",\"voice_type_name\":" << JsonEscape(candidate.voiceTypeName);
            }
        }
        out << ",\"distance_m\":" << std::fixed << std::setprecision(2) << distanceMeters;
        out << ",\"under_crosshair\":" << (candidate.underCrosshair ? "true" : "false");
        out << "}";
    }
    out << "]}}";
    return out.str();
}

std::string BuildAdminVoiceRequestMetadata(PlayerCharacter* player)
{
    std::string base = Trim(BuildTextRequestMetadata(player, nullptr));
    if (base.empty() || base == "{}")
    {
        base = "{";
    }
    else if (base.back() == '}')
    {
        base.pop_back();
        base += ",";
    }
    else
    {
        base = "{";
    }

    base += "\"admin\":true";
    base += ",\"adminMode\":true";
    base += ",\"voice_request\":true";
    base += ",\"input_mode\":\"admin\"";
    base += ",\"targetName\":";
    base += JsonEscape(kAdminNpcName);
    base += ",\"source\":\"fallout-new-vegas-native-admin-voice\"";
    base += "}";
    return base;
}

std::optional<ResolvedNpcTarget> FindFocusedMappedNpcForChat(PlayerCharacter* player)
{
    if (!player || !player->parentCell)
    {
        return std::nullopt;
    }

    TESObjectREFR* crosshairTarget = GetCrosshairRef();
    if (!crosshairTarget)
    {
        return std::nullopt;
    }

    const double maxDistanceSquared = std::pow(static_cast<double>(kChatNpcSearchRadiusMeters * kGameUnitsPerMeter), 2.0);
    ResolvedNpcTarget best{};
    ConsiderNearestMappedNpc(player, crosshairTarget, maxDistanceSquared, true, best);
    if (!best.ref)
    {
        return std::nullopt;
    }

    return best;
}

float SubtitleDuration(std::string_view text)
{
    const float seconds = 2.0f + static_cast<float>(text.size()) / 24.0f;
    return (std::min)((std::max)(seconds, kDefaultSubtitleSeconds), kMaxSubtitleSeconds);
}

std::string ActionNpcKey(const ResponsePayload& response)
{
    return response.actionNpcKey.empty() ? response.npcKey : response.actionNpcKey;
}

std::string ActionNpcName(const ResponsePayload& response)
{
    return response.actionNpcName.empty() ? response.npcName : response.actionNpcName;
}

SpeakerSnapshot ResolveActionSpeaker(const ResponsePayload& response)
{
    const std::string actionNpcKey = ActionNpcKey(response);
    const std::string actionNpcName = ActionNpcName(response);
    const bool hasExplicitActionTarget = !response.actionNpcKey.empty() || !response.actionNpcName.empty();

    if (!hasExplicitActionTarget && g_state.pendingSpeaker.refId)
    {
        return g_state.pendingSpeaker;
    }

    if (!actionNpcKey.empty() && actionNpcKey == g_state.lastNpcKey && g_state.lastNpcSpeaker.refId)
    {
        return g_state.lastNpcSpeaker;
    }

    if (const auto resolvedSpeaker = ResolveSpeakerSnapshotForNpc(actionNpcKey, actionNpcName); resolvedSpeaker.has_value())
    {
        return *resolvedSpeaker;
    }

    if (!hasExplicitActionTarget && g_state.pendingSpeaker.refId)
    {
        return g_state.pendingSpeaker;
    }

    return {};
}

Script* CompileTrustedExecutionScript(const ResponsePayload& response)
{
    if (!g_scriptInterface || response.executionScript.empty())
    {
        return nullptr;
    }

    const std::string baseKey = !response.executionTemplateId.empty()
        ? response.executionTemplateId
        : (!response.actionId.empty() ? response.actionId : response.gameMasterAction);
    const std::string cacheKey = baseKey + ":" + HashString64Hex(response.executionScript);
    const auto cached = g_trustedExecutionScripts.find(cacheKey);
    if (cached != g_trustedExecutionScripts.end())
    {
        return cached->second;
    }

    Script* script = g_scriptInterface->CompileScript(response.executionScript.c_str());
    if (!script)
    {
        LogLine("Failed to compile trusted Action Book script: action_id=%s template_id=%s.",
            response.actionId.c_str(),
            response.executionTemplateId.c_str());
        return nullptr;
    }

    g_trustedExecutionScripts[cacheKey] = script;
    LogLine("Compiled trusted Action Book script: action_id=%s template_id=%s hash=%s.",
        response.actionId.c_str(),
        response.executionTemplateId.c_str(),
        cacheKey.c_str());
    return script;
}

TESObjectREFR* ResolveTrustedExecutionRefArgument(const std::string& rawName, TESObjectREFR* actorRef, PlayerCharacter* player)
{
    const std::string name = ToLowerAscii(Trim(rawName));
    if (name == "actor" || name == "source" || name == "speaker" || name == "subject" || name == "npc" || name == "npc_ref")
    {
        return actorRef;
    }
    if (name == "player" || name == "target" || name == "target_actor" || name == "player_ref")
    {
        return player;
    }
    return nullptr;
}

TESObjectREFR* ResolveTrustedExecutionRefIdArgument(const std::string& rawValue)
{
    const auto formId = ParseTrustedFormId(rawValue);
    if (!formId.has_value())
    {
        return nullptr;
    }
    TESForm* form = LookupFormByID(*formId);
    if (!form)
    {
        return nullptr;
    }
    return DYNAMIC_CAST(form, TESForm, TESObjectREFR);
}

std::optional<UInt32> ParseTrustedFormId(const std::string& rawValue)
{
    std::string value = Trim(rawValue);
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    {
        value = value.substr(2);
    }
    if (value.empty() || value.size() > 8)
    {
        return std::nullopt;
    }
    for (const char ch : value)
    {
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
        {
            return std::nullopt;
        }
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 16);
    if (!end || *end != '\0')
    {
        return std::nullopt;
    }
    return static_cast<UInt32>(parsed);
}

std::optional<double> ParseTrustedNumber(const std::string& rawValue)
{
    const std::string value = Trim(rawValue);
    if (value.empty())
    {
        return std::nullopt;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (!end || *end != '\0')
    {
        return std::nullopt;
    }
    return parsed;
}

std::optional<TrustedExecutionArgument> ResolveTrustedExecutionArgument(const std::string& rawName, TESObjectREFR* actorRef, PlayerCharacter* player)
{
    const std::string raw = Trim(rawName);
    const size_t separator = raw.find(':');
    if (separator != std::string::npos)
    {
        const std::string type = ToLowerAscii(Trim(raw.substr(0, separator)));
        const std::string value = Trim(raw.substr(separator + 1));
        if (type == "ref" || type == "reference")
        {
            TESObjectREFR* ref = ResolveTrustedExecutionRefArgument(value, actorRef, player);
            if (!ref)
            {
                ref = ResolveTrustedExecutionRefIdArgument(value);
            }
            if (!ref)
            {
                return std::nullopt;
            }
            TrustedExecutionArgument argument{};
            argument.type = TrustedExecutionArgumentType::Ref;
            argument.ref = ref;
            return argument;
        }
        if (type == "refid" || type == "reference_id" || type == "referenceid")
        {
            TESObjectREFR* ref = ResolveTrustedExecutionRefIdArgument(value);
            if (!ref)
            {
                return std::nullopt;
            }
            TrustedExecutionArgument argument{};
            argument.type = TrustedExecutionArgumentType::Ref;
            argument.ref = ref;
            return argument;
        }
        if (type == "form" || type == "formid" || type == "form_id")
        {
            const auto formId = ParseTrustedFormId(value);
            TESForm* form = formId.has_value() ? LookupFormByID(*formId) : nullptr;
            if (!form)
            {
                return std::nullopt;
            }
            TrustedExecutionArgument argument{};
            argument.type = TrustedExecutionArgumentType::Form;
            argument.form = form;
            return argument;
        }
        if (type == "number" || type == "float" || type == "int" || type == "integer")
        {
            const auto number = ParseTrustedNumber(value);
            if (!number.has_value())
            {
                return std::nullopt;
            }
            TrustedExecutionArgument argument{};
            argument.type = TrustedExecutionArgumentType::Number;
            argument.number = *number;
            return argument;
        }
        if (type == "string" || type == "str")
        {
            TrustedExecutionArgument argument{};
            argument.type = TrustedExecutionArgumentType::String;
            argument.text = value;
            return argument;
        }
    }

    TESObjectREFR* ref = ResolveTrustedExecutionRefArgument(raw, actorRef, player);
    if (!ref)
    {
        return std::nullopt;
    }
    TrustedExecutionArgument argument{};
    argument.type = TrustedExecutionArgumentType::Ref;
    argument.ref = ref;
    return argument;
}

using TrustedCallValue = std::variant<TESObjectREFR*, TESForm*, double, const char*>;

TrustedCallValue GetTrustedCallValue(const TrustedExecutionArgument& arg)
{
    switch (arg.type)
    {
    case TrustedExecutionArgumentType::Ref:
        return arg.ref;
    case TrustedExecutionArgumentType::Form:
        return arg.form;
    case TrustedExecutionArgumentType::Number:
        return arg.number;
    case TrustedExecutionArgumentType::String:
        return arg.text.c_str();
    default:
        return static_cast<TESObjectREFR*>(nullptr);
    }
}

bool CallTrustedExecutionScript(Script* script, TESObjectREFR* callingRef, const std::vector<TrustedExecutionArgument>& args, NVSEArrayVarInterface::Element& result)
{
    if (!script || !g_scriptInterface || !callingRef)
    {
        return false;
    }

    std::vector<TrustedCallValue> values;
    values.reserve(args.size());
    for (const TrustedExecutionArgument& arg : args)
    {
        values.push_back(GetTrustedCallValue(arg));
    }

    switch (values.size())
    {
    case 0:
        return g_scriptInterface->CallFunction(script, callingRef, nullptr, &result, 0);
    case 1:
        return std::visit([&](auto arg0) {
            return g_scriptInterface->CallFunction(script, callingRef, nullptr, &result, 1, arg0);
        }, values[0]);
    case 2:
        return std::visit([&](auto arg0, auto arg1) {
            return g_scriptInterface->CallFunction(script, callingRef, nullptr, &result, 2, arg0, arg1);
        }, values[0], values[1]);
    case 3:
        return std::visit([&](auto arg0, auto arg1, auto arg2) {
            return g_scriptInterface->CallFunction(script, callingRef, nullptr, &result, 3, arg0, arg1, arg2);
        }, values[0], values[1], values[2]);
    case 4:
        return std::visit([&](auto arg0, auto arg1, auto arg2, auto arg3) {
            return g_scriptInterface->CallFunction(script, callingRef, nullptr, &result, 4, arg0, arg1, arg2, arg3);
        }, values[0], values[1], values[2], values[3]);
    default:
        LogLine("Trusted Action Book script has too many arguments: %zu.", args.size());
        return false;
    }
}

bool TriggerTrustedActionBinding(const ResponsePayload& response)
{
    if (response.executionScript.empty())
    {
        return false;
    }
    if (ToLowerAscii(Trim(response.executionEngine)) != kTrustedFNVActionEngine)
    {
        LogLine("Ignoring trusted Action Book execution for unsupported engine: %s.", response.executionEngine.c_str());
        return false;
    }
    if (!response.executionLanguage.empty()
        && response.executionLanguage != "geck/xnvse"
        && response.executionLanguage != "xnvse"
        && response.executionLanguage != "geck")
    {
        LogLine("Ignoring trusted Action Book execution for unsupported language: %s.", response.executionLanguage.c_str());
        return false;
    }

    Script* script = CompileTrustedExecutionScript(response);
    if (!script)
    {
        return false;
    }

    const std::string actionNpcKey = ActionNpcKey(response);
    const std::string actionNpcName = ActionNpcName(response);
    TESObjectREFR* actorRef = ResolveSpeakerRef(ResolveActionSpeaker(response));
    PlayerCharacter* player = GetPlayer();
    if (!player)
    {
        LogLine("Could not resolve player for trusted Action Book action: npc=%s action_id=%s template_id=%s.",
            actionNpcName.c_str(),
            response.actionId.c_str(),
            response.executionTemplateId.c_str());
        return false;
    }

    std::vector<TrustedExecutionArgument> trustedArgs;
    for (const std::string& argName : response.executionArguments)
    {
        const auto resolvedArg = ResolveTrustedExecutionArgument(argName, actorRef, player);
        if (!resolvedArg.has_value())
        {
            LogLine("Could not resolve trusted Action Book argument '%s' for action_id=%s template_id=%s.",
                argName.c_str(),
                response.actionId.c_str(),
                response.executionTemplateId.c_str());
            return false;
        }
        trustedArgs.push_back(*resolvedArg);
    }

    TESObjectREFR* callingRef = actorRef ? actorRef : static_cast<TESObjectREFR*>(player);
    NVSEArrayVarInterface::Element result;
    const bool callOk = CallTrustedExecutionScript(script, callingRef, trustedArgs, result);
    const bool issued = callOk
        && (result.GetType() != NVSEArrayVarInterface::Element::kType_Numeric || result.GetNumber() != 0.0);

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "trusted_action_book_execution",
            {
                { "npc_key", actionNpcKey },
                { "npc_name", actionNpcName },
                { "action_id", response.actionId },
                { "action_book_id", response.actionBookId },
                { "template_id", response.executionTemplateId },
                { "engine", response.executionEngine },
                { "language", response.executionLanguage },
            },
            {
                { "speaker_ref_id", actorRef ? static_cast<double>(actorRef->refID) : 0.0 },
                { "arg_count", static_cast<double>(trustedArgs.size()) },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "call_ok", callOk },
                { "issued", issued },
            });
    }

    if (!issued)
    {
        LogLine("Trusted Action Book execution did not issue: action_id=%s template_id=%s call_ok=%d result_type=%d.",
            response.actionId.c_str(),
            response.executionTemplateId.c_str(),
            callOk ? 1 : 0,
            callOk ? static_cast<int>(result.GetType()) : 0);
        return false;
    }

    if (actorRef)
    {
        RememberNpcTarget(actionNpcKey, actionNpcName, CaptureSpeakerSnapshot(actorRef));
    }
    if (!response.requestId.empty())
    {
        g_state.movementActionRequestIds.insert(response.requestId);
    }

    const std::string label = !response.actionId.empty() ? response.actionId : response.executionTemplateId;
    ShowHudMessage((actionNpcName.empty() ? std::string("NPC") : actionNpcName) + " executed " + (label.empty() ? std::string("Action Book action") : label) + ".");
    LogLine("Triggered trusted Action Book execution for %s: action_id=%s template_id=%s.",
        actionNpcName.c_str(),
        response.actionId.c_str(),
        response.executionTemplateId.c_str());
    return true;
}

bool TriggerNpcAttack(const ResponsePayload& response)
{
    if (ToUpperAscii(Trim(response.gameMasterAction)) != "ATTACK")
    {
        return false;
    }

    if (!EnsureStartCombatScript())
    {
        return false;
    }

    const std::string actionNpcName = ActionNpcName(response);
    TESObjectREFR* actorRef = ResolveSpeakerRef(ResolveActionSpeaker(response));
    PlayerCharacter* player = GetPlayer();
    if (!actorRef || !player)
    {
        LogLine("Could not resolve actor/player for ATTACK action: npc=%s action=%s", actionNpcName.c_str(), response.gameMasterAction.c_str());
        return false;
    }

    if (!g_scriptInterface->CallFunctionAlt(g_startCombatScript, actorRef, 2, actorRef, player))
    {
        LogLine("CallFunctionAlt failed for ATTACK action on %s.", actionNpcName.c_str());
        return false;
    }

    LogLine("Triggered StartCombat for %s via gamemaster ATTACK.", actionNpcName.c_str());
    return true;
}

bool TriggerNpcFollow(const ResponsePayload& response)
{
    if (ToUpperAscii(Trim(response.gameMasterAction)) != "FOLLOW")
    {
        return false;
    }

    const std::string actionNpcKey = ActionNpcKey(response);
    const std::string actionNpcName = ActionNpcName(response);
    TESObjectREFR* actorRef = ResolveSpeakerRef(ResolveActionSpeaker(response));
    PlayerCharacter* player = GetPlayer();
    TESForm* followPackage = ResolveDefaultFollowPackage();
    if (!actorRef || !player || !followPackage)
    {
        LogLine("Could not resolve actor/player/follow package for FOLLOW action: npc=%s action=%s", actionNpcName.c_str(), response.gameMasterAction.c_str());
        return false;
    }

    const bool teammateIssued = SetActorPlayerTeammate(actorRef);
    const bool packageIssued = AddActorScriptPackage(actorRef, followPackage, kDefaultFollowPackageEditorId, "game_master_follow_package");
    if (!packageIssued)
    {
        LogLine("Failed to apply follow package for %s.", actionNpcName.c_str());
        return false;
    }

    const SpeakerSnapshot snapshot = CaptureSpeakerSnapshot(actorRef);
    RememberNpcTarget(actionNpcKey, actionNpcName, snapshot);
    if (!response.requestId.empty())
    {
        g_state.movementActionRequestIds.insert(response.requestId);
    }

    if (!g_state.traceRequestId.empty())
    {
            TraceRequestEvent(g_state.traceRequestId, "game_master_follow_triggered",
            {
                { "npc_key", actionNpcKey },
                { "npc_name", actionNpcName },
                { "package_editor_id", kDefaultFollowPackageEditorId },
            },
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "player_ref_id", static_cast<double>(player->refID) },
                { "package_ref_id", static_cast<double>(followPackage->refID) },
            },
            {
                { "teammate_issued", teammateIssued },
                { "package_issued", packageIssued },
            });
    }

    ShowHudMessage((actionNpcName.empty() ? std::string("NPC") : actionNpcName) + " is following.");
    LogLine("Triggered follow package %s for %s via gamemaster FOLLOW.", kDefaultFollowPackageEditorId, actionNpcName.c_str());
    return true;
}

bool TriggerNpcStopFollow(const ResponsePayload& response)
{
    if (ToUpperAscii(Trim(response.gameMasterAction)) != "STOP_FOLLOW")
    {
        return false;
    }

    const std::string actionNpcKey = ActionNpcKey(response);
    const std::string actionNpcName = ActionNpcName(response);
    TESObjectREFR* actorRef = ResolveSpeakerRef(ResolveActionSpeaker(response));
    PlayerCharacter* player = GetPlayer();
    if (!actorRef)
    {
        LogLine("Could not resolve actor for STOP_FOLLOW action: npc=%s action=%s", actionNpcName.c_str(), response.gameMasterAction.c_str());
        return false;
    }

    TESForm* followPackage = ResolveDefaultFollowPackage();
    bool packageCurrentKnown = false;
    const bool packageCurrent = followPackage ? IsActorUsingPackage(actorRef, followPackage, &packageCurrentKnown) : false;
    const bool packageRemoved = followPackage && packageCurrentKnown && packageCurrent
        ? RemoveActorScriptPackage(actorRef, "game_master_stop_follow_package")
        : false;
    const bool teammateCleared = SetActorPlayerTeammate(actorRef, false, "game_master_stop_follow_teammate");

    const SpeakerSnapshot snapshot = CaptureSpeakerSnapshot(actorRef);
    RememberNpcTarget(actionNpcKey, actionNpcName, snapshot);
    if (!response.requestId.empty())
    {
        g_state.movementActionRequestIds.insert(response.requestId);
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "game_master_stop_follow_triggered",
            {
                { "npc_key", actionNpcKey },
                { "npc_name", actionNpcName },
                { "package_editor_id", kDefaultFollowPackageEditorId },
            },
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "player_ref_id", player ? static_cast<double>(player->refID) : 0.0 },
                { "package_ref_id", followPackage ? static_cast<double>(followPackage->refID) : 0.0 },
            },
            {
                { "package_current_known", packageCurrentKnown },
                { "package_current", packageCurrent },
                { "package_removed", packageRemoved },
                { "teammate_cleared", teammateCleared },
            });
    }

    if (!packageRemoved && !teammateCleared)
    {
        LogLine("STOP_FOLLOW action for %s resolved actor but did not remove package or clear teammate state.", actionNpcName.c_str());
        return false;
    }

    ShowHudMessage((actionNpcName.empty() ? std::string("NPC") : actionNpcName) + " stopped following.");
    LogLine("Triggered stop follow for %s via gamemaster STOP_FOLLOW (package_removed=%d teammate_cleared=%d).", actionNpcName.c_str(), packageRemoved ? 1 : 0, teammateCleared ? 1 : 0);
    return true;
}

bool TriggerGameMasterAction(const ResponsePayload& response, std::string* outTriggeredAction = nullptr)
{
    if (!response.isFinal || !response.ok || !response.gameMasterShouldTrigger)
    {
        return false;
    }

    const std::string action = ToUpperAscii(Trim(response.gameMasterAction));
    if (action.empty() || action == "NONE")
    {
        return false;
    }

    ReleaseConversationHold("game_master_action");

    if (!response.executionScript.empty())
    {
        const bool triggered = TriggerTrustedActionBinding(response);
        if (triggered)
        {
            if (outTriggeredAction)
            {
                *outTriggeredAction = response.actionId.empty() ? action : response.actionId;
            }
            return true;
        }
        if (action == "ACTION_BOOK")
        {
            return false;
        }
        LogLine("Trusted Action Book execution failed for %s; trying legacy native action %s.",
            response.actionId.c_str(),
            action.c_str());
    }

    if (action == "ATTACK")
    {
        const bool triggered = TriggerNpcAttack(response);
        if (triggered && outTriggeredAction)
        {
            *outTriggeredAction = action;
        }
        return triggered;
    }

    if (action == "FOLLOW")
    {
        const bool triggered = TriggerNpcFollow(response);
        if (triggered && outTriggeredAction)
        {
            *outTriggeredAction = action;
        }
        return triggered;
    }

    if (action == "STOP_FOLLOW")
    {
        const bool triggered = TriggerNpcStopFollow(response);
        if (triggered && outTriggeredAction)
        {
            *outTriggeredAction = action;
        }
        return triggered;
    }

    if (action == "ACTION_BOOK")
    {
        LogLine("Action Book command %s did not include executable trusted metadata.", response.actionId.c_str());
        return false;
    }

    LogLine("No native handler is implemented for gamemaster action %s on %s.", action.c_str(), response.npcName.c_str());
    return false;
}

std::string BuildSubtitleMessage(const std::string& speaker, const std::string& text)
{
    const std::string cleanText = Trim(text);
    if (cleanText.empty())
    {
        return "";
    }
    return cleanText;
}

void ShowGeneralSubtitle(const std::string& speaker, const std::string& text, float seconds)
{
    const std::string message = BuildSubtitleMessage(speaker, text);
    if (message.empty())
    {
        return;
    }

    LogLine("Subtitle: %s", message.c_str());
}

TileMenu* GetTileMenuByTypeLocal(UInt32 menuType)
{
    auto* menuArray = reinterpret_cast<NiTArray<TileMenu*>*>(kTileMenuArrayAddress);
    if (!menuArray || menuType < kMenuType_Min || menuType > kMenuType_Max)
    {
        return nullptr;
    }

    return menuArray->Get(menuType - kMenuType_Min);
}

Menu* GetMenuByTypeLocal(UInt32 menuType)
{
    TileMenu* tileMenu = GetTileMenuByTypeLocal(menuType);
    return tileMenu ? tileMenu->menu : nullptr;
}

Tile::Value* GetTileValueByIdLocal(Tile* tile, UInt32 valueId)
{
    if (!tile)
    {
        return nullptr;
    }

    UInt32 left = 0;
    UInt32 right = tile->values.size;
    while (left < right)
    {
        const UInt32 mid = left + ((right - left) / 2);
        Tile::Value* value = tile->values[mid];
        if (!value)
        {
            return nullptr;
        }

        if (value->id == valueId)
        {
            return value;
        }

        if (value->id < valueId)
        {
            left = mid + 1;
        }
        else
        {
            right = mid;
        }
    }

    return nullptr;
}

Tile::Value* GetTileValueByNameLocal(Tile* tile, const char* valueName)
{
    if (!tile || !valueName || !g_traitNameToId)
    {
        return nullptr;
    }

    return GetTileValueByIdLocal(tile, g_traitNameToId(valueName));
}

Tile* GetChildTileLocal(Tile* parentTile, const char* childName)
{
    if (!parentTile || !childName || !*childName)
    {
        return nullptr;
    }

    int childIndex = 0;
    char* colon = std::strchr(const_cast<char*>(childName), ':');
    if (colon)
    {
        if (colon == childName)
        {
            return nullptr;
        }
        *colon = 0;
        childIndex = std::atoi(colon + 1);
    }

    Tile* result = nullptr;
    const bool wildcard = *childName == '*';
    for (tList<Tile::ChildNode>::Iterator iter = parentTile->childList.Begin(); !iter.End(); ++iter)
    {
        if (*iter && iter->child && (wildcard || _stricmp(iter->child->name.m_data, childName) == 0) && !childIndex--)
        {
            result = iter->child;
            break;
        }
    }

    if (colon)
    {
        *colon = ':';
    }
    return result;
}

Tile::Value* GetTileComponentValueLocal(Tile* rootTile, const char* componentPath)
{
    if (!rootTile || !componentPath || !*componentPath)
    {
        return nullptr;
    }

    std::string mutablePath(componentPath);
    Tile* currentTile = rootTile;
    const char* remaining = mutablePath.c_str();
    char* slash = nullptr;
    while ((slash = std::strpbrk(const_cast<char*>(remaining), "/\\")) != nullptr)
    {
        *slash = 0;
        currentTile = GetChildTileLocal(currentTile, remaining);
        if (!currentTile)
        {
            return nullptr;
        }
        remaining = slash + 1;
    }

    return *remaining ? GetTileValueByNameLocal(currentTile, remaining) : nullptr;
}

bool SetMenuTileString(Menu* menu, const char* componentPath, const std::string& value)
{
    if (!menu || !menu->tile || !componentPath)
    {
        return false;
    }

    Tile::Value* tileValue = GetTileComponentValueLocal(menu->tile, componentPath);
    if (!tileValue || !tileValue->parent)
    {
        return false;
    }

    CALL_MEMBER_FN(tileValue->parent, SetStringValue)(tileValue->id, value.c_str(), true);
    return true;
}

bool SetMenuTileFloat(Menu* menu, const char* componentPath, float value)
{
    if (!menu || !menu->tile || !componentPath)
    {
        return false;
    }

    Tile::Value* tileValue = GetTileComponentValueLocal(menu->tile, componentPath);
    if (!tileValue || !tileValue->parent)
    {
        return false;
    }

    CALL_MEMBER_FN(tileValue->parent, SetFloatValue)(tileValue->id, value, true);
    return true;
}

bool SetTileTraitString(Tile* tile, const char* traitName, const std::string& value)
{
    if (!tile || !traitName || !*traitName)
    {
        return false;
    }

    Tile::Value* tileValue = GetTileValueByNameLocal(tile, traitName);
    if (!tileValue || !tileValue->parent)
    {
        return false;
    }

    CALL_MEMBER_FN(tileValue->parent, SetStringValue)(tileValue->id, value.c_str(), true);
    return true;
}

bool SetTileTraitFloat(Tile* tile, const char* traitName, float value)
{
    if (!tile || !traitName || !*traitName)
    {
        return false;
    }

    Tile::Value* tileValue = GetTileValueByNameLocal(tile, traitName);
    if (!tileValue || !tileValue->parent)
    {
        return false;
    }

    CALL_MEMBER_FN(tileValue->parent, SetFloatValue)(tileValue->id, value, true);
    return true;
}

std::string DescribeTilePath(Tile* tile)
{
    std::vector<std::string> parts;
    while (tile)
    {
        parts.push_back(tile->name.m_data ? tile->name.m_data : "<unnamed>");
        tile = tile->parent;
    }

    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    {
        if (!path.empty())
        {
            path += "/";
        }
        path += *it;
    }
    return path;
}

int ScoreSubtitleTileCandidate(Tile* tile)
{
    if (!tile || !tile->name.m_data)
    {
        return 0;
    }

    std::string lowerName = ToLowerAscii(tile->name.m_data);
    int score = 0;
    if (lowerName.find("subtitle") != std::string::npos)
    {
        score += 100;
    }
    if (lowerName.find("text") != std::string::npos)
    {
        score += 50;
    }
    if (lowerName.find("message") != std::string::npos)
    {
        score += 25;
    }
    if (lowerName.find("activator") != std::string::npos)
    {
        score -= 100;
    }
    return score;
}

Tile* FindBestSubtitleTextTile(Tile* root, int* bestScore = nullptr)
{
    if (!root)
    {
        if (bestScore)
        {
            *bestScore = 0;
        }
        return nullptr;
    }

    Tile* bestTile = nullptr;
    int localBestScore = 0;
    if (GetTileValueByNameLocal(root, "string"))
    {
        localBestScore = 1 + ScoreSubtitleTileCandidate(root);
        bestTile = root;
    }

    for (tList<Tile::ChildNode>::Iterator iter = root->childList.Begin(); !iter.End(); ++iter)
    {
        if (!*iter || !iter->child)
        {
            continue;
        }

        int childScore = 0;
        Tile* childBest = FindBestSubtitleTextTile(iter->child, &childScore);
        if (childBest && childScore > localBestScore)
        {
            localBestScore = childScore;
            bestTile = childBest;
        }
    }

    if (bestScore)
    {
        *bestScore = localBestScore;
    }
    return bestTile;
}

void ClearDescendantSubtitleStrings(Tile* root)
{
    if (!root)
    {
        return;
    }

    SetTileTraitString(root, "string", "");
    SetTileTraitFloat(root, "visible", 0.0f);
    SetTileTraitFloat(root, "alpha", 0.0f);
    for (tList<Tile::ChildNode>::Iterator iter = root->childList.Begin(); !iter.End(); ++iter)
    {
        if (*iter && iter->child)
        {
            ClearDescendantSubtitleStrings(iter->child);
        }
    }
}

void MakeTileChainVisible(Tile* tile)
{
    while (tile)
    {
        SetTileTraitFloat(tile, "visible", 1.0f);
        SetTileTraitFloat(tile, "alpha", 255.0f);
        tile = tile->parent;
    }
}

bool SetAnyMenuTileString(Menu* menu, std::initializer_list<const char*> componentPaths, const std::string& value, const char** matchedPath = nullptr)
{
    if (matchedPath)
    {
        *matchedPath = nullptr;
    }
    for (const char* componentPath : componentPaths)
    {
        if (!componentPath || !*componentPath)
        {
            continue;
        }

        if (SetMenuTileString(menu, componentPath, value))
        {
            if (matchedPath)
            {
                *matchedPath = componentPath;
            }
            return true;
        }
    }

    return false;
}

bool SetAnyMenuTileFloat(Menu* menu, std::initializer_list<const char*> componentPaths, float value, const char** matchedPath = nullptr)
{
    if (matchedPath)
    {
        *matchedPath = nullptr;
    }
    for (const char* componentPath : componentPaths)
    {
        if (!componentPath || !*componentPath)
        {
            continue;
        }

        if (SetMenuTileFloat(menu, componentPath, value))
        {
            if (matchedPath)
            {
                *matchedPath = componentPath;
            }
            return true;
        }
    }

    return false;
}

void ClearDialogSubtitle()
{
    if (Menu* hudMenu = GetMenuByTypeLocal(kMenuType_HUDMain))
    {
        if (Tile* subtitlesRoot = GetChildTileLocal(hudMenu->tile, "Subtitles"))
        {
            ClearDescendantSubtitleStrings(subtitlesRoot);
        }
        SetAnyMenuTileString(hudMenu,
            {
                "Info/justify_center_text/string",
                "Info/justify_center_hotrect/justify_center_text/string",
            },
            "");
        SetAnyMenuTileFloat(hudMenu,
            {
                "Info/justify_center_text/visible",
                "Info/justify_center_hotrect/justify_center_text/visible",
                "Info/justify_center_hotrect/visible",
            },
            0.0f);
        SetAnyMenuTileFloat(hudMenu,
            {
                "Info/justify_center_hotrect/alpha",
            },
            0.0f);
    }
    g_state.dialogSubtitleActive = false;
    g_state.dialogSubtitleHideTick = 0;
}

void ClearOutboxArtifacts(const char* reason)
{
    std::error_code ec;
    bool removedAny = false;

    if (fs::remove(OutboxPath(), ec))
    {
        removedAny = true;
    }
    ec.clear();

    if (fs::exists(OutboxChunkDir(), ec))
    {
        for (const auto& entry : fs::directory_iterator(OutboxChunkDir(), ec))
        {
            std::error_code removeEc;
            if (entry.is_regular_file(removeEc) && fs::remove(entry.path(), removeEc))
            {
                removedAny = true;
            }
        }
    }

    if (removedAny)
    {
        LogLine("Cleared bridge outbox artifacts (%s).", reason ? reason : "cleanup");
    }
}

bool HasQueuedOrPlayingReply()
{
    return g_state.awaitingReply
        || !g_state.pendingAudioChunks.empty()
        || !g_state.activeSounds.empty()
        || g_state.streamActive // Phase 3: the single streaming buffer counts as playing
        || (g_state.activeSpeechUntilTick && GetTickCount() < g_state.activeSpeechUntilTick)
        || HasPendingChunkFiles();
}

void ClearIdleOutboxArtifacts(const char* reason)
{
    if (g_state.awaitingReply
        || !g_state.pendingAudioChunks.empty()
        || !g_state.activeSounds.empty()
        || (g_state.activeSpeechUntilTick && GetTickCount() < g_state.activeSpeechUntilTick))
    {
        return;
    }

    std::error_code ec;
    if (fs::exists(OutboxPath(), ec) || HasPendingChunkFiles())
    {
        ClearOutboxArtifacts(reason ? reason : "idle_stale_response");
    }
}

void InterruptBridgeReplyAndPlayback(const char* reason)
{
    // Match the gate (HasQueuedOrPlayingReply) exactly. The old guard omitted
    // `streamActive`, so when a streaming reply had finished generating
    // (awaitingReply already false) but was still audibly playing out the stream
    // buffer, callers (e.g. STT push-to-talk) saw "playing" via the gate yet this
    // function early-returned and never called StopStreamingVoice — the line kept
    // playing until the next reply's first chunk replaced it (i.e. after release).
    const bool hadReplyState = HasQueuedOrPlayingReply();
    if (!hadReplyState)
    {
        return;
    }

    ClearOutboxArtifacts(reason ? reason : "reply_interrupted");

    StopSpeechAnimation();
    ClearDialogSubtitle();

    for (auto& sound : g_state.activeSounds)
    {
        if (sound.buffer)
        {
            sound.buffer->Stop();
            sound.buffer->Release();
            sound.buffer = nullptr;
        }
        if (sound.buffer3d)
        {
            sound.buffer3d->Release();
            sound.buffer3d = nullptr;
        }
    }
    g_state.activeSounds.clear();
    ShutdownDirectSound();

    ReleaseConversationHold(reason ? reason : "reply_interrupted");
    g_state.awaitingReply = false;
    g_state.awaitingVoiceReply = false;
    g_state.replyStartedTick = 0;
    g_state.lastBridgeActivityTick = 0;
    g_state.sawBridgeActivity = false;
    g_state.activeRequestId.clear();
    g_state.lastAudioChunkIndex = -1;
    g_state.subtitleShownForReply = false;
    g_state.activeSpeechUntilTick = 0;
    g_state.replySubtitleText.clear();
    g_state.streamedAudioSeenForReply = false;
    g_state.pendingAudioChunks.clear();
    StopStreamingVoice("reply_state_reset");
    WriteRuntimeHeartbeatIfNeeded(true);
    LogLine("Interrupted bridge reply playback (%s).", reason ? reason : "reply_interrupted");
}

bool ShowDialogSubtitle(const std::string& speaker, const std::string& text, float seconds)
{
    ShowGeneralSubtitle(speaker, text, seconds);

    const std::string message = BuildSubtitleMessage(speaker, text);
    if (message.empty())
    {
        return false;
    }

    ClearDialogSubtitle();

    Menu* hudMenu = GetMenuByTypeLocal(kMenuType_HUDMain);
    if (!hudMenu)
    {
        LogLine("HUDMainMenu unavailable while showing general subtitle.");
        return false;
    }

    if (Tile* subtitlesRoot = GetChildTileLocal(hudMenu->tile, "Subtitles"))
    {
        int subtitleScore = 0;
        Tile* subtitleTile = FindBestSubtitleTextTile(subtitlesRoot, &subtitleScore);
        if (subtitleTile && SetTileTraitString(subtitleTile, "string", message))
        {
            MakeTileChainVisible(subtitleTile);
            g_state.dialogSubtitleActive = true;
            const DWORD durationMs = static_cast<DWORD>((std::max)(0.1f, seconds) * 1000.0f);
            g_state.dialogSubtitleHideTick = GetTickCount() + durationMs;
            LogLine("Displayed general subtitle via HUD Subtitles tile %s for %.2fs.",
                DescribeTilePath(subtitleTile).c_str(),
                seconds);
            return true;
        }
    }

    const char* matchedTextPath = nullptr;
    const bool textSet = SetAnyMenuTileString(hudMenu,
        {
            "Info/justify_center_text/string",
            "Info/justify_center_hotrect/justify_center_text/string",
        },
        message,
        &matchedTextPath);
    const bool visibleSet = SetAnyMenuTileFloat(hudMenu,
        {
            "Info/justify_center_text/visible",
            "Info/justify_center_hotrect/justify_center_text/visible",
            "Info/justify_center_hotrect/visible",
        },
        1.0f);
    const bool alphaSet = SetAnyMenuTileFloat(hudMenu,
        {
            "Info/justify_center_hotrect/alpha",
        },
        255.0f);
    if (!textSet)
    {
        LogLine("Failed to set HUD centered subtitle text on any known path.");
        return false;
    }

    g_state.dialogSubtitleActive = true;
    const DWORD durationMs = static_cast<DWORD>((std::max)(0.1f, seconds) * 1000.0f);
    g_state.dialogSubtitleHideTick = GetTickCount() + durationMs;
    LogLine("Displayed HUD centered subtitle via path %s for %.2fs.",
        matchedTextPath ? matchedTextPath : "<unknown>",
        seconds);
    if (!visibleSet && !alphaSet)
    {
        LogLine("HUD centered subtitle text set, but no known visible/alpha trait was found.");
    }
    return true;
}

void ShowHudMessage(const std::string& message)
{
    if (message.empty())
    {
        return;
    }

    const std::string uiMessage = ToUiAscii(message);
    if (g_queueUiMessage && !uiMessage.empty())
    {
        g_queueUiMessage(uiMessage.c_str(), 0, nullptr, nullptr, 2.5f, false);
    }
    LogLine("HUD: %s", message.c_str());
}

void ShowRecognizedPlayerSubtitleIfNeeded(const ResponsePayload& response)
{
    // Player-origin text is retained in diagnostics/responses, but the in-game HUD
    // should only subtitle NPC/Todd replies.
    (void)response;
}

std::string ExtractJsonStringField(const std::string& text, const char* fieldName)
{
    if (!fieldName || !*fieldName)
    {
        return "";
    }

    const std::string needle = std::string("\"") + fieldName + "\"";
    const size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
    {
        return "";
    }

    size_t colonPos = text.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos)
    {
        return "";
    }

    size_t valueStart = text.find('"', colonPos + 1);
    if (valueStart == std::string::npos)
    {
        return "";
    }

    std::string result;
    bool escaped = false;
    for (size_t index = valueStart + 1; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (escaped)
        {
            switch (ch)
            {
            case '\\':
            case '"':
            case '/':
                result.push_back(ch);
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                result.push_back(ch);
                break;
            }
            escaped = false;
            continue;
        }

        if (ch == '\\')
        {
            escaped = true;
            continue;
        }

        if (ch == '"')
        {
            return result;
        }

        result.push_back(ch);
    }

    return "";
}

void UpdateVoiceBootstrapStatus()
{
    const DWORD now = GetTickCount();
    if (g_state.voiceBootstrapStatusPollTick && (now - g_state.voiceBootstrapStatusPollTick) < 300)
    {
        if (g_state.voiceBootstrapSubtitleActive
            && (!g_state.voiceBootstrapSubtitleRefreshTick || now >= g_state.voiceBootstrapSubtitleRefreshTick))
        {
            const std::string message = g_state.voiceBootstrapMessage.empty()
                ? "Cloning voices..."
                : g_state.voiceBootstrapMessage;
            ShowDialogSubtitle("", message, 1.2f);
            g_state.voiceBootstrapSubtitleRefreshTick = now + 700;
        }
        return;
    }

    g_state.voiceBootstrapStatusPollTick = now;

    std::ifstream in(VoiceBootstrapStatusPath(), std::ios::binary);
    if (!in)
    {
        if (g_state.voiceBootstrapSubtitleActive)
        {
            g_state.voiceBootstrapSubtitleActive = false;
            g_state.voiceBootstrapMessage.clear();
            g_state.voiceBootstrapSubtitleRefreshTick = 0;
            ClearDialogSubtitle();
        }
        return;
    }

    const std::string payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string status = ToLowerAscii(Trim(ExtractJsonStringField(payload, "status")));
    if (status != "running")
    {
        if (g_state.voiceBootstrapSubtitleActive)
        {
            g_state.voiceBootstrapSubtitleActive = false;
            g_state.voiceBootstrapMessage.clear();
            g_state.voiceBootstrapSubtitleRefreshTick = 0;
            ClearDialogSubtitle();
        }
        return;
    }

    g_state.voiceBootstrapSubtitleActive = true;
    g_state.voiceBootstrapMessage = Trim(ExtractJsonStringField(payload, "message"));
    if (g_state.voiceBootstrapMessage.empty())
    {
        g_state.voiceBootstrapMessage = "Cloning voices...";
    }

    if (!g_state.voiceBootstrapSubtitleRefreshTick || now >= g_state.voiceBootstrapSubtitleRefreshTick)
    {
        ShowDialogSubtitle("", g_state.voiceBootstrapMessage, 1.2f);
        g_state.voiceBootstrapSubtitleRefreshTick = now + 700;
    }
}

SpeakerSnapshot CaptureSpeakerSnapshot(TESObjectREFR* ref)
{
    SpeakerSnapshot snapshot{};
    if (!ref)
    {
        return snapshot;
    }

    snapshot.refId = ref->refID;
    snapshot.posX = ref->posX;
    snapshot.posY = ref->posY;
    snapshot.posZ = ref->posZ;
    snapshot.valid = true;
    return snapshot;
}

void RememberNpcTarget(const std::string& npcKey, const std::string& npcName, const SpeakerSnapshot& speaker)
{
    g_state.lastNpcKey = npcKey;
    g_state.lastNpcName = npcName;
    g_state.lastNpcSpeaker = speaker;
    if (!npcKey.empty() && speaker.valid)
    {
        g_state.npcSpeakersByKey[npcKey] = speaker;
    }
}

std::optional<SpeakerSnapshot> ResolveSpeakerSnapshotForNpc(const std::string& npcKey, const std::string& npcName)
{
    const auto matchesNpc = [&](const std::string& candidateKey, const std::string& candidateName) -> bool {
        if (!npcKey.empty() && !candidateKey.empty() && _stricmp(candidateKey.c_str(), npcKey.c_str()) == 0)
        {
            return true;
        }
        if (!npcName.empty() && !candidateName.empty() && _stricmp(candidateName.c_str(), npcName.c_str()) == 0)
        {
            return true;
        }
        return false;
    };

    if (matchesNpc(g_state.pendingNpcKey, g_state.pendingNpcName) && g_state.pendingSpeaker.valid)
    {
        return g_state.pendingSpeaker;
    }

    if (matchesNpc(g_state.lastNpcKey, g_state.lastNpcName) && g_state.lastNpcSpeaker.valid)
    {
        if (TESObjectREFR* lastRef = ResolveSpeakerRef(g_state.lastNpcSpeaker))
        {
            const SpeakerSnapshot liveSnapshot = CaptureSpeakerSnapshot(lastRef);
            if (liveSnapshot.valid)
            {
                return liveSnapshot;
            }
        }
        return g_state.lastNpcSpeaker;
    }

    if (!npcKey.empty())
    {
        auto remembered = g_state.npcSpeakersByKey.find(npcKey);
        if (remembered != g_state.npcSpeakersByKey.end())
        {
            if (TESObjectREFR* rememberedRef = ResolveSpeakerRef(remembered->second))
            {
                const SpeakerSnapshot liveSnapshot = CaptureSpeakerSnapshot(rememberedRef);
                if (liveSnapshot.valid)
                {
                    g_state.npcSpeakersByKey[npcKey] = liveSnapshot;
                    return liveSnapshot;
                }
            }
        }
    }

    PlayerCharacter* player = GetPlayer();
    if (!player)
    {
        return std::nullopt;
    }

    std::vector<NearbyNpcCandidate> candidates = FindNearbyMappedNpcsAround(player, kGamestateNearbyRadiusMeters);
    if (g_state.pendingSpeaker.valid)
    {
        if (TESObjectREFR* pendingRef = ResolveSpeakerRef(g_state.pendingSpeaker))
        {
            MergeNearbyNpcCandidates(candidates, FindNearbyMappedNpcsAround(pendingRef, kGamestateNearbyRadiusMeters));
        }
    }

    auto match = std::find_if(candidates.begin(), candidates.end(), [&](const NearbyNpcCandidate& candidate) {
        return matchesNpc(candidate.npcKey, candidate.npcName);
        });
    if (match == candidates.end() || !match->ref)
    {
        return std::nullopt;
    }

    const SpeakerSnapshot liveSnapshot = CaptureSpeakerSnapshot(match->ref);
    if (!liveSnapshot.valid)
    {
        return std::nullopt;
    }

    RememberNpcTarget(match->npcKey, match->npcName, liveSnapshot);
    return liveSnapshot;
}

std::string GetFormNameSafe(TESForm* form)
{
    if (!form)
    {
        return "";
    }

    char* name = form->GetName();
    if (!name)
    {
        return "";
    }

    return SanitizeLine(name);
}

std::string GetStringValueSafe(String& value)
{
    const char* text = value.m_data;
    return text ? SanitizeLine(text) : "";
}

double DistanceSquared3D(const TESObjectREFR* left, const TESObjectREFR* right)
{
    if (!left || !right)
    {
        return 0.0;
    }

    const double dx = static_cast<double>(left->posX) - static_cast<double>(right->posX);
    const double dy = static_cast<double>(left->posY) - static_cast<double>(right->posY);
    const double dz = static_cast<double>(left->posZ) - static_cast<double>(right->posZ);
    return (dx * dx) + (dy * dy) + (dz * dz);
}

LONG ComputeDistanceAttenuatedVolume(const TESObjectREFR* listener, const SpeakerSnapshot& speaker)
{
    if (!listener || !speaker.valid)
    {
        return DSBVOLUME_MAX;
    }

    const double dx = static_cast<double>(listener->posX) - static_cast<double>(speaker.posX);
    const double dy = static_cast<double>(listener->posY) - static_cast<double>(speaker.posY);
    const double dz = static_cast<double>(listener->posZ) - static_cast<double>(speaker.posZ);
    const double distanceMeters = std::sqrt(dx * dx + dy * dy + dz * dz) / kGameUnitsPerMeter;

    if (distanceMeters <= kVoiceMinDistanceMeters)
    {
        return DSBVOLUME_MAX;
    }

    if (distanceMeters >= kVoiceMaxDistanceMeters)
    {
        return DSBVOLUME_MIN;
    }

    const double t = (distanceMeters - kVoiceMinDistanceMeters) / (kVoiceMaxDistanceMeters - kVoiceMinDistanceMeters);
    const double amplitude = std::clamp(1.0 - t, 0.0, 1.0);
    if (amplitude <= 0.00001)
    {
        return DSBVOLUME_MIN;
    }

    const double db = 2000.0 * std::log10(amplitude);
    return static_cast<LONG>(std::clamp(db, static_cast<double>(DSBVOLUME_MIN), static_cast<double>(DSBVOLUME_MAX)));
}

float RadiansToDegrees(float radians)
{
    return radians * (180.0f / 3.14159265358979323846f);
}

float NormalizeDegrees360(float degrees)
{
    float normalized = std::fmod(degrees, 360.0f);
    if (normalized < 0.0f)
    {
        normalized += 360.0f;
    }
    return normalized;
}

float NormalizeSignedDegrees(float degrees)
{
    float normalized = NormalizeDegrees360(degrees);
    if (normalized > 180.0f)
    {
        normalized -= 360.0f;
    }
    return normalized;
}

float FacingDegreesTowardTarget(const TESObjectREFR* actorRef, const TESObjectREFR* targetRef)
{
    if (!actorRef || !targetRef)
    {
        return FLT_MAX;
    }

    const float dx = targetRef->posX - actorRef->posX;
    const float dy = targetRef->posY - actorRef->posY;
    if (std::fabs(dx) < 1.0f && std::fabs(dy) < 1.0f)
    {
        return FLT_MAX;
    }

    return NormalizeDegrees360(RadiansToDegrees(std::atan2(-dx, -dy)));
}

bool IsConversationHardHoldActive(DWORD now)
{
    return g_state.awaitingInput
        || g_state.awaitingReply
        || !g_state.pendingAudioChunks.empty()
        || !g_state.activeSounds.empty()
        || (g_state.activeSpeechUntilTick && now < g_state.activeSpeechUntilTick);
}

bool TryGetActorRestrained(TESObjectREFR* actorRef, bool& isRestrained)
{
    if (!actorRef || !EnsureConversationHoldScripts())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    if (!g_scriptInterface->CallFunction(g_getRestrainedScript, actorRef, nullptr, &result, 1, actorRef))
    {
        LogLine("CallFunction failed for GetRestrained helper on %08X.", actorRef->refID);
        return false;
    }

    isRestrained = result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric && result.GetNumber() != 0.0;
    return true;
}

bool SetActorRestrainedState(TESObjectREFR* actorRef, bool restrained)
{
    if (!actorRef || !EnsureConversationHoldScripts())
    {
        return false;
    }

    Script* script = restrained ? g_setRestrainedScript : g_clearRestrainedScript;
    if (!g_scriptInterface->CallFunctionAlt(script, actorRef, 1, actorRef))
    {
        LogLine("CallFunctionAlt failed for %sRestrained helper on %08X.",
            restrained ? "Set" : "Clear",
            actorRef->refID);
        return false;
    }

    return true;
}

bool SetActorLookAtPlayer(TESObjectREFR* actorRef, PlayerCharacter* player, bool enabled)
{
    if (!actorRef || !player || !EnsureConversationHoldScripts())
    {
        return false;
    }

    Script* script = enabled ? g_startLookScript : g_stopLookScript;
    const UInt8 numArgs = enabled ? 2 : 1;
    const bool ok = enabled
        ? g_scriptInterface->CallFunctionAlt(script, actorRef, numArgs, actorRef, player)
        : g_scriptInterface->CallFunctionAlt(script, actorRef, numArgs, actorRef);
    if (!ok)
    {
        LogLine("CallFunctionAlt failed for %sLook helper on %08X.",
            enabled ? "" : "Stop",
            actorRef->refID);
        if (!g_state.traceRequestId.empty())
        {
            TraceRequestEvent(g_state.traceRequestId, "conversation_hold_look",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                },
                {
                    { "enabled", enabled },
                    { "call_ok", false },
                });
        }
        return false;
    }

    if (g_state.conversationHold.active)
    {
        g_state.conversationHold.lookApplied = enabled;
        if (!enabled)
        {
            g_state.conversationHold.lastAppliedFacingDegrees = FLT_MAX;
            g_state.conversationHold.lastFaceUpdateTick = 0;
            g_state.conversationHold.lastBodyFaceUpdateTick = 0;
        }
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_look",
            {},
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
            },
            {
                { "enabled", enabled },
                { "call_ok", true },
            });
    }

    return true;
}

bool SetActorFacingPlayerIfNeeded(TESObjectREFR* actorRef, PlayerCharacter* player, DWORD now, bool force, bool* outIssued = nullptr)
{
    if (outIssued)
    {
        *outIssued = false;
    }
    if (!actorRef || !player || !EnsureConversationHoldScripts())
    {
        return false;
    }

    auto& hold = g_state.conversationHold;
    if (!force && hold.lastBodyFaceUpdateTick && now - hold.lastBodyFaceUpdateTick < g_debugConfig.conversationModeFaceRefreshIntervalMs)
    {
        return true;
    }

    const float targetDegrees = FacingDegreesTowardTarget(actorRef, player);
    if (targetDegrees == FLT_MAX)
    {
        hold.lastBodyFaceUpdateTick = now;
        return true;
    }

    const bool hasPriorFacing = hold.lastAppliedFacingDegrees != FLT_MAX;
    const float delta = hasPriorFacing
        ? std::fabs(NormalizeSignedDegrees(targetDegrees - hold.lastAppliedFacingDegrees))
        : FLT_MAX;
    if (!force && hasPriorFacing && delta < kConversationFaceTurnThresholdDegrees)
    {
        hold.lastBodyFaceUpdateTick = now;
        return true;
    }

    if (!g_faceObjectScript)
    {
        hold.lastBodyFaceUpdateTick = now;
        return true;
    }

    if (!g_scriptInterface->CallFunctionAlt(g_faceObjectScript, actorRef, 2, actorRef, player))
    {
        hold.lastBodyFaceUpdateTick = now;
        LogLine("CallFunctionAlt failed for FaceObject helper on %08X.", actorRef->refID);
        if (!g_state.traceRequestId.empty())
        {
            TraceRequestEvent(g_state.traceRequestId, "conversation_hold_face_player",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                    { "target_degrees", static_cast<double>(targetDegrees) },
                },
                {
                    { "call_ok", false },
                    { "issued", false },
                    { "used_face_object", true },
                });
        }
        return false;
    }

    hold.lastAppliedFacingDegrees = targetDegrees;
    hold.lastBodyFaceUpdateTick = now;
    if (outIssued)
    {
        *outIssued = true;
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_face_player",
            {},
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "target_degrees", static_cast<double>(targetDegrees) },
                { "delta_degrees", hasPriorFacing ? static_cast<double>(delta) : 360.0 },
            },
            {
                { "call_ok", true },
                { "issued", true },
                { "used_face_object", true },
            });
    }

    return true;
}

bool RefreshActorLookAtPlayerIfNeeded(TESObjectREFR* actorRef, PlayerCharacter* player, DWORD now, bool force, bool* outIssued = nullptr)
{
    if (outIssued)
    {
        *outIssued = false;
    }
    if (!actorRef || !player)
    {
        return false;
    }

    auto& hold = g_state.conversationHold;
    bool shouldIssue = force || !hold.lookApplied || !hold.lastFaceUpdateTick;
    if (!shouldIssue && now - hold.lastFaceUpdateTick >= g_debugConfig.conversationLookRefreshIntervalMs)
    {
        shouldIssue = true;
    }
    if (!shouldIssue)
    {
        return true;
    }

    if (!SetActorLookAtPlayer(actorRef, player, true))
    {
        return false;
    }

    hold.lookApplied = true;
    hold.lastFaceUpdateTick = now;
    if (outIssued)
    {
        *outIssued = true;
    }
    return true;
}

bool IsConversationModeDistanceExceeded(TESObjectREFR* actorRef, PlayerCharacter* player)
{
    if (!actorRef || !player)
    {
        return false;
    }

    const double maxDistanceUnits = static_cast<double>(g_debugConfig.conversationModeReleaseDistanceMeters) * kGameUnitsPerMeter;
    return DistanceSquared3D(actorRef, player) > (maxDistanceUnits * maxDistanceUnits);
}

bool SetActorNoMovePackageState(TESObjectREFR* actorRef, bool enabled, bool* outIssued = nullptr)
{
    if (outIssued)
    {
        *outIssued = false;
    }
    if (!actorRef || !EnsureConversationHoldScripts())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    bool callOk = false;
    bool issued = false;
    if (enabled)
    {
        callOk = g_applyNoMovePackageScript
            && g_scriptInterface->CallFunction(g_applyNoMovePackageScript, actorRef, nullptr, &result, 1, actorRef);
    }
    else
    {
        callOk = g_scriptInterface->CallFunction(g_removeScriptPackageScript, actorRef, nullptr, &result, 1, actorRef);
    }

    issued = callOk
        && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric
        && result.GetNumber() != 0.0;
    if (!enabled && issued)
    {
        EvaluateActorPackage(actorRef);
    }
    if (outIssued)
    {
        *outIssued = issued;
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_no_move_package",
            {
                { "package_editor_id", "DefaultSandboxNoMoveCurrentLocation200" },
            },
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "enabled", enabled },
                { "call_ok", callOk },
                { "issued", issued },
            });
    }

    return issued;
}

bool ApplyConversationModeIfNeeded(TESObjectREFR* actorRef, PlayerCharacter* player, DWORD now, bool force, bool* outRestrainedIssued = nullptr, bool* outFaceIssued = nullptr, bool* outLookIssued = nullptr)
{
    if (outRestrainedIssued)
    {
        *outRestrainedIssued = false;
    }
    if (outFaceIssued)
    {
        *outFaceIssued = false;
    }
    if (outLookIssued)
    {
        *outLookIssued = false;
    }
    if (!g_debugConfig.conversationModeEnabled || !actorRef || !player)
    {
        return false;
    }

    auto& hold = g_state.conversationHold;
    if (!hold.active)
    {
        return false;
    }

    if (hold.preserveFurnitureState)
    {
        if (hold.noMovePackageApplied)
        {
            SetActorNoMovePackageState(actorRef, false);
            hold.noMovePackageApplied = false;
        }
        if (hold.restrainedApplied)
        {
            SetActorRestrainedState(actorRef, false);
            hold.restrainedApplied = false;
        }
        hold.conversationModeApplied = true;
        return RefreshActorLookAtPlayerIfNeeded(actorRef, player, now, force, outLookIssued);
    }

    if (!hold.conversationModeApplied)
    {
        bool originalRestrained = false;
        hold.originalRestrainedKnown = TryGetActorRestrained(actorRef, originalRestrained);
        hold.originalRestrained = hold.originalRestrainedKnown ? originalRestrained : false;

        bool noMoveIssued = false;
        hold.noMovePackageApplied = SetActorNoMovePackageState(actorRef, true, &noMoveIssued);
        if (hold.noMovePackageApplied)
        {
            hold.conversationModeApplied = true;
            hold.restrainedApplied = false;
            if (outRestrainedIssued)
            {
                *outRestrainedIssued = noMoveIssued;
            }
        }
        else if (SetActorRestrainedState(actorRef, true))
        {
            hold.conversationModeApplied = true;
            hold.restrainedApplied = !hold.originalRestrainedKnown || !hold.originalRestrained;
            if (outRestrainedIssued)
            {
                *outRestrainedIssued = true;
            }
        }
        else
        {
            return false;
        }
    }

    const bool faceOk = SetActorFacingPlayerIfNeeded(actorRef, player, now, force, outFaceIssued);
    const bool lookOk = RefreshActorLookAtPlayerIfNeeded(actorRef, player, now, force, outLookIssued);
    return faceOk && lookOk;
}

bool StartActorConversationWithPlayer(TESObjectREFR* actorRef, PlayerCharacter* player)
{
    if (!actorRef || !player || !EnsureConversationHoldScripts())
    {
        return false;
    }

    TESForm* topicForm = ResolveModLocalForm(kBridgeDialoguePluginName, kBridgeDialogueTopicLocalFormId);
    if (!topicForm)
    {
        LogLine("StartConversation hold could not resolve topic %08X from %s.",
            kBridgeDialogueTopicLocalFormId,
            kBridgeDialoguePluginName);
        return false;
    }

    NVSEArrayVarInterface::Element result;
    const bool callOk = g_scriptInterface->CallFunction(g_startConversationScript, actorRef, nullptr, &result, 5,
        actorRef, player, topicForm, actorRef, player);
    if (!callOk)
    {
        LogLine("CallFunction failed for StartConversation helper on %08X.", actorRef->refID);
        if (!g_state.traceRequestId.empty())
        {
            TraceRequestEvent(g_state.traceRequestId, "conversation_hold_start_conversation",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                    { "topic_form_id", static_cast<double>(topicForm->refID) },
                },
                {
                    { "call_ok", false },
                    { "issued", false },
                });
        }
        return false;
    }

    const bool issued = result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric && result.GetNumber() != 0.0;
    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_start_conversation",
            {},
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "topic_form_id", static_cast<double>(topicForm->refID) },
                { "result_type", static_cast<double>(result.GetType()) },
                { "result_number", result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "call_ok", true },
                { "issued", issued },
            });
    }

    if (!issued)
    {
        LogLine("StartConversation helper returned not-issued for %08X topic %08X.",
            actorRef->refID,
            topicForm->refID);
    }

    return issued;
}

bool EvaluateActorPackage(TESObjectREFR* actorRef)
{
    if (!actorRef || !EnsureConversationHoldScripts())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    const bool callOk = g_scriptInterface->CallFunction(g_evaluatePackageScript, actorRef, nullptr, &result, 1, actorRef);
    const bool issued = callOk
        && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric
        && result.GetNumber() != 0.0;
    if (!callOk)
    {
        LogLine("CallFunction failed for EvaluatePackage helper on %08X.", actorRef->refID);
    }
    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_evaluate_package",
            {},
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "call_ok", callOk },
                { "issued", issued },
            });
    }
    return issued;
}

bool AddActorScriptPackage(TESObjectREFR* actorRef, TESForm* packageForm, const char* packageEditorId, const char* traceStage)
{
    if (!actorRef || !packageForm)
    {
        return false;
    }

    if (!EnsureConversationHoldScripts())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    const bool callOk = g_scriptInterface->CallFunction(g_addScriptPackageScript, actorRef, nullptr, &result, 2, actorRef, packageForm);
    const bool issued = callOk
        && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric
        && result.GetNumber() != 0.0;

    if (issued)
    {
        EvaluateActorPackage(actorRef);
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, traceStage ? traceStage : "script_package_added",
            {
                { "package_editor_id", packageEditorId ? packageEditorId : "" },
            },
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "package_ref_id", static_cast<double>(packageForm->refID) },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "call_ok", callOk },
                { "issued", issued },
            });
    }

    if (!callOk)
    {
        LogLine("CallFunction failed for AddScriptPackage helper on %08X package %s.", actorRef->refID, packageEditorId ? packageEditorId : "<unknown>");
    }

    return issued;
}

bool RemoveActorScriptPackage(TESObjectREFR* actorRef, const char* traceStage)
{
    if (!actorRef || !EnsureConversationHoldScripts())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    const bool callOk = g_scriptInterface->CallFunction(g_removeScriptPackageScript, actorRef, nullptr, &result, 1, actorRef);
    const bool issued = callOk
        && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric
        && result.GetNumber() != 0.0;

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, traceStage ? traceStage : "script_package_removed",
            {},
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "call_ok", callOk },
                { "issued", issued },
            });
    }

    if (!callOk)
    {
        LogLine("CallFunction failed for RemoveScriptPackage helper on %08X.", actorRef->refID);
    }

    return issued;
}

bool SetActorPlayerTeammate(TESObjectREFR* actorRef, bool enabled, const char* traceStage)
{
    if (!actorRef || !EnsurePlayerTeammateScript())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    const bool callOk = g_scriptInterface->CallFunction(g_setPlayerTeammateScript, actorRef, nullptr, &result, 2, actorRef, enabled ? 1 : 0);
    const bool issued = callOk
        && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric
        && result.GetNumber() != 0.0;

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, traceStage ? traceStage : "game_master_follow_teammate",
            {},
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "call_ok", callOk },
                { "issued", issued },
                { "teammate_enabled", enabled },
            });
    }

    if (!callOk)
    {
        LogLine("CallFunction failed for SetPlayerTeammate helper on %08X enabled=%d.", actorRef->refID, enabled ? 1 : 0);
    }

    return issued;
}

TESForm* ResolveDefaultFollowPackage()
{
    TESForm* form = GetFormByID(kDefaultFollowPackageEditorId);
    if (!form)
    {
        LogLine("Could not resolve follow package editor id %s.", kDefaultFollowPackageEditorId);
        return nullptr;
    }

    TESPackage* package = DYNAMIC_CAST(form, TESForm, TESPackage);
    if (!package)
    {
        LogLine("Resolved %s to %08X, but it is not a TESPackage.", kDefaultFollowPackageEditorId, form->refID);
        return nullptr;
    }

    return form;
}

bool SetActorConversationPackageState(TESObjectREFR* actorRef, bool enabled, bool* outIssued = nullptr)
{
    if (!actorRef)
    {
        if (outIssued)
        {
            *outIssued = false;
        }
        return false;
    }

    if (!EnsureConversationHoldScripts())
    {
        if (outIssued)
        {
            *outIssued = false;
        }
        return false;
    }

    TESForm* packageForm = enabled ? ResolveModLocalForm(kBridgeDialoguePluginName, kBridgeConversationPackageLocalFormId) : nullptr;
    NVSEArrayVarInterface::Element result;
    bool callOk = false;
    bool issued = false;

    if (enabled)
    {
        callOk = packageForm
            && g_scriptInterface->CallFunction(g_addScriptPackageScript, actorRef, nullptr, &result, 2, actorRef, packageForm);
    }
    else
    {
        callOk = g_scriptInterface->CallFunction(g_removeScriptPackageScript, actorRef, nullptr, &result, 1, actorRef);
    }

    issued = callOk
        && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric
        && result.GetNumber() != 0.0;
    if (issued)
    {
        EvaluateActorPackage(actorRef);
    }
    if (outIssued)
    {
        *outIssued = issued;
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_script_package",
            {
                { "package_editor_id", kBridgeConversationPackageEditorId },
            },
            {
                { "speaker_ref_id", static_cast<double>(actorRef->refID) },
                { "package_local_form_id_hint", static_cast<double>(kBridgeConversationPackageLocalFormId) },
                { "package_ref_id", packageForm ? static_cast<double>(packageForm->refID) : 0.0 },
                { "result_type", callOk ? static_cast<double>(result.GetType()) : 0.0 },
                { "result_number", callOk && result.GetType() == NVSEArrayVarInterface::Element::kType_Numeric ? result.GetNumber() : 0.0 },
            },
            {
                { "enabled", enabled },
                { "call_ok", callOk },
                { "issued", issued },
            });
    }

    return issued;
}

bool IsActorUsingPackage(TESObjectREFR* actorRef, TESForm* packageForm, bool* outKnown)
{
    if (outKnown)
    {
        *outKnown = false;
    }

    if (!actorRef || !packageForm || !EnsureConversationHoldScripts())
    {
        return false;
    }

    NVSEArrayVarInterface::Element result;
    const bool callOk = g_scriptInterface->CallFunction(g_isCurrentPackageScript, actorRef, nullptr, &result, 2, actorRef, packageForm);
    if (!callOk || result.GetType() != NVSEArrayVarInterface::Element::kType_Numeric)
    {
        return false;
    }

    if (outKnown)
    {
        *outKnown = true;
    }
    return result.GetNumber() != 0.0;
}

bool IsActorUsingBridgeConversationPackage(TESObjectREFR* actorRef, bool* outKnown)
{
    TESForm* packageForm = ResolveModLocalForm(kBridgeDialoguePluginName, kBridgeConversationPackageLocalFormId);
    return IsActorUsingPackage(actorRef, packageForm, outKnown);
}

bool ShouldPreserveActorConversationAnimation(TESObjectREFR* speakerRef)
{
    auto* actor = static_cast<Actor*>(speakerRef);
    if (!actor || !actor->baseProcess)
    {
        return false;
    }

    const int sitSleepState = actor->baseProcess->GetSitSleepState();
    return sitSleepState == HighProcess::kSitSleepState_LoadSitIdle
        || sitSleepState == HighProcess::kSitSleepState_WantToSit
        || sitSleepState == HighProcess::kSitSleepState_WaitingForSitAnim
        || sitSleepState == HighProcess::kSitSleepState_Sitting
        || sitSleepState == HighProcess::kSitSleepState_LoadingSleepIdle
        || sitSleepState == HighProcess::kSitSleepState_WantToSleep
        || sitSleepState == HighProcess::kSitSleepState_WaitingForSleepAnim
        || sitSleepState == HighProcess::kSitSleepState_Sleeping;
}

void ReleaseConversationHold(const char* reason)
{
    auto hold = std::move(g_state.conversationHold);
    g_state.conversationHold = {};

    if (!hold.active)
    {
        return;
    }

    TESObjectREFR* actorRef = ResolveSpeakerRef(hold.speaker);
    PlayerCharacter* player = GetPlayer();
    if (actorRef && player)
    {
        SetActorLookAtPlayer(actorRef, player, false);
    }

    if (actorRef && hold.scriptPackageApplied)
    {
        SetActorConversationPackageState(actorRef, false);
    }

    if (actorRef && hold.noMovePackageApplied)
    {
        SetActorNoMovePackageState(actorRef, false);
    }

    if (actorRef && hold.conversationModeApplied)
    {
        if (hold.restrainedApplied)
        {
            SetActorRestrainedState(actorRef, false);
        }
        else if (hold.originalRestrained)
        {
            EvaluateActorPackage(actorRef);
        }
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_released",
            {
                { "reason", reason ? reason : "" },
                { "npc_key", hold.npcKey },
                { "npc_name", hold.npcName },
            },
            {
                { "speaker_ref_id", static_cast<double>(hold.speaker.refId) },
            },
            {
                { "script_package_applied", hold.scriptPackageApplied },
                { "conversation_mode_applied", hold.conversationModeApplied },
                { "no_move_package_applied", hold.noMovePackageApplied },
                { "restrained_applied", hold.restrainedApplied },
                { "original_restrained_known", hold.originalRestrainedKnown },
                { "original_restrained", hold.originalRestrained },
            });
    }
}

void EngageConversationHold(const std::string& npcKey, const std::string& npcName, const SpeakerSnapshot& speaker)
{
    if (!speaker.refId)
    {
        return;
    }

    auto& hold = g_state.conversationHold;
    if (hold.active && hold.speaker.refId != speaker.refId)
    {
        ReleaseConversationHold("speaker_changed");
    }

    ConversationHoldState& updatedHold = g_state.conversationHold;
    const bool wasActive = updatedHold.active;
    updatedHold.active = true;
    updatedHold.npcKey = npcKey;
    updatedHold.npcName = npcName;
    updatedHold.speaker = speaker;
    updatedHold.releaseTick = 0;
    updatedHold.conversationIssued = wasActive ? updatedHold.conversationIssued : false;
    updatedHold.lookApplied = wasActive ? updatedHold.lookApplied : false;
    updatedHold.conversationModeApplied = wasActive ? updatedHold.conversationModeApplied : false;
    updatedHold.originalRestrainedKnown = wasActive ? updatedHold.originalRestrainedKnown : false;
    updatedHold.originalRestrained = wasActive ? updatedHold.originalRestrained : false;
    updatedHold.restrainedApplied = wasActive ? updatedHold.restrainedApplied : false;
    updatedHold.noMovePackageApplied = wasActive ? updatedHold.noMovePackageApplied : false;
    updatedHold.lastAppliedFacingDegrees = wasActive ? updatedHold.lastAppliedFacingDegrees : FLT_MAX;
    updatedHold.lastBodyFaceUpdateTick = wasActive ? updatedHold.lastBodyFaceUpdateTick : 0;
    updatedHold.lastPackageCheckTick = wasActive ? updatedHold.lastPackageCheckTick : 0;

    TESObjectREFR* actorRef = ResolveSpeakerRef(speaker);
    PlayerCharacter* player = GetPlayer();
    if (!actorRef || !player)
    {
        return;
    }

    updatedHold.preserveFurnitureState = wasActive
        ? (updatedHold.preserveFurnitureState || ShouldPreserveActorConversationAnimation(actorRef))
        : ShouldPreserveActorConversationAnimation(actorRef);

    updatedHold.scriptPackageApplied = wasActive ? updatedHold.scriptPackageApplied : false;
    const bool useScriptPackage = !g_debugConfig.conversationModeEnabled && !updatedHold.preserveFurnitureState;
    if (!wasActive && useScriptPackage)
    {
        bool packageIssued = false;
        updatedHold.scriptPackageApplied = SetActorConversationPackageState(actorRef, true, &packageIssued);
        if (!g_state.traceRequestId.empty())
        {
            TraceRequestEvent(g_state.traceRequestId, "conversation_hold_package_applied",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(speaker.refId) },
                },
                {
                    { "issued", packageIssued },
                    { "active_after_call", updatedHold.scriptPackageApplied },
                });
        }
        updatedHold.conversationIssued = updatedHold.scriptPackageApplied
            ? StartActorConversationWithPlayer(actorRef, player)
            : false;
    }
    else if (!wasActive)
    {
        updatedHold.conversationIssued = false;
    }
    const DWORD now = GetTickCount();
    bool restrainedIssued = false;
    bool faceIssued = false;
    bool lookIssued = false;
    if (g_debugConfig.conversationModeEnabled)
    {
        ApplyConversationModeIfNeeded(actorRef, player, now, !wasActive, &restrainedIssued, &faceIssued, &lookIssued);
    }
    else
    {
        RefreshActorLookAtPlayerIfNeeded(actorRef, player, now, !wasActive, &lookIssued);
    }

    if (!g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_engaged",
            {
                { "npc_key", npcKey },
                { "npc_name", npcName },
            },
            {
                { "speaker_ref_id", static_cast<double>(speaker.refId) },
            },
            {
                { "script_package_applied", updatedHold.scriptPackageApplied },
                { "conversation_issued", updatedHold.conversationIssued },
                { "preserve_furniture_state", updatedHold.preserveFurnitureState },
                { "conversation_mode_enabled", g_debugConfig.conversationModeEnabled },
                { "conversation_mode_applied", updatedHold.conversationModeApplied },
                { "no_move_package_applied", updatedHold.noMovePackageApplied },
                { "restrained_issued", restrainedIssued },
                { "face_issued", faceIssued },
                { "look_issued", lookIssued },
            });
    }
}

void UpdateConversationHold()
{
    auto& hold = g_state.conversationHold;
    if (!hold.active)
    {
        return;
    }

    const DWORD now = GetTickCount();
    const bool hardHold = IsConversationHardHoldActive(now);
    if (hardHold)
    {
        hold.releaseTick = now + kConversationReleaseDelayMs;
    }
    else if (!hold.releaseTick)
    {
        hold.releaseTick = now + kConversationReleaseDelayMs;
    }

    TESObjectREFR* actorRef = ResolveSpeakerRef(hold.speaker);
    PlayerCharacter* player = GetPlayer();
    if (!actorRef || !player)
    {
        ReleaseConversationHold("speaker_unresolved");
        return;
    }

    if (g_debugConfig.conversationModeEnabled && IsConversationModeDistanceExceeded(actorRef, player))
    {
        ReleaseConversationHold("player_out_of_range");
        return;
    }

    const SpeakerSnapshot liveSpeaker = CaptureSpeakerSnapshot(actorRef);
    if (liveSpeaker.valid)
    {
        hold.speaker = liveSpeaker;
    }
    if (!hold.preserveFurnitureState && ShouldPreserveActorConversationAnimation(actorRef))
    {
        hold.preserveFurnitureState = true;
    }

    bool packageIssued = false;
    bool packageCurrentKnown = false;
    bool packageCurrent = false;
    bool packageChecked = false;
    if (!g_debugConfig.conversationModeEnabled && !hold.preserveFurnitureState && (!hold.lastPackageCheckTick || now - hold.lastPackageCheckTick >= kConversationPackageRefreshIntervalMs))
    {
        packageChecked = true;
        hold.lastPackageCheckTick = now;
        packageCurrent = IsActorUsingBridgeConversationPackage(actorRef, &packageCurrentKnown);
        if (!hold.scriptPackageApplied || (packageCurrentKnown && !packageCurrent))
        {
            hold.scriptPackageApplied = SetActorConversationPackageState(actorRef, true, &packageIssued);
        }
    }

    bool restrainedIssued = false;
    bool faceIssued = false;
    bool lookIssued = false;
    bool modeRefreshed = false;
    if (g_debugConfig.conversationModeEnabled)
    {
        modeRefreshed = ApplyConversationModeIfNeeded(actorRef, player, now, false, &restrainedIssued, &faceIssued, &lookIssued);
    }
    else if (!hold.lastFaceUpdateTick || now - hold.lastFaceUpdateTick >= kConversationFaceUpdateIntervalMs)
    {
        RefreshActorLookAtPlayerIfNeeded(actorRef, player, now, false, &lookIssued);
    }

    if ((packageChecked || restrainedIssued || faceIssued || lookIssued) && !g_state.traceRequestId.empty())
    {
        TraceRequestEvent(g_state.traceRequestId, "conversation_hold_refreshed",
            {
                { "npc_key", hold.npcKey },
                { "npc_name", hold.npcName },
            },
            {
                { "speaker_ref_id", static_cast<double>(hold.speaker.refId) },
            },
            {
                { "conversation_issued", hold.conversationIssued },
                { "script_package_refresh_issued", packageIssued },
                { "script_package_applied", hold.scriptPackageApplied },
                { "script_package_current_known", packageCurrentKnown },
                { "script_package_current", packageCurrent },
                { "script_package_checked", packageChecked },
                { "preserve_furniture_state", hold.preserveFurnitureState },
                { "conversation_mode_enabled", g_debugConfig.conversationModeEnabled },
                { "conversation_mode_refreshed", modeRefreshed },
                { "conversation_mode_applied", hold.conversationModeApplied },
                { "no_move_package_applied", hold.noMovePackageApplied },
                { "restrained_issued", restrainedIssued },
                { "face_issued", faceIssued },
                { "look_issued", lookIssued },
            });
    }

    if (!g_debugConfig.conversationModeEnabled && !hardHold && hold.releaseTick && now >= hold.releaseTick)
    {
        ReleaseConversationHold("idle_timeout");
    }
}

bool IsMapMarkerRef(TESObjectREFR* ref)
{
    return ref && ref->baseForm && ref->baseForm->refID == 0x10;
}

ExtraMapMarker* GetMapMarkerExtra(TESObjectREFR* ref)
{
    if (!IsMapMarkerRef(ref))
    {
        return nullptr;
    }

    BSExtraData* extra = ref->extraDataList.GetByType(kExtraData_MapMarker);
    return extra ? reinterpret_cast<ExtraMapMarker*>(extra) : nullptr;
}

std::string GetMapMarkerDisplayName(TESObjectREFR* ref)
{
    ExtraMapMarker* marker = GetMapMarkerExtra(ref);
    if (marker && marker->data)
    {
        const std::string markerName = GetStringValueSafe(marker->data->fullName.name);
        if (!markerName.empty())
        {
            return markerName;
        }
    }

    return GetFormNameSafe(ref);
}

bool IsLandmarkBaseType(UInt8 typeId)
{
    switch (typeId)
    {
    case kFormType_TESObjectACTI:
    case kFormType_BGSTalkingActivator:
    case kFormType_BGSTerminal:
    case kFormType_TESObjectCONT:
    case kFormType_TESObjectDOOR:
    case kFormType_TESObjectSTAT:
    case kFormType_BGSMovableStatic:
    case kFormType_TESFurniture:
    case kFormType_TESObjectTREE:
    case kFormType_TESFlora:
        return true;
    default:
        return false;
    }
}

bool IsCandidateLandmarkRef(TESObjectREFR* ref)
{
    if (!ref || ref == GetPlayer() || !ref->baseForm)
    {
        return false;
    }

    if (IsMapMarkerRef(ref))
    {
        return false;
    }

    if (!IsLandmarkBaseType(ref->baseForm->typeID))
    {
        return false;
    }

    const std::string name = GetFormNameSafe(ref);
    if (name.empty())
    {
        return false;
    }

    const std::string slug = Slugify(name);
    if (slug.empty() || slug == "door" || slug == "container" || slug == "activator")
    {
        return false;
    }

    static const char* kRejectedLandmarkTerms[] = {
        "chair", "stool", "bench", "table", "desk", "bed", "booth", "crate",
        "barrel", "sack", "campfire", "bottle", "cup", "mug", "plate", "fork",
        "spoon", "knife", "lantern", "rock", "rubble", "trash", "poster",
        "signpost", "easypete", "easy_pete", "sunny", "trudy", "chet", "ringo",
        "victor", "docmitchell", "doc_mitchell", "goodsprings settler"
    };

    for (const char* rejected : kRejectedLandmarkTerms)
    {
        if (slug.find(rejected) != std::string::npos)
        {
            return false;
        }
    }

    return true;
}

std::string FindNearestWorldMapLocation(PlayerCharacter* player)
{
    if (!player || !player->parentCell || !player->parentCell->worldSpace)
    {
        return "";
    }

    TESWorldSpace* currentWorld = player->parentCell->worldSpace;
    if (!currentWorld || !currentWorld->cellMap)
    {
        return "";
    }

    const auto parentCellCoordinates = GetWorldCellCoordinates(player->parentCell);
    if (!parentCellCoordinates.has_value())
    {
        return "";
    }

    const SInt32 centerX = parentCellCoordinates->first;
    const SInt32 centerY = parentCellCoordinates->second;
    constexpr SInt32 kSearchDepth = 8;
    double bestDistance = DBL_MAX;
    std::string bestName;

    for (SInt32 y = centerY - kSearchDepth; y <= centerY + kSearchDepth; ++y)
    {
        for (SInt32 x = centerX - kSearchDepth; x <= centerX + kSearchDepth; ++x)
        {
            TESObjectCELL* cell = currentWorld->cellMap->Lookup(MakeWorldCellKey(x, y));
            if (!cell)
            {
                continue;
            }

            for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
            {
                TESObjectREFR* ref = *iter;
                if (!ref || !IsMapMarkerRef(ref))
                {
                    continue;
                }

                const std::string name = GetMapMarkerDisplayName(ref);
                if (name.empty())
                {
                    continue;
                }

                const double distance = DistanceSquared3D(player, ref);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestName = name;
                }
            }
        }
    }

    return bestName;
}

std::string FindNearestLocalMapLocation(PlayerCharacter* player)
{
    if (!player || !player->parentCell)
    {
        return "";
    }

    TESObjectCELL* cell = player->parentCell;
    if (cell->worldSpace == nullptr)
    {
        const std::string inferred = InferMinorLocationFromCellIdentifier(GetFormNameSafe(cell));
        if (!inferred.empty())
        {
            return inferred;
        }
    }

    double bestDistance = DBL_MAX;
    std::string bestName;

    for (auto iter = cell->objectList.Begin(); !iter.End(); ++iter)
    {
        TESObjectREFR* ref = *iter;
        if (!IsCandidateLandmarkRef(ref))
        {
            continue;
        }

        const std::string name = GetNaturalMinorLocationName(ref);
        if (name.empty())
        {
            continue;
        }
        const double distance = DistanceSquared3D(player, ref);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestName = name;
        }
    }

    if (!bestName.empty())
    {
        const std::string bestSlug = Slugify(bestName);
        if (bestSlug == "easypetechairref" || bestSlug == "easypetechair" || bestSlug == "easypeteboothref")
        {
            bestName.clear();
        }
    }

    if (!bestName.empty())
    {
        const std::string inferred = InferMinorLocationFromCellIdentifier(bestName);
        return inferred.empty() ? bestName : inferred;
    }

    const std::string cellName = GetFormNameSafe(cell);
    return InferMinorLocationFromCellIdentifier(cellName);
}

LocationSnapshot CapturePlayerLocation()
{
    LocationSnapshot snapshot{};
    PlayerCharacter* player = GetPlayer();
    if (!player || !player->parentCell)
    {
        return snapshot;
    }

    TESObjectCELL* cell = player->parentCell;
    snapshot.cell = GetFormNameSafe(cell);
    if (cell->worldSpace)
    {
        snapshot.worldspace = GetFormNameSafe(cell->worldSpace);
    }
    snapshot.major = FindNearestWorldMapLocation(player);
    if (snapshot.major.empty() && cell->worldSpace == nullptr)
    {
        snapshot.major = InferMajorLocationFromCellIdentifier(snapshot.cell);
    }
    snapshot.minor = FindNearestLocalMapLocation(player);
    if (snapshot.minor.empty())
    {
        snapshot.minor = InferMinorLocationFromCellIdentifier(snapshot.cell);
    }

    return snapshot;
}

std::string EscapeForDiag(const std::string& value)
{
    std::string out = value;
    out = ReplaceAll(out, '\r', ' ');
    out = ReplaceAll(out, '\n', ' ');
    return out;
}

std::string ToUiAscii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch < 0x80)
        {
            out.push_back(static_cast<char>(ch));
            continue;
        }

        if (i + 2 < value.size() && ch == 0xE2)
        {
            const unsigned char b1 = static_cast<unsigned char>(value[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(value[i + 2]);
            if (b1 == 0x80 && (b2 == 0x93 || b2 == 0x94))
            {
                out.push_back('-');
                i += 2;
                continue;
            }
            if (b1 == 0x80 && (b2 == 0x98 || b2 == 0x99))
            {
                out.push_back('\'');
                i += 2;
                continue;
            }
            if (b1 == 0x80 && (b2 == 0x9C || b2 == 0x9D))
            {
                out.push_back('"');
                i += 2;
                continue;
            }
            if (b1 == 0x80 && b2 == 0xA6)
            {
                out += "...";
                i += 2;
                continue;
            }
        }

        if (i + 1 < value.size() && ch == 0xC2 && static_cast<unsigned char>(value[i + 1]) == 0xA0)
        {
            out.push_back(' ');
            i += 1;
            continue;
        }

        out.push_back('?');
    }

    return out;
}

bool WriteRequest(const std::string& npcKey, const std::string& npcName, const std::string& text, const LocationSnapshot& location, const std::string& metadataJson, bool clearSpeechSidecar)
{
    EnsureBridgeDirectories();
    StopSpeechAnimation();
    ClearDialogSubtitle();
    g_state.activeRequestId = GenerateRequestId();
    EnsureTraceContext(g_state.activeRequestId);
    g_state.lastAudioChunkIndex = -1;
    g_state.subtitleShownForReply = false;
    g_state.activeSpeechUntilTick = 0;
    g_state.replySubtitleText.clear();
    g_state.streamedAudioSeenForReply = false;
    g_state.pendingAudioChunks.clear();
    StopStreamingVoice("reply_state_reset");
    g_state.replyStartedTick = GetTickCount();
    g_state.lastBridgeActivityTick = g_state.replyStartedTick;
    g_state.sawBridgeActivity = false;

    std::error_code ec;
    fs::remove(InboxPath(), ec);
    if (clearSpeechSidecar)
    {
        fs::remove(SttInboxAudioPath(), ec);
    }
    fs::remove(OutboxPath(), ec);
    if (fs::exists(OutboxChunkDir()))
    {
        for (const auto& entry : fs::directory_iterator(OutboxChunkDir(), ec))
        {
            fs::remove(entry.path(), ec);
        }
    }

    std::ofstream out(InboxPath(), std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LogLine("Failed to open inbox path for request.");
        return false;
    }

    out << g_state.activeRequestId << "\r\n";
    out << SanitizeLine(npcKey) << "\r\n";
    out << SanitizeLine(npcName) << "\r\n";
    out << "1\r\n";
    out << SanitizeLine(text) << "\r\n";
    out << SanitizeLine(location.cell) << "\r\n";
    out << SanitizeLine(location.worldspace) << "\r\n";
    out << SanitizeLine(location.region) << "\r\n";
    out << SanitizeLine(location.major) << "\r\n";
    out << SanitizeLine(location.minor) << "\r\n";
    out << SanitizeLine(metadataJson) << "\r\n";
    out.flush();

    if (!out.good())
    {
        LogLine("Failed while writing request file.");
        return false;
    }

    TraceRequestEvent(g_state.activeRequestId, "request_file_written",
        {
            { "npc_key", npcKey },
            { "npc_name", npcName },
            { "location_major", location.major },
            { "location_minor", location.minor },
            { "location_cell", location.cell },
            { "has_targeting_metadata", metadataJson.empty() ? "0" : "1" },
        },
        {
            { "player_text_length", static_cast<double>(text.size()) },
        });
    WriteRuntimeHeartbeatIfNeeded(true);

    return true;
}

bool WriteVoiceRequest(const std::string& npcKey, const std::string& npcName, const SpeakerSnapshot& speaker, const std::vector<BYTE>& wavBytes, const LocationSnapshot& location, bool adminMode)
{
    if (wavBytes.empty())
    {
        LogLine("Voice request write skipped because WAV payload was empty.");
        return false;
    }

    std::ofstream audioOut(SttInboxAudioPath(), std::ios::binary | std::ios::trunc);
    if (!audioOut)
    {
        LogLine("Failed to open STT audio sidecar path for request.");
        return false;
    }

    audioOut.write(reinterpret_cast<const char*>(wavBytes.data()), static_cast<std::streamsize>(wavBytes.size()));
    audioOut.flush();
    if (!audioOut.good())
    {
        LogLine("Failed while writing STT audio sidecar.");
        return false;
    }

    const std::string metadataJson = adminMode
        ? BuildAdminVoiceRequestMetadata(GetPlayer())
        : BuildTextRequestMetadata(GetPlayer(), &speaker);
    if (!WriteRequest(npcKey, npcName, "", location, metadataJson, false))
    {
        std::error_code ec;
        fs::remove(SttInboxAudioPath(), ec);
        return false;
    }

    TraceRequestEvent(g_state.activeRequestId, "voice_request_audio_written",
        {
            { "npc_key", npcKey },
            { "npc_name", npcName },
            { "location_major", location.major },
            { "location_minor", location.minor },
            { "voice_target", adminMode ? "admin_todd" : "live_chat" },
        },
        {
            { "audio_size_bytes", static_cast<double>(wavBytes.size()) },
            { "speaker_ref_id", static_cast<double>(speaker.refId) },
        });

    return true;
}

std::optional<ResponsePayload> ReadResponse()
{
    const fs::path path = OutboxPath();
    if (!fs::exists(path))
    {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        LogLine("Outbox exists but could not be opened.");
        return std::nullopt;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }

    if (lines.size() < 7)
    {
        return std::nullopt;
    }

    ResponsePayload payload{};
    const std::string statusToken = Trim(lines[0]);
    payload.statusCode = statusToken.empty() ? 0 : std::atoi(statusToken.c_str());
    payload.isFinal = payload.statusCode != 2;
    payload.ok = payload.statusCode == 1 || payload.statusCode == 2;
    payload.requestId = Trim(lines[1]);
    payload.npcKey = Trim(lines[2]);
    payload.npcName = Trim(lines[3]);
    payload.audioFile = Trim(lines[4]);
    payload.text = Trim(lines[5]);
    payload.error = Trim(lines[6]);
    payload.playerText = lines.size() > 8 ? Trim(lines[8]) : "";

    size_t responseIndex = 9;
    bool chunkIndexSeen = false;
    while (responseIndex < lines.size())
    {
        const std::string token = Trim(lines[responseIndex]);
        const size_t equals = token.find('=');
        if (!chunkIndexSeen && IsIntegerToken(token))
        {
            payload.audioChunkIndex = std::atoi(token.c_str());
            chunkIndexSeen = true;
            ++responseIndex;
            continue;
        }
        if (equals != std::string::npos)
        {
            ApplyResponseMetadata(payload, token.substr(0, equals), token.substr(equals + 1));
            ++responseIndex;
            continue;
        }
        break;
    }

    payload.gameMasterAction = responseIndex < lines.size() ? Trim(lines[responseIndex]) : "";
    payload.gameMasterConfidence = responseIndex + 1 < lines.size() ? std::atof(Trim(lines[responseIndex + 1]).c_str()) : 0.0;
    payload.gameMasterShouldTrigger = responseIndex + 2 < lines.size() && Trim(lines[responseIndex + 2]) == "1";

    if (!g_state.activeRequestId.empty() && payload.requestId != g_state.activeRequestId)
    {
        ClearOutboxArtifacts("stale_response");
        LogLine("Ignoring stale outbox response for request %s while awaiting %s.", payload.requestId.c_str(), g_state.activeRequestId.c_str());
        return std::nullopt;
    }

    return payload;
}

std::optional<ResponsePayload> ReadNativeActionCommand(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        LogLine("Native action command exists but could not be opened: %s", path.string().c_str());
        return std::nullopt;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }

    if (!lines.empty() && Trim(lines[0]) == kNativeActionCommandVersion2)
    {
        const auto fields = ParseKeyValueLines(lines, 1);
        ResponsePayload payload{};
        payload.statusCode = 1;
        payload.ok = true;
        payload.isFinal = true;
        payload.requestId = Trim(GetField(fields, "request_id"));
        if (payload.requestId.empty())
        {
            payload.requestId = path.stem().string();
        }
        payload.npcKey = Trim(GetField(fields, "npc_key"));
        payload.npcName = Trim(GetField(fields, "npc_name"));
        payload.actionNpcKey = payload.npcKey;
        payload.actionNpcName = payload.npcName;
        payload.gameMasterAction = ToUpperAscii(Trim(GetField(fields, "action")));
        if (payload.gameMasterAction.empty())
        {
            payload.gameMasterAction = "ACTION_BOOK";
        }
        payload.actionId = Trim(GetField(fields, "action_id"));
        payload.actionBookId = Trim(GetField(fields, "action_book_id"));
        payload.executionEngine = ToLowerAscii(Trim(GetField(fields, "engine")));
        payload.executionTemplateId = Trim(GetField(fields, "template_id"));
        payload.executionLanguage = ToLowerAscii(Trim(GetField(fields, "language")));
        payload.executionArguments = SplitCommaList(GetField(fields, "arguments"));
        payload.gameMasterConfidence = std::atof(Trim(GetField(fields, "confidence")).c_str());
        if (payload.gameMasterConfidence <= 0.0)
        {
            payload.gameMasterConfidence = 1.0;
        }
        payload.gameMasterShouldTrigger = true;
        payload.text = Trim(GetField(fields, "reason"));

        if (const auto decodedPlayerText = DecodeBase64String(GetField(fields, "player_text"), 16ull * 1024ull); decodedPlayerText.has_value())
        {
            payload.playerText = Trim(*decodedPlayerText);
        }
        if (const auto decodedScript = DecodeBase64String(GetField(fields, "script_base64"), kMaxTrustedExecutionScriptBytes); decodedScript.has_value())
        {
            payload.executionScript = *decodedScript;
        }
        else if (!GetField(fields, "script_base64").empty())
        {
            LogLine("Ignoring invalid or oversized trusted execution script in %s.", path.filename().string().c_str());
        }

        if (payload.npcKey.empty() && payload.npcName.empty())
        {
            LogLine("Ignoring native action command %s without an NPC identity.", path.filename().string().c_str());
            return std::nullopt;
        }

        return payload;
    }

    if (lines.size() < 4)
    {
        LogLine("Ignoring malformed native action command %s with %zu line(s).", path.filename().string().c_str(), lines.size());
        return std::nullopt;
    }

    ResponsePayload payload{};
    payload.statusCode = 1;
    payload.ok = true;
    payload.isFinal = true;
    payload.requestId = Trim(lines[0]);
    if (payload.requestId.empty())
    {
        payload.requestId = path.stem().string();
    }
    payload.npcKey = Trim(lines[1]);
    payload.npcName = Trim(lines[2]);
    payload.actionNpcKey = payload.npcKey;
    payload.actionNpcName = payload.npcName;
    payload.gameMasterAction = ToUpperAscii(Trim(lines[3]));
    payload.gameMasterConfidence = lines.size() > 4 ? std::atof(Trim(lines[4]).c_str()) : 1.0;
    payload.gameMasterShouldTrigger = true;
    payload.text = lines.size() > 5 ? Trim(lines[5]) : "";
    payload.playerText = lines.size() > 6 ? Trim(lines[6]) : "";

    if (payload.npcKey.empty() && payload.npcName.empty())
    {
        LogLine("Ignoring native action command %s without an NPC identity.", path.filename().string().c_str());
        return std::nullopt;
    }

    if (payload.gameMasterAction != "ATTACK" && payload.gameMasterAction != "FOLLOW" && payload.gameMasterAction != "STOP_FOLLOW")
    {
        LogLine("Ignoring unsupported native action command %s for %s.", payload.gameMasterAction.c_str(), payload.npcName.c_str());
        return std::nullopt;
    }

    return payload;
}

void PollNativeActionCommands()
{
    const fs::path directory = NativeActionCommandDir();
    if (!fs::exists(directory))
    {
        return;
    }

    std::error_code iterEc;
    for (const auto& entry : fs::directory_iterator(directory, iterEc))
    {
        if (iterEc)
        {
            LogLine("Failed while iterating native action command directory: %s", iterEc.message().c_str());
            break;
        }

        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc))
        {
            continue;
        }

        const std::string extension = ToLowerAscii(entry.path().extension().string());
        if (extension != ".txt")
        {
            continue;
        }

        const auto command = ReadNativeActionCommand(entry.path());
        if (command.has_value())
        {
            EnsureTraceContext(command->requestId);
            std::string triggeredAction;
            const bool triggered = TriggerGameMasterAction(*command, &triggeredAction);
            TraceRequestEvent(command->requestId, "native_action_command_processed",
                {
                    { "file", entry.path().filename().string() },
                    { "npc_key", command->npcKey },
                    { "npc_name", command->npcName },
                    { "game_master_action", command->gameMasterAction },
                    { "action_id", command->actionId },
                    { "action_book_id", command->actionBookId },
                    { "template_id", command->executionTemplateId },
                    { "engine", command->executionEngine },
                    { "triggered_action", triggeredAction },
                },
                {
                    { "game_master_confidence", command->gameMasterConfidence },
                },
                {
                    { "triggered", triggered },
                });
            LogLine("Native action command %s for %s %s.", command->gameMasterAction.c_str(), command->npcName.c_str(), triggered ? "triggered" : "did not trigger");
        }

        std::error_code removeEc;
        fs::remove(entry.path(), removeEc);
        if (removeEc)
        {
            LogLine("Failed to remove native action command %s: %s", entry.path().string().c_str(), removeEc.message().c_str());
        }
    }
}

std::optional<std::string> ReadSubmittedInput()
{
    const fs::path path = UiSubmitPath();
    if (!fs::exists(path))
    {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        LogLine("Submit file exists but could not be opened.");
        return std::nullopt;
    }

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
    {
        text.pop_back();
    }

    return Trim(text);
}

struct WavData
{
    WAVEFORMATEX format{};
    std::vector<BYTE> pcmData;
};

bool ReadUInt32LE(const std::vector<BYTE>& data, size_t offset, UInt32& value)
{
    if (offset + 4 > data.size())
    {
        return false;
    }

    value = static_cast<UInt32>(data[offset]) |
        (static_cast<UInt32>(data[offset + 1]) << 8) |
        (static_cast<UInt32>(data[offset + 2]) << 16) |
        (static_cast<UInt32>(data[offset + 3]) << 24);
    return true;
}

bool ReadUInt16LE(const std::vector<BYTE>& data, size_t offset, UInt16& value)
{
    if (offset + 2 > data.size())
    {
        return false;
    }

    value = static_cast<UInt16>(data[offset]) |
        (static_cast<UInt16>(data[offset + 1]) << 8);
    return true;
}

UInt16 BytesPerAudioSample(UInt16 bitsPerSample)
{
    if (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32 || bitsPerSample == 64)
    {
        return static_cast<UInt16>(bitsPerSample / 8);
    }

    return 0;
}

WORD ResolveWaveFormatTag(const std::vector<BYTE>& bytes, size_t chunkData, UInt32 chunkSize, const WAVEFORMATEX& format)
{
    if (format.wFormatTag != kWaveFormatExtensible || chunkSize < 40)
    {
        return format.wFormatTag;
    }

    UInt32 subFormat = 0;
    if (!ReadUInt32LE(bytes, chunkData + 24, subFormat))
    {
        return format.wFormatTag;
    }

    const WORD subFormatTag = static_cast<WORD>(subFormat & 0xFFFF);
    if (subFormatTag == kWaveFormatPcm || subFormatTag == kWaveFormatIeeeFloat)
    {
        return subFormatTag;
    }

    return format.wFormatTag;
}

double DecodePcmSample(const BYTE* sample, UInt16 bitsPerSample)
{
    switch (bitsPerSample)
    {
    case 8:
        return (static_cast<int>(*sample) - 128) / 128.0;
    case 16:
    {
        const int value = static_cast<int>(sample[0]) | (static_cast<int>(sample[1]) << 8);
        const short signedValue = static_cast<short>(value);
        return static_cast<double>(signedValue) / 32768.0;
    }
    case 24:
    {
        UInt32 rawValue = static_cast<UInt32>(sample[0]) |
            (static_cast<UInt32>(sample[1]) << 8) |
            (static_cast<UInt32>(sample[2]) << 16);
        if (rawValue & 0x00800000)
        {
            rawValue |= 0xFF000000;
        }
        const SInt32 value = static_cast<SInt32>(rawValue);
        return static_cast<double>(value) / 8388608.0;
    }
    case 32:
    {
        const UInt32 rawValue = static_cast<UInt32>(sample[0]) |
            (static_cast<UInt32>(sample[1]) << 8) |
            (static_cast<UInt32>(sample[2]) << 16) |
            (static_cast<UInt32>(sample[3]) << 24);
        const SInt32 value = static_cast<SInt32>(rawValue);
        return static_cast<double>(value) / 2147483648.0;
    }
    default:
        return 0.0;
    }
}

double DecodeFloatSample(const BYTE* sample, UInt16 bitsPerSample)
{
    if (bitsPerSample == 32)
    {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        return std::isfinite(value) ? std::clamp(static_cast<double>(value), -1.0, 1.0) : 0.0;
    }

    if (bitsPerSample == 64)
    {
        double value = 0.0;
        std::memcpy(&value, sample, sizeof(value));
        return std::isfinite(value) ? std::clamp(value, -1.0, 1.0) : 0.0;
    }

    return 0.0;
}

std::vector<float> DecodeMonoSamples(const WAVEFORMATEX& format, WORD resolvedFormatTag, const std::vector<BYTE>& pcmData)
{
    std::vector<float> monoSamples;
    if (pcmData.empty() || !format.nChannels || !format.nSamplesPerSec)
    {
        return monoSamples;
    }

    const UInt16 bytesPerSample = BytesPerAudioSample(format.wBitsPerSample);
    if (!bytesPerSample)
    {
        return monoSamples;
    }

    const UInt16 fallbackBlockAlign = static_cast<UInt16>(format.nChannels * bytesPerSample);
    const UInt16 blockAlign = format.nBlockAlign >= fallbackBlockAlign ? format.nBlockAlign : fallbackBlockAlign;
    if (!blockAlign)
    {
        return monoSamples;
    }

    const size_t frameCount = pcmData.size() / blockAlign;
    monoSamples.reserve(frameCount);
    for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
    {
        const BYTE* frame = pcmData.data() + (frameIndex * blockAlign);
        double sum = 0.0;
        for (UInt16 channel = 0; channel < format.nChannels; ++channel)
        {
            const size_t sampleOffset = static_cast<size_t>(channel) * bytesPerSample;
            if (sampleOffset + bytesPerSample > blockAlign)
            {
                continue;
            }

            const BYTE* sample = frame + sampleOffset;
            if (resolvedFormatTag == kWaveFormatPcm)
            {
                sum += DecodePcmSample(sample, format.wBitsPerSample);
            }
            else if (resolvedFormatTag == kWaveFormatIeeeFloat)
            {
                sum += DecodeFloatSample(sample, format.wBitsPerSample);
            }
        }

        monoSamples.push_back(static_cast<float>(std::clamp(sum / static_cast<double>(format.nChannels), -1.0, 1.0)));
    }

    return monoSamples;
}

std::vector<float> ResampleMonoSamples(const std::vector<float>& samples, DWORD sourceRate, DWORD targetRate)
{
    if (samples.empty() || !sourceRate || !targetRate || sourceRate == targetRate || samples.size() < 2)
    {
        return samples;
    }

    const double targetCountExact = static_cast<double>(samples.size()) * static_cast<double>(targetRate) / static_cast<double>(sourceRate);
    const size_t targetCount = (std::max<size_t>)(1, static_cast<size_t>(targetCountExact + 0.5));
    std::vector<float> output;
    output.reserve(targetCount);

    const double step = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
    for (size_t index = 0; index < targetCount; ++index)
    {
        const double sourcePosition = static_cast<double>(index) * step;
        const size_t left = (std::min<size_t>)(static_cast<size_t>(sourcePosition), samples.size() - 1);
        const size_t right = (std::min<size_t>)(left + 1, samples.size() - 1);
        const double fraction = sourcePosition - static_cast<double>(left);
        const double value = (static_cast<double>(samples[left]) * (1.0 - fraction)) + (static_cast<double>(samples[right]) * fraction);
        output.push_back(static_cast<float>(std::clamp(value, -1.0, 1.0)));
    }

    return output;
}

void ConditionPlaybackSamples(std::vector<float>& samples, DWORD sampleRate)
{
    (void)sampleRate;
    if (samples.empty())
    {
        return;
    }

    // Per-chunk edge fades removed: these chunks are consecutive slices of ONE
    // continuous streaming buffer, so fading each chunk's first/last few ms ate the
    // soft onsets of words landing at chunk seams (and the line's very first word) —
    // most audible on PocketTTS, whose onsets ramp up gently from near-zero. Click
    // protection at the true start/end of a line is handled upstream by the TTS
    // server's lead-in / trailing silence pads. Only the loudness clamp remains.
    for (float& sample : samples)
    {
        sample = static_cast<float>(std::clamp(static_cast<double>(sample) * kVoicePlaybackHeadroom, -0.98, 0.98));
    }
}

std::vector<BYTE> EncodeInt16MonoSamples(const std::vector<float>& samples)
{
    std::vector<BYTE> pcm;
    pcm.resize(samples.size() * sizeof(short));
    for (size_t index = 0; index < samples.size(); ++index)
    {
        const double sample = std::clamp(static_cast<double>(samples[index]), -0.98, 0.98);
        const short value = static_cast<short>(std::lround(sample * 32767.0));
        pcm[index * 2] = static_cast<BYTE>(value & 0xFF);
        pcm[(index * 2) + 1] = static_cast<BYTE>((value >> 8) & 0xFF);
    }
    return pcm;
}

std::optional<WavData> PrepareWavForPlayback(const WAVEFORMATEX& sourceFormat, WORD resolvedFormatTag, const std::vector<BYTE>& sourcePcmData, const fs::path& path)
{
    if (resolvedFormatTag != kWaveFormatPcm && resolvedFormatTag != kWaveFormatIeeeFloat)
    {
        LogLine("Unsupported WAV format %u for game playback: %s", static_cast<unsigned>(resolvedFormatTag), path.string().c_str());
        return std::nullopt;
    }

    std::vector<float> monoSamples = DecodeMonoSamples(sourceFormat, resolvedFormatTag, sourcePcmData);
    if (monoSamples.empty())
    {
        LogLine("Could not decode WAV samples for game playback: %s", path.string().c_str());
        return std::nullopt;
    }

    monoSamples = ResampleMonoSamples(monoSamples, sourceFormat.nSamplesPerSec, kVoicePlaybackSampleRate);
    ConditionPlaybackSamples(monoSamples, kVoicePlaybackSampleRate);

    WavData data{};
    data.format.wFormatTag = WAVE_FORMAT_PCM;
    data.format.nChannels = kVoicePlaybackChannels;
    data.format.nSamplesPerSec = kVoicePlaybackSampleRate;
    data.format.wBitsPerSample = kVoicePlaybackBitsPerSample;
    data.format.nBlockAlign = static_cast<WORD>((data.format.nChannels * data.format.wBitsPerSample) / 8);
    data.format.nAvgBytesPerSec = data.format.nSamplesPerSec * data.format.nBlockAlign;
    data.format.cbSize = 0;
    data.pcmData = EncodeInt16MonoSamples(monoSamples);
    return data;
}

std::optional<WavData> LoadWavFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        LogLine("Could not open WAV file: %s", path.string().c_str());
        return std::nullopt;
    }

    // Block read (single read() call) rather than std::istreambuf_iterator: the
    // iterator extracts byte-by-byte (plus vector regrowth) and cost ~20 ms per
    // ~130 KB streamed chunk INSIDE the game process — a per-chunk frame hitch on
    // every 1.5 s slice. tellg()+read() is ~1 ms.
    const std::streamoff fileSize = in.tellg();
    if (fileSize <= 0)
    {
        LogLine("WAV file empty or unreadable: %s", path.string().c_str());
        return std::nullopt;
    }
    in.seekg(0, std::ios::beg);
    std::vector<BYTE> bytes(static_cast<size_t>(fileSize));
    in.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    bytes.resize(static_cast<size_t>(in.gcount()));
    if (bytes.size() < 44)
    {
        LogLine("WAV file too small: %s", path.string().c_str());
        return std::nullopt;
    }

    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        LogLine("Invalid WAV header: %s", path.string().c_str());
        return std::nullopt;
    }

    size_t offset = 12;
    std::optional<WAVEFORMATEX> format;
    WORD resolvedFormatTag = kWaveFormatPcm;
    std::vector<BYTE> pcmData;

    while (offset + 8 <= bytes.size())
    {
        const char* chunkId = reinterpret_cast<const char*>(bytes.data() + offset);
        UInt32 chunkSize = 0;
        if (!ReadUInt32LE(bytes, offset + 4, chunkSize))
        {
            break;
        }

        const size_t chunkData = offset + 8;
        if (chunkData + chunkSize > bytes.size())
        {
            break;
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16)
        {
            WAVEFORMATEX fmt{};
            ReadUInt16LE(bytes, chunkData + 0, fmt.wFormatTag);
            ReadUInt16LE(bytes, chunkData + 2, fmt.nChannels);
            ReadUInt32LE(bytes, chunkData + 4, fmt.nSamplesPerSec);
            ReadUInt32LE(bytes, chunkData + 8, fmt.nAvgBytesPerSec);
            ReadUInt16LE(bytes, chunkData + 12, fmt.nBlockAlign);
            ReadUInt16LE(bytes, chunkData + 14, fmt.wBitsPerSample);
            fmt.cbSize = 0;
            if (chunkSize >= 18)
            {
                ReadUInt16LE(bytes, chunkData + 16, fmt.cbSize);
            }
            resolvedFormatTag = ResolveWaveFormatTag(bytes, chunkData, chunkSize, fmt);
            format = fmt;
        }
        else if (std::memcmp(chunkId, "data", 4) == 0)
        {
            pcmData.assign(bytes.begin() + static_cast<std::ptrdiff_t>(chunkData),
                bytes.begin() + static_cast<std::ptrdiff_t>(chunkData + chunkSize));
        }

        offset = chunkData + chunkSize + (chunkSize % 2);
    }

    if (!format.has_value() || pcmData.empty())
    {
        LogLine("Missing WAV fmt/data chunk: %s", path.string().c_str());
        return std::nullopt;
    }

    return PrepareWavForPlayback(format.value(), resolvedFormatTag, pcmData, path);
}

DWORD GetWavDurationMs(const WavData& wavData)
{
    if (!wavData.format.nAvgBytesPerSec)
    {
        return 0;
    }

    const double seconds = static_cast<double>(wavData.pcmData.size()) / static_cast<double>(wavData.format.nAvgBytesPerSec);
    if (seconds <= 0.0)
    {
        return 0;
    }

    return static_cast<DWORD>((seconds * 1000.0) + 0.5);
}

bool EnsureDirectSound()
{
    if (g_state.directSound)
    {
        if (g_debugConfig.directSound3dEnabled && !g_state.listener3d)
        {
            ShutdownDirectSound();
        }
        else
        {
            return true;
        }
    }

    if (g_state.directSound)
    {
        return true;
    }

    HWND hwnd = GetGameWindow();
    if (!hwnd)
    {
        LogLine("Cannot initialize DirectSound without a game window.");
        return false;
    }

    IDirectSound8* directSound = nullptr;
    HRESULT hr = DirectSoundCreate8(nullptr, &directSound, nullptr);
    if (FAILED(hr) || !directSound)
    {
        LogLine("DirectSoundCreate8 failed: 0x%08X", hr);
        return false;
    }

    hr = directSound->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
    if (FAILED(hr))
    {
        LogLine("SetCooperativeLevel failed: 0x%08X", hr);
        directSound->Release();
        return false;
    }

    DSBUFFERDESC primaryDesc{};
    primaryDesc.dwSize = sizeof(primaryDesc);
    primaryDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    if (g_debugConfig.directSound3dEnabled)
    {
        primaryDesc.dwFlags |= DSBCAPS_CTRL3D;
    }

    IDirectSoundBuffer* primaryBuffer = nullptr;
    hr = directSound->CreateSoundBuffer(&primaryDesc, &primaryBuffer, nullptr);
    if (FAILED(hr) || !primaryBuffer)
    {
        LogLine("Primary CreateSoundBuffer failed: 0x%08X", hr);
        directSound->Release();
        return false;
    }

    WAVEFORMATEX primaryFormat{};
    primaryFormat.wFormatTag = WAVE_FORMAT_PCM;
    primaryFormat.nChannels = 2;
    primaryFormat.nSamplesPerSec = kVoicePlaybackSampleRate;
    primaryFormat.wBitsPerSample = kVoicePlaybackBitsPerSample;
    primaryFormat.nBlockAlign = static_cast<WORD>((primaryFormat.nChannels * primaryFormat.wBitsPerSample) / 8);
    primaryFormat.nAvgBytesPerSec = primaryFormat.nSamplesPerSec * primaryFormat.nBlockAlign;
    hr = primaryBuffer->SetFormat(&primaryFormat);
    if (FAILED(hr))
    {
        LogLine("Primary SetFormat failed, continuing with device default: 0x%08X", hr);
    }

    IDirectSound3DListener* listener3d = nullptr;
    if (g_debugConfig.directSound3dEnabled)
    {
        hr = primaryBuffer->QueryInterface(IID_IDirectSound3DListener, reinterpret_cast<void**>(&listener3d));
        if (FAILED(hr) || !listener3d)
        {
            LogLine("Primary QueryInterface(IDirectSound3DListener) failed: 0x%08X", hr);
            primaryBuffer->Release();
            directSound->Release();
            return false;
        }
    }

    g_state.directSound = directSound;
    g_state.primaryBuffer = primaryBuffer;
    g_state.listener3d = listener3d;
    return true;
}

void UpdateListener3d(const PlayerCharacter* player)
{
    if (!g_debugConfig.listenerUpdatesEnabled || !g_state.listener3d || !player)
    {
        return;
    }

    const float listenerX = player->posX / kGameUnitsPerMeter;
    const float listenerY = player->posY / kGameUnitsPerMeter;
    const float listenerZ = player->posZ / kGameUnitsPerMeter;
    const float forwardX = -std::sin(player->rotZ);
    const float forwardY = -std::cos(player->rotZ);
    g_state.listener3d->SetPosition(listenerX, listenerY, listenerZ, DS3D_IMMEDIATE);
    g_state.listener3d->SetOrientation(forwardX, forwardY, 0.0f, 0.0f, 0.0f, 1.0f, DS3D_IMMEDIATE);
}

void UpdateActiveSoundPositions()
{
    if (g_state.activeSounds.empty())
    {
        return;
    }

    const PlayerCharacter* player = GetPlayer();
    UpdateListener3d(player);

    if (!g_debugConfig.listenerUpdatesEnabled)
    {
        return;
    }

    for (auto& sound : g_state.activeSounds)
    {
        if (!sound.buffer || !sound.speaker.valid || !sound.speaker.refId)
        {
            continue;
        }

        TESObjectREFR* speakerRef = ResolveSpeakerRef(sound.speaker);
        if (!speakerRef)
        {
            continue;
        }

        const SpeakerSnapshot liveSpeaker = CaptureSpeakerSnapshot(speakerRef);
        if (!liveSpeaker.valid)
        {
            continue;
        }

        if (!sound.buffer3d)
        {
            continue;
        }

        const float emitterX = liveSpeaker.posX / kGameUnitsPerMeter;
        const float emitterY = liveSpeaker.posY / kGameUnitsPerMeter;
        const float emitterZ = liveSpeaker.posZ / kGameUnitsPerMeter;
        sound.buffer3d->SetPosition(emitterX, emitterY, emitterZ, DS3D_IMMEDIATE);
        sound.buffer->SetVolume(ComputeDistanceAttenuatedVolume(player, liveSpeaker));
        sound.speaker = liveSpeaker;
    }
}

void CleanupFinishedSounds()
{
    auto it = g_state.activeSounds.begin();
    while (it != g_state.activeSounds.end())
    {
        DWORD status = 0;
        if (!it->buffer || FAILED(it->buffer->GetStatus(&status)) || !(status & DSBSTATUS_PLAYING))
        {
            if (it->buffer)
            {
                it->buffer->Release();
                it->buffer = nullptr;
            }
            if (it->buffer3d)
            {
                it->buffer3d->Release();
                it->buffer3d = nullptr;
            }
            it = g_state.activeSounds.erase(it);
            continue;
        }
        ++it;
    }

    if (g_state.activeSounds.empty() && (g_state.listener3d || g_state.primaryBuffer || g_state.directSound))
    {
        const DWORD now = GetTickCount();
        const bool replyStillBusy = g_state.awaitingReply
            || !g_state.pendingAudioChunks.empty()
            || g_state.streamActive // Phase 3: the single streaming buffer lives outside
                                    // activeSounds; never release the device under it.
            || (g_state.activeSpeechUntilTick && now < g_state.activeSpeechUntilTick);
        if (replyStillBusy)
        {
            g_state.directSoundIdleSinceTick = 0;
            return;
        }
        if (!g_state.directSoundIdleSinceTick)
        {
            g_state.directSoundIdleSinceTick = now;
            return;
        }
        if (now - g_state.directSoundIdleSinceTick < kDirectSoundIdleReleaseDelayMs)
        {
            return;
        }

        g_state.streamedAudioSeenForReply = false;
        ShutdownDirectSound();
    }
}

bool PlayVoiceWav(const fs::path& wavPath, const SpeakerSnapshot& speaker, const WavData* preloadedWavData = nullptr, bool force2d = false)
{
    if (!EnsureDirectSound())
    {
        return false;
    }

    std::optional<WavData> loadedWavData;
    const WavData* wavData = preloadedWavData;
    if (!wavData)
    {
        loadedWavData = LoadWavFile(wavPath);
        if (!loadedWavData.has_value())
        {
            return false;
        }
        wavData = &*loadedWavData;
    }
    if (!wavData)
    {
        return false;
    }

    const bool use3d = g_debugConfig.directSound3dEnabled && !force2d;
    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS;
    if (use3d)
    {
        desc.dwFlags |= DSBCAPS_CTRL3D;
    }
    if (g_debugConfig.directSoundSoftwareBufferEnabled)
    {
        desc.dwFlags |= DSBCAPS_LOCSOFTWARE;
    }
    desc.dwBufferBytes = static_cast<DWORD>(wavData->pcmData.size());
    desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&wavData->format);

    IDirectSoundBuffer* rawBuffer = nullptr;
    HRESULT hr = g_state.directSound->CreateSoundBuffer(&desc, &rawBuffer, nullptr);
    if (FAILED(hr) || !rawBuffer)
    {
        LogLine("CreateSoundBuffer failed: 0x%08X", hr);
        return false;
    }

    IDirectSoundBuffer8* buffer = nullptr;
    hr = rawBuffer->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&buffer));
    rawBuffer->Release();
    if (FAILED(hr) || !buffer)
    {
        LogLine("QueryInterface(IDirectSoundBuffer8) failed: 0x%08X", hr);
        return false;
    }

    void* lockPtr1 = nullptr;
    void* lockPtr2 = nullptr;
    DWORD lockSize1 = 0;
    DWORD lockSize2 = 0;
    hr = buffer->Lock(0, desc.dwBufferBytes, &lockPtr1, &lockSize1, &lockPtr2, &lockSize2, 0);
    if (FAILED(hr))
    {
        LogLine("DirectSound buffer lock failed: 0x%08X", hr);
        buffer->Release();
        return false;
    }

    std::memcpy(lockPtr1, wavData->pcmData.data(), lockSize1);
    if (lockPtr2 && lockSize2)
    {
        std::memcpy(lockPtr2, wavData->pcmData.data() + lockSize1, lockSize2);
    }
    buffer->Unlock(lockPtr1, lockSize1, lockPtr2, lockSize2);

    const PlayerCharacter* player = GetPlayer();
    if (use3d)
    {
        UpdateListener3d(player);
    }

    IDirectSound3DBuffer* buffer3d = nullptr;
    if (use3d)
    {
        hr = buffer->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&buffer3d));
        if (FAILED(hr) || !buffer3d)
        {
            LogLine("QueryInterface(IDirectSound3DBuffer) failed: 0x%08X", hr);
            buffer->Release();
            return false;
        }
    }

    buffer->SetVolume(force2d ? DSBVOLUME_MAX : ComputeDistanceAttenuatedVolume(player, speaker));

    if (buffer3d && speaker.valid)
    {
        const float emitterX = speaker.posX / kGameUnitsPerMeter;
        const float emitterY = speaker.posY / kGameUnitsPerMeter;
        const float emitterZ = speaker.posZ / kGameUnitsPerMeter;
        buffer3d->SetPosition(emitterX, emitterY, emitterZ, DS3D_IMMEDIATE);
    }
    else if (buffer3d)
    {
        buffer3d->SetPosition(0.0f, 1.5f, 0.0f, DS3D_IMMEDIATE);
    }

    if (buffer3d)
    {
        buffer3d->SetMinDistance(kVoiceMinDistanceMeters, DS3D_IMMEDIATE);
        buffer3d->SetMaxDistance(kVoiceMaxDistanceMeters, DS3D_IMMEDIATE);
        buffer3d->SetMode(DS3DMODE_NORMAL, DS3D_IMMEDIATE);
    }
    buffer->SetCurrentPosition(0);

    hr = buffer->Play(0, 0, 0);
    if (FAILED(hr))
    {
        LogLine("DirectSound play failed: 0x%08X", hr);
        if (buffer3d)
        {
            buffer3d->Release();
        }
        buffer->Release();
        return false;
    }

    g_state.directSoundIdleSinceTick = 0;
    g_state.activeSounds.push_back(ActiveSound{ buffer, buffer3d, speaker });
    return true;
}

void ShutdownDirectSound()
{
    // Release the single streaming buffer first so it never dangles past its device
    // (e.g. if the device is torn down for any reason while a stream is live).
    if (g_state.streamActive || g_state.streamBuffer)
    {
        StopStreamingVoice("directsound_shutdown");
    }
    g_state.directSoundIdleSinceTick = 0;
    if (!g_state.traceRequestId.empty() && (g_state.listener3d || g_state.primaryBuffer || g_state.directSound))
    {
        TraceRequestEvent(g_state.traceRequestId, "directsound_shutdown",
            {},
            {},
            {
                { "had_listener", g_state.listener3d != nullptr },
                { "had_primary_buffer", g_state.primaryBuffer != nullptr },
                { "had_device", g_state.directSound != nullptr },
            });
    }
    if (g_state.listener3d)
    {
        g_state.listener3d->Release();
        g_state.listener3d = nullptr;
    }
    if (g_state.primaryBuffer)
    {
        g_state.primaryBuffer->Release();
        g_state.primaryBuffer = nullptr;
    }
    if (g_state.directSound)
    {
        g_state.directSound->Release();
        g_state.directSound = nullptr;
    }
    WriteRuntimeHeartbeatIfNeeded(true);
}

TESObjectREFR* ResolveSpeakerRef(const SpeakerSnapshot& speaker)
{
    if (!speaker.refId)
    {
        return nullptr;
    }

    TESForm* form = LookupFormByIdRuntime(speaker.refId);
    if (!form)
    {
        return nullptr;
    }

    return static_cast<TESObjectREFR*>(form);
}

bool HasPageAccess(DWORD protect, bool requireWrite)
{
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
    {
        return false;
    }

    protect &= 0xFF;

    if (requireWrite)
    {
        return protect == PAGE_READWRITE
            || protect == PAGE_WRITECOPY
            || protect == PAGE_EXECUTE_READWRITE
            || protect == PAGE_EXECUTE_WRITECOPY;
    }

    return protect == PAGE_READONLY
        || protect == PAGE_READWRITE
        || protect == PAGE_WRITECOPY
        || protect == PAGE_EXECUTE_READ
        || protect == PAGE_EXECUTE_READWRITE
        || protect == PAGE_EXECUTE_WRITECOPY;
}

bool IsAccessibleRange(const void* ptr, size_t size, bool requireWrite)
{
    if (!ptr || !size)
    {
        return false;
    }

    auto* cursor = static_cast<const BYTE*>(ptr);
    const auto* end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(cursor, &mbi, sizeof(mbi)))
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT || !HasPageAccess(mbi.Protect, requireWrite))
        {
            return false;
        }

        const auto* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }

        cursor = (std::min)(regionEnd, end);
    }

    return true;
}

template <typename T>
bool TryReadMemory(const void* ptr, T& out)
{
    if (!IsAccessibleRange(ptr, sizeof(T), false))
    {
        return false;
    }

    std::memcpy(&out, ptr, sizeof(T));
    return true;
}

bool IsPlausibleKeyframeValues(const float* values, UInt32 count)
{
    if (!values || !count || count > 32 || !IsAccessibleRange(values, sizeof(float) * count, true))
    {
        return false;
    }

    for (UInt32 i = 0; i < count; ++i)
    {
        const float value = values[i];
        if (!std::isfinite(value) || value < -0.25f || value > 1.25f)
        {
            return false;
        }
    }

    return true;
}

bool DiscoverFaceAnimationBinding(const SpeakerSnapshot& speaker, FaceAnimationBinding& binding)
{
    binding = {};

    auto* speakerRef = ResolveSpeakerRef(speaker);
    auto* actor = static_cast<Actor*>(speakerRef);
    if (!actor || !actor->baseProcess)
    {
        return false;
    }

    if (actor->baseProcess->processLevel > BaseProcess::kProcessLevel_MiddleHigh)
    {
        return false;
    }

    auto* process = static_cast<MiddleHighProcess*>(actor->baseProcess);
    auto* faceNode = reinterpret_cast<FaceGenNiNodeRuntime*>(process->unk248 ? process->unk248 : process->unk24C);
    void* animationData = process->unk178 ? static_cast<void*>(process->unk178) : (faceNode ? static_cast<void*>(faceNode->spAnimationData) : nullptr);
    if (!faceNode || !animationData)
    {
        return false;
    }

    FaceGenKeyframeMultiple32* phonemeKeyframe = nullptr;
    FaceGenKeyframeMultiple32* modifierKeyframe = nullptr;

    for (size_t offset = 0; offset + sizeof(FaceGenKeyframeMultiple32) <= 0x180; offset += sizeof(UInt32))
    {
        auto* candidatePtr = reinterpret_cast<FaceGenKeyframeMultiple32*>(static_cast<BYTE*>(animationData) + offset);
        FaceGenKeyframeMultiple32 candidate{};
        if (!TryReadMemory(candidatePtr, candidate))
        {
            continue;
        }

        if (!candidate.values || candidate.count < 8 || candidate.count > 24)
        {
            continue;
        }

        if (!IsPlausibleKeyframeValues(candidate.values, candidate.count))
        {
            continue;
        }

        if (!phonemeKeyframe && candidate.count == kFaceGenPhonemeCount)
        {
            phonemeKeyframe = candidatePtr;
        }
        else if (!modifierKeyframe && candidate.count == 17)
        {
            modifierKeyframe = candidatePtr;
        }
    }

    if (!phonemeKeyframe || !IsAccessibleRange(phonemeKeyframe, sizeof(FaceGenKeyframeMultiple32), true))
    {
        return false;
    }

    binding.speakerRefId = speaker.refId;
    binding.faceNode = faceNode;
    binding.animationData = animationData;
    binding.phonemeKeyframe = phonemeKeyframe;
    binding.modifierKeyframe = modifierKeyframe;
    return true;
}

bool CanSafelyWriteFaceAnimationBinding(const FaceAnimationBinding& binding)
{
    if (!binding.faceNode || !binding.phonemeKeyframe)
    {
        return false;
    }

    if (!IsAccessibleRange(binding.faceNode, sizeof(FaceGenNiNodeRuntime), true)
        || !IsAccessibleRange(binding.phonemeKeyframe, sizeof(FaceGenKeyframeMultiple32), true))
    {
        return false;
    }

    FaceGenKeyframeMultiple32 keyframe{};
    if (!TryReadMemory(binding.phonemeKeyframe, keyframe))
    {
        return false;
    }

    const UInt32 count = (std::min)(keyframe.count, kFaceGenPhonemeCount);
    if (count && !IsAccessibleRange(keyframe.values, sizeof(float) * count, true))
    {
        return false;
    }

    return true;
}

bool IsCurrentFaceAnimationBinding(const SpeakerSnapshot& speaker, const FaceAnimationBinding& binding)
{
    if (!binding.faceNode || !binding.phonemeKeyframe || !speaker.refId || binding.speakerRefId != speaker.refId)
    {
        return false;
    }

    FaceAnimationBinding current{};
    if (!DiscoverFaceAnimationBinding(speaker, current))
    {
        return false;
    }

    return current.faceNode == binding.faceNode
        && current.animationData == binding.animationData
        && current.phonemeKeyframe == binding.phonemeKeyframe
        && current.modifierKeyframe == binding.modifierKeyframe;
}

void CaptureOriginalFaceAnimationBindingState(ActiveSpeechAnimation& animation)
{
    if (animation.originalBindingStateCaptured || !CanSafelyWriteFaceAnimationBinding(animation.binding))
    {
        return;
    }

    auto* phonemeKeyframe = animation.binding.phonemeKeyframe;
    const UInt32 count = (std::min)(phonemeKeyframe->count, kFaceGenPhonemeCount);
    if (!count || !IsAccessibleRange(phonemeKeyframe->values, sizeof(float) * count, true))
    {
        return;
    }

    animation.originalWeights.fill(0.0f);
    for (UInt32 i = 0; i < count; ++i)
    {
        animation.originalWeights[i] = phonemeKeyframe->values[i];
    }
    animation.originalPhonemeCount = count;
    animation.originalPhonemeIsUpdated = phonemeKeyframe->isUpdated;
    animation.originalFaceAnimationUpdate = animation.binding.faceNode->bAnimationUpdate;
    animation.originalFaceInDialogue = animation.binding.faceNode->bIAmInDialouge;
    animation.originalBindingStateCaptured = true;
}

void ClearFaceAnimationBinding(FaceAnimationBinding& binding)
{
    if (!CanSafelyWriteFaceAnimationBinding(binding))
    {
        binding = {};
        return;
    }

    auto* phonemeKeyframe = binding.phonemeKeyframe;
    const UInt32 count = (std::min)(phonemeKeyframe->count, kFaceGenPhonemeCount);
    if (g_debugConfig.speechWritePhonemeValues
        && count
        && IsAccessibleRange(phonemeKeyframe->values, sizeof(float) * count, true))
    {
        for (UInt32 i = 0; i < count; ++i)
        {
            phonemeKeyframe->values[i] = 0.0f;
        }
    }

    if (g_debugConfig.speechWriteFaceFlags)
    {
        binding.faceNode->bAnimationUpdate = 1;
        binding.faceNode->bIAmInDialouge = 0;
    }
    binding = {};
}

void RestoreFaceAnimationBinding(FaceAnimationBinding& binding, const ActiveSpeechAnimation& animation)
{
    if (!animation.originalBindingStateCaptured || !CanSafelyWriteFaceAnimationBinding(binding))
    {
        binding = {};
        return;
    }

    auto* phonemeKeyframe = binding.phonemeKeyframe;
    const UInt32 count = (std::min)(phonemeKeyframe->count, animation.originalPhonemeCount);
    if (count && IsAccessibleRange(phonemeKeyframe->values, sizeof(float) * count, true))
    {
        for (UInt32 i = 0; i < count; ++i)
        {
            const float prev = animation.lastWeights[i] < 0.0f ? 0.0f : animation.lastWeights[i];
            if (prev > 0.001f)
            {
                phonemeKeyframe->values[i] = animation.originalWeights[i];
            }
        }
    }

    binding.faceNode->bAnimationUpdate = animation.originalFaceAnimationUpdate;
    binding.faceNode->bIAmInDialouge = animation.originalFaceInDialogue;
    binding = {};
}

void ApplySpeechWeights(ActiveSpeechAnimation& animation, const std::array<float, kFaceGenPhonemeCount>& weights)
{
    auto& binding = animation.binding;
    if (!binding.faceNode || !binding.phonemeKeyframe)
    {
        return;
    }

    auto* phonemeKeyframe = binding.phonemeKeyframe;
    const UInt32 count = (std::min)(phonemeKeyframe->count, kFaceGenPhonemeCount);
    if (!count || !IsAccessibleRange(phonemeKeyframe->values, sizeof(float) * count, true))
    {
        return;
    }

    if (g_debugConfig.speechWritePhonemeValues)
    {
        for (UInt32 i = 0; i < count; ++i)
        {
            const float prev = animation.lastWeights[i] < 0.0f ? 0.0f : animation.lastWeights[i];
            const float next = weights[i];
            if (prev <= 0.001f && next <= 0.001f)
            {
                continue;
            }

            float target = animation.originalWeights[i];
            if (next > 0.001f)
            {
                target = (std::clamp)((std::max)(target, next), 0.0f, 1.0f);
            }
            phonemeKeyframe->values[i] = target;
        }
    }

    if (g_debugConfig.speechWriteFaceFlags)
    {
        binding.faceNode->bAnimationUpdate = 1;
        binding.faceNode->bIAmInDialouge = 1;
    }
}

std::vector<float> BuildSpeechEnvelope(const WavData& wavData, DWORD windowMs)
{
    std::vector<float> envelope;
    if (wavData.pcmData.empty() || !wavData.format.nChannels || !wavData.format.nSamplesPerSec || !wavData.format.nBlockAlign)
    {
        return envelope;
    }

    const UInt16 bitsPerSample = wavData.format.wBitsPerSample;
    if (bitsPerSample != 8 && bitsPerSample != 16)
    {
        return envelope;
    }

    const size_t totalFrames = wavData.pcmData.size() / wavData.format.nBlockAlign;
    if (!totalFrames)
    {
        return envelope;
    }

    const size_t framesPerWindow = (std::max<size_t>)(1, static_cast<size_t>((static_cast<unsigned long long>(wavData.format.nSamplesPerSec) * windowMs) / 1000ULL));
    envelope.reserve((totalFrames + framesPerWindow - 1) / framesPerWindow);

    float peak = 0.0f;
    for (size_t frameStart = 0; frameStart < totalFrames; frameStart += framesPerWindow)
    {
        const size_t frameEnd = (std::min)(frameStart + framesPerWindow, totalFrames);
        double windowTotal = 0.0;
        size_t windowSamples = 0;

        for (size_t frameIndex = frameStart; frameIndex < frameEnd; ++frameIndex)
        {
            const BYTE* framePtr = wavData.pcmData.data() + (frameIndex * wavData.format.nBlockAlign);
            for (UInt16 channel = 0; channel < wavData.format.nChannels; ++channel)
            {
                double sample = 0.0;
                if (bitsPerSample == 16)
                {
                    const short pcmSample = *reinterpret_cast<const short*>(framePtr + (channel * sizeof(short)));
                    sample = std::abs(static_cast<double>(pcmSample) / 32768.0);
                }
                else
                {
                    const int pcmSample = static_cast<int>(framePtr[channel]) - 128;
                    sample = std::abs(static_cast<double>(pcmSample) / 128.0);
                }

                windowTotal += sample;
                ++windowSamples;
            }
        }

        const float amplitude = windowSamples ? static_cast<float>(windowTotal / static_cast<double>(windowSamples)) : 0.0f;
        peak = (std::max)(peak, amplitude);
        envelope.push_back(amplitude);
    }

    if (peak > 0.0f)
    {
        for (float& amplitude : envelope)
        {
            const float normalised = amplitude / peak;
            amplitude = std::pow((std::clamp)(normalised, 0.0f, 1.0f), 0.65f);
        }
    }

    return envelope;
}

bool IsVowelPhoneme(UInt32 phonemeId)
{
    switch (phonemeId)
    {
    case 0:
    case 1:
    case 5:
    case 6:
    case 8:
    case 11:
    case 12:
        return true;
    default:
        return false;
    }
}

std::vector<UInt32> BuildPhonemeSequenceFromText(const std::string& text)
{
    std::string normalised;
    normalised.reserve(text.size());
    for (unsigned char ch : text)
    {
        if (std::isalpha(ch))
        {
            normalised.push_back(static_cast<char>(std::tolower(ch)));
        }
        else if (std::isspace(ch))
        {
            normalised.push_back(' ');
        }
    }

    std::vector<UInt32> sequence;
    sequence.reserve(normalised.size());
    for (size_t i = 0; i < normalised.size();)
    {
        const char ch = normalised[i];
        const char next = (i + 1 < normalised.size()) ? normalised[i + 1] : '\0';
        if (ch == ' ')
        {
            ++i;
            continue;
        }

        if (ch == 't' && next == 'h')
        {
            sequence.push_back(14);
            i += 2;
            continue;
        }

        if ((ch == 's' && next == 'h') || (ch == 'c' && next == 'h') || (ch == 'j' && next == 'h'))
        {
            sequence.push_back(3);
            i += 2;
            continue;
        }

        if (ch == 'o' && next == 'o')
        {
            sequence.push_back(12);
            i += 2;
            continue;
        }

        if (ch == 'b' || ch == 'm' || ch == 'p')
        {
            sequence.push_back(2);
        }
        else if (ch == 'f' || ch == 'v')
        {
            sequence.push_back(7);
        }
        else if (ch == 'c' || ch == 'k' || ch == 'g' || ch == 'q')
        {
            sequence.push_back(9);
        }
        else if (ch == 'n')
        {
            sequence.push_back(10);
        }
        else if (ch == 'r')
        {
            sequence.push_back(13);
        }
        else if (ch == 'w')
        {
            sequence.push_back(15);
        }
        else if (ch == 'a')
        {
            sequence.push_back(0);
        }
        else if (ch == 'e')
        {
            sequence.push_back(5);
        }
        else if (ch == 'i' || ch == 'y')
        {
            sequence.push_back(8);
        }
        else if (ch == 'o')
        {
            sequence.push_back(11);
        }
        else if (ch == 'u')
        {
            sequence.push_back(12);
        }
        else if (ch == 'd' || ch == 't' || ch == 'l' || ch == 's' || ch == 'x' || ch == 'z')
        {
            sequence.push_back(4);
        }
        else if (ch == 'j' || ch == 'h')
        {
            sequence.push_back(3);
        }

        ++i;
    }

    if (sequence.empty())
    {
        sequence = { 0, 11, 5 };
    }

    return sequence;
}

std::vector<VisemeCue> BuildVisemeTimeline(const std::string& text, DWORD durationMs)
{
    std::vector<VisemeCue> visemes;
    if (!durationMs)
    {
        return visemes;
    }

    auto sequence = BuildPhonemeSequenceFromText(text);
    if (sequence.empty())
    {
        return visemes;
    }

    const size_t maxCueCount = (std::max<size_t>)(1, static_cast<size_t>(durationMs / 70));
    if (sequence.size() > maxCueCount)
    {
        std::vector<UInt32> reduced;
        reduced.reserve(maxCueCount);
        for (size_t index = 0; index < maxCueCount; ++index)
        {
            const size_t sourceIndex = (index * sequence.size()) / maxCueCount;
            reduced.push_back(sequence[sourceIndex]);
        }
        sequence = std::move(reduced);
    }

    visemes.reserve(sequence.size());
    for (size_t index = 0; index < sequence.size(); ++index)
    {
        const DWORD startMs = static_cast<DWORD>((static_cast<unsigned long long>(durationMs) * index) / sequence.size());
        const DWORD endMs = static_cast<DWORD>((static_cast<unsigned long long>(durationMs) * (index + 1)) / sequence.size());
        const UInt32 phonemeId = sequence[index];
        visemes.push_back(VisemeCue{
            startMs,
            endMs > startMs ? endMs : (startMs + 1),
            phonemeId,
            IsVowelPhoneme(phonemeId) ? 1.0f : 0.82f,
        });
    }

    return visemes;
}

float SampleSpeechEnvelope(const ActiveSpeechAnimation& animation, DWORD elapsedMs)
{
    if (animation.envelope.empty())
    {
        return 0.0f;
    }

    const size_t index = (std::min<size_t>)(animation.envelope.size() - 1, elapsedMs / animation.envelopeWindowMs);
    return animation.envelope[index];
}

std::array<float, kFaceGenPhonemeCount> BuildSpeechWeights(const ActiveSpeechAnimation& animation, DWORD elapsedMs)
{
    std::array<float, kFaceGenPhonemeCount> weights{};
    if (animation.visemes.empty())
    {
        return weights;
    }

    const float amplitude = SampleSpeechEnvelope(animation, elapsedMs);
    if (amplitude < kSpeechSilenceThreshold)
    {
        return weights;
    }

    size_t cueIndex = animation.visemes.size() - 1;
    for (size_t index = 0; index < animation.visemes.size(); ++index)
    {
        if (elapsedMs < animation.visemes[index].endMs)
        {
            cueIndex = index;
            break;
        }
    }

    const VisemeCue& cue = animation.visemes[cueIndex];
    const float baseWeight = (std::clamp)(kSpeechMinWeight + (amplitude * (kSpeechMaxWeight - kSpeechMinWeight)), 0.0f, kSpeechMaxWeight);
    const float scaledBaseWeight = (std::clamp)(baseWeight * g_debugConfig.speechWeightScale, 0.0f, 1.0f);
    weights[cue.phonemeId] = scaledBaseWeight * cue.emphasis;

    const DWORD cueSpan = cue.endMs > cue.startMs ? (cue.endMs - cue.startMs) : 1;
    if (cueIndex + 1 < animation.visemes.size())
    {
        const float phase = static_cast<float>((elapsedMs > cue.startMs) ? (elapsedMs - cue.startMs) : 0) / static_cast<float>(cueSpan);
        float blend = (std::clamp)((phase - 0.62f) / 0.38f, 0.0f, 1.0f);
        blend = blend * blend * (3.0f - (2.0f * blend));
        weights[cue.phonemeId] *= (1.0f - (blend * 0.35f));
        const VisemeCue& nextCue = animation.visemes[cueIndex + 1];
        weights[nextCue.phonemeId] = (std::max)(weights[nextCue.phonemeId], scaledBaseWeight * 0.68f * blend * nextCue.emphasis);
    }

    return weights;
}

void AbandonSpeechAnimation(const char* reason)
{
    auto animation = std::move(g_state.speechAnimation);
    if (animation.active && !animation.requestId.empty())
    {
        TraceRequestEvent(animation.requestId, "speech_animation_abandoned",
            {
                { "reason", reason ? reason : "" },
            },
            {
                { "speaker_ref_id", static_cast<double>(animation.speaker.refId) },
            });
    }

    g_state.speechAnimation = {};
    g_state.speechAnimation.lastWeights.fill(0.0f);

    if (reason && *reason)
    {
        LogLine("Abandoned speech animation: reason=%s", reason);
    }
}

void StopSpeechAnimation()
{
    if (g_state.speechAnimation.active && !g_state.speechAnimation.requestId.empty())
    {
        TraceRequestEvent(g_state.speechAnimation.requestId, "speech_animation_stopped",
            {},
            {
                { "duration_ms", static_cast<double>(g_state.speechAnimation.durationMs) },
            });
    }

    const bool hasBinding = g_state.speechAnimation.binding.faceNode || g_state.speechAnimation.binding.phonemeKeyframe;
    const bool canClearBinding = hasBinding
        && CanSafelyWriteFaceAnimationBinding(g_state.speechAnimation.binding)
        && IsCurrentFaceAnimationBinding(g_state.speechAnimation.speaker, g_state.speechAnimation.binding);
    if (canClearBinding && g_debugConfig.speechClearBindingOnStop)
    {
        if (g_state.speechAnimation.originalBindingStateCaptured)
        {
            RestoreFaceAnimationBinding(g_state.speechAnimation.binding, g_state.speechAnimation);
        }
        else
        {
            ClearFaceAnimationBinding(g_state.speechAnimation.binding);
        }
    }
    else if (canClearBinding)
    {
        LogLine("Skipped speech binding clear on stop by config for speaker %08X.",
            g_state.speechAnimation.speaker.refId);
    }
    else if (hasBinding)
    {
        LogLine("Skipped clearing stale speech animation binding for speaker %08X.",
            g_state.speechAnimation.speaker.refId);
    }

    g_state.speechAnimation = {};
    g_state.speechAnimation.lastWeights.fill(0.0f);
    WriteRuntimeHeartbeatIfNeeded(true);
}

void StartSpeechAnimation(const WavData& wavData, const QueuedAudioChunk& chunk, const SpeakerSnapshot& speaker, DWORD durationMs)
{
    if (!g_debugConfig.speechAnimationEnabled)
    {
        StopSpeechAnimation();
        if (!chunk.requestId.empty())
        {
            TraceRequestEvent(chunk.requestId, "speech_animation_disabled_by_config",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(speaker.refId) },
                });
        }
        return;
    }

    ActiveSpeechAnimation previous = std::move(g_state.speechAnimation);
    const bool canReuseBinding = previous.active
        && previous.speaker.refId == speaker.refId
        && previous.binding.phonemeKeyframe
        && CanSafelyWriteFaceAnimationBinding(previous.binding);
    if (!canReuseBinding)
    {
        g_state.speechAnimation = std::move(previous);
        StopSpeechAnimation();
    }
    else
    {
        g_state.speechAnimation = {};
    }

    ActiveSpeechAnimation animation{};
    animation.active = durationMs > 0 && speaker.refId != 0;
    animation.requestId = chunk.requestId;
    animation.speaker = speaker;
    animation.startedTick = GetTickCount();
    animation.durationMs = durationMs;
    animation.envelopeWindowMs = kSpeechEnvelopeWindowMs;
    animation.envelope = BuildSpeechEnvelope(wavData, animation.envelopeWindowMs);
    animation.visemes = BuildVisemeTimeline(chunk.subtitleText.empty() ? chunk.audioFile : chunk.subtitleText, durationMs);
    if (canReuseBinding)
    {
        animation.binding = previous.binding;
        animation.originalWeights = previous.originalWeights;
        animation.originalPhonemeCount = previous.originalPhonemeCount;
        animation.originalPhonemeIsUpdated = previous.originalPhonemeIsUpdated;
        animation.originalFaceAnimationUpdate = previous.originalFaceAnimationUpdate;
        animation.originalFaceInDialogue = previous.originalFaceInDialogue;
        animation.originalBindingStateCaptured = previous.originalBindingStateCaptured;
        animation.lastWeights = previous.lastWeights;
        animation.lastBindingValidationTick = previous.lastBindingValidationTick;
    }
    else
    {
        animation.lastWeights.fill(-1.0f);
    }
    g_state.speechAnimation = std::move(animation);

    if (g_state.speechAnimation.active)
    {
        TraceRequestEvent(g_state.speechAnimation.requestId, "speech_animation_started",
            {
                { "audio_file", chunk.audioFile },
            },
            {
                { "duration_ms", static_cast<double>(durationMs) },
                { "envelope_samples", static_cast<double>(g_state.speechAnimation.envelope.size()) },
                { "viseme_count", static_cast<double>(g_state.speechAnimation.visemes.size()) },
                { "speaker_ref_id", static_cast<double>(speaker.refId) },
            });
        if (g_debugConfig.requestTracingEnabled)
        {
            LogLine("Started native speech animation: speaker=%08X duration=%lu envelope=%zu visemes=%zu",
                speaker.refId,
                static_cast<unsigned long>(durationMs),
                g_state.speechAnimation.envelope.size(),
                g_state.speechAnimation.visemes.size());
        }
    }
}

void UpdateSpeechAnimation()
{
    auto& animation = g_state.speechAnimation;
    if (!animation.active)
    {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD elapsedMs = now - animation.startedTick;
    if (elapsedMs > animation.durationMs + kSpeechAnimationTailMs)
    {
        StopSpeechAnimation();
        return;
    }

    if (!animation.binding.phonemeKeyframe)
    {
        if (!animation.lastBindingAttemptTick || (now - animation.lastBindingAttemptTick) >= kSpeechBindingRetryMs)
        {
            animation.lastBindingAttemptTick = now;
            if (!DiscoverFaceAnimationBinding(animation.speaker, animation.binding))
            {
                if (!animation.loggedBindingFailure)
                {
                    TraceRequestEvent(animation.requestId, "speech_face_binding_missing",
                        {},
                        {
                            { "speaker_ref_id", static_cast<double>(animation.speaker.refId) },
                        });
                    LogLine("Could not resolve face animation binding for speaker %08X.", animation.speaker.refId);
                    animation.loggedBindingFailure = true;
                }
                return;
            }

            TraceRequestEvent(animation.requestId, "speech_face_binding_resolved",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(animation.speaker.refId) },
                    { "phoneme_count", static_cast<double>(animation.binding.phonemeKeyframe ? animation.binding.phonemeKeyframe->count : 0) },
                    { "modifier_count", static_cast<double>(animation.binding.modifierKeyframe ? animation.binding.modifierKeyframe->count : 0) },
                });
            LogLine("Resolved face animation binding: speaker=%08X animation=%p phonemeCount=%u modifierCount=%u",
                animation.speaker.refId,
                animation.binding.animationData,
                animation.binding.phonemeKeyframe ? animation.binding.phonemeKeyframe->count : 0,
                animation.binding.modifierKeyframe ? animation.binding.modifierKeyframe->count : 0);
            animation.lastBindingValidationTick = now;
            CaptureOriginalFaceAnimationBindingState(animation);
        }
        else
        {
            return;
        }
    }
    else if (!animation.lastBindingValidationTick
        || (now - animation.lastBindingValidationTick) >= g_debugConfig.speechBindingValidationIntervalMs)
    {
        animation.lastBindingValidationTick = now;
        if (!CanSafelyWriteFaceAnimationBinding(animation.binding)
            || !IsCurrentFaceAnimationBinding(animation.speaker, animation.binding))
        {
            TraceRequestEvent(animation.requestId, "speech_face_binding_invalidated",
                {},
                {
                    { "speaker_ref_id", static_cast<double>(animation.speaker.refId) },
                });
            LogLine("Speech face binding invalidated for speaker %08X; stopping animation.", animation.speaker.refId);
            AbandonSpeechAnimation("binding_invalidated");
            return;
        }
    }

    if (animation.lastWeightsUpdateTick
        && (now - animation.lastWeightsUpdateTick) < g_debugConfig.speechAnimationUpdateIntervalMs)
    {
        return;
    }
    animation.lastWeightsUpdateTick = now;

    auto weights = BuildSpeechWeights(animation, elapsedMs);
    if (!animation.firstWeightsAppliedLogged)
    {
        for (float value : weights)
        {
            if (value > 0.001f)
            {
                animation.firstWeightsAppliedLogged = true;
                TraceRequestEvent(animation.requestId, "speech_first_weights_applied",
                    {},
                    {
                        { "speaker_ref_id", static_cast<double>(animation.speaker.refId) },
                        { "elapsed_since_animation_start_ms", static_cast<double>(elapsedMs) },
                    });
                break;
            }
        }
    }
    if (weights != animation.lastWeights)
    {
        ApplySpeechWeights(animation, weights);
        animation.lastWeights = weights;
    }
}

// ===========================================================================
// Phase 3: single-buffer streaming voice
//
// Instead of one static DirectSound buffer per mini-chunk (the static path below),
// hold ONE non-looping buffer per utterance, write each chunk's PCM into it as it
// arrives, and play it continuously. Audio is buffered ahead of the play cursor
// (seamless); lip-sync is fired per chunk on the playback schedule so the mouth
// stays in sync. Gated on DebugConfig.singleBufferStreaming (off => static path).
// ===========================================================================

// Release the current streaming buffer + lip-sync queue and reset stream state.
void StopStreamingVoice(const char* reason)
{
    const bool wasActive = g_state.streamActive;
    if (g_state.streamBuffer3d)
    {
        g_state.streamBuffer3d->Release();
        g_state.streamBuffer3d = nullptr;
    }
    if (g_state.streamBuffer)
    {
        g_state.streamBuffer->Stop();
        g_state.streamBuffer->Release();
        g_state.streamBuffer = nullptr;
    }
    if (wasActive && reason && !g_state.streamRequestId.empty())
    {
        TraceRequestEvent(g_state.streamRequestId, "stream_voice_stopped",
            { { "reason", reason } },
            { { "written_bytes", static_cast<double>(g_state.streamWriteCursor) } });
    }
    g_state.streamActive = false;
    g_state.streamStarted = false;
    g_state.streamCapacityBytes = 0;
    g_state.streamWriteCursor = 0;
    g_state.streamPlayStartTick = 0;
    g_state.streamLastAppendTick = 0;
    g_state.streamCumulativeMs = 0;
    g_state.streamRequestId.clear();
    g_state.streamSpeaker = {};
    g_state.streamNonPositional = false;
    g_state.streamFormat = {};
    g_state.streamLipSyncQueue.clear();
    g_state.captionMaxChars = -1;
    g_state.captionSegments.clear();
    g_state.captionSegmentStartChar.clear();
    g_state.captionCurrentIndex = -1;
    g_state.captionSourceText.clear();
    g_state.captionTotalMsLocked = 0;
    g_state.captionLastShowTick = 0;
    g_state.lipSyncAccumPcm.clear();
    g_state.lipSyncAccumMs = 0;
    g_state.lipSyncAccumStartMs = 0;
}

// Apply 3D position + distance-attenuated volume to the streaming buffer.
void ApplyStreamingVoiceSpatial(const SpeakerSnapshot& speaker, bool nonPositional)
{
    if (!g_state.streamBuffer)
    {
        return;
    }
    const PlayerCharacter* player = GetPlayer();
    if (g_state.streamBuffer3d && !nonPositional)
    {
        UpdateListener3d(player);
        if (speaker.valid)
        {
            g_state.streamBuffer3d->SetPosition(speaker.posX / kGameUnitsPerMeter,
                speaker.posY / kGameUnitsPerMeter, speaker.posZ / kGameUnitsPerMeter, DS3D_IMMEDIATE);
        }
        g_state.streamBuffer3d->SetMinDistance(kVoiceMinDistanceMeters, DS3D_IMMEDIATE);
        g_state.streamBuffer3d->SetMaxDistance(kVoiceMaxDistanceMeters, DS3D_IMMEDIATE);
        g_state.streamBuffer3d->SetMode(DS3DMODE_NORMAL, DS3D_IMMEDIATE);
    }
    g_state.streamBuffer->SetVolume(nonPositional ? DSBVOLUME_MAX
        : ComputeDistanceAttenuatedVolume(player, speaker));
}

// Create the streaming buffer for a new utterance, sized to hold the whole line.
bool StartStreamingVoice(const WavData& first, const SpeakerSnapshot& speaker,
    const std::string& requestId, bool nonPositional)
{
    if (!EnsureDirectSound())
    {
        return false;
    }
    StopStreamingVoice("new_utterance");

    const WAVEFORMATEX& fmt = first.format;
    if (!fmt.nSamplesPerSec || !fmt.nBlockAlign)
    {
        return false;
    }
    DWORD byteRate = fmt.nAvgBytesPerSec ? fmt.nAvgBytesPerSec
        : fmt.nSamplesPerSec * fmt.nBlockAlign;
    DWORD capacity = byteRate * kStreamingVoiceMaxSeconds;
    capacity -= capacity % fmt.nBlockAlign; // frame-align
    if (!capacity)
    {
        return false;
    }

    const bool use3d = g_debugConfig.directSound3dEnabled && !nonPositional;
    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    if (use3d)
    {
        desc.dwFlags |= DSBCAPS_CTRL3D;
    }
    if (g_debugConfig.directSoundSoftwareBufferEnabled)
    {
        desc.dwFlags |= DSBCAPS_LOCSOFTWARE;
    }
    desc.dwBufferBytes = capacity;
    desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&fmt);

    IDirectSoundBuffer* raw = nullptr;
    if (FAILED(g_state.directSound->CreateSoundBuffer(&desc, &raw, nullptr)) || !raw)
    {
        LogLine("Stream CreateSoundBuffer failed");
        return false;
    }
    IDirectSoundBuffer8* buffer = nullptr;
    HRESULT hr = raw->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&buffer));
    raw->Release();
    if (FAILED(hr) || !buffer)
    {
        LogLine("Stream QueryInterface(IDirectSoundBuffer8) failed: 0x%08X", hr);
        return false;
    }

    // Zero-fill so any underrun region plays silence rather than stale data.
    void* p1 = nullptr;
    void* p2 = nullptr;
    DWORD s1 = 0;
    DWORD s2 = 0;
    if (SUCCEEDED(buffer->Lock(0, capacity, &p1, &s1, &p2, &s2, 0)))
    {
        if (p1)
        {
            std::memset(p1, 0, s1);
        }
        if (p2)
        {
            std::memset(p2, 0, s2);
        }
        buffer->Unlock(p1, s1, p2, s2);
    }

    IDirectSound3DBuffer* buffer3d = nullptr;
    if (use3d && FAILED(buffer->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&buffer3d))))
    {
        buffer3d = nullptr;
    }

    g_state.streamBuffer = buffer;
    g_state.streamBuffer3d = buffer3d;
    g_state.streamActive = true;
    g_state.streamStarted = false;
    g_state.streamCapacityBytes = capacity;
    g_state.streamWriteCursor = 0;
    g_state.streamCumulativeMs = 0;
    g_state.streamFormat = fmt;
    g_state.streamRequestId = requestId;
    g_state.streamSpeaker = speaker;
    g_state.streamNonPositional = nonPositional;
    g_state.streamLipSyncQueue.clear();
    TraceRequestEvent(requestId, "stream_voice_started",
        {},
        {
            { "capacity_bytes", static_cast<double>(capacity) },
            { "sample_rate", static_cast<double>(fmt.nSamplesPerSec) },
        });
    return true;
}

// Split the NPC's full line into <= maxChars caption segments at word/punctuation
// boundaries and record each segment's CHARACTER offset. UpdateStreamingVoice reveals
// a segment when the actual play cursor reaches that character's share of the line, so
// captions track the real speech (not a fixed rate). maxChars <= 0 => one segment (the
// whole line). DISPLAY ONLY — never touches the audio.
void BuildCaptionSegments(const std::string& text, int maxChars)
{
    g_state.captionSegments.clear();
    g_state.captionSegmentStartChar.clear();
    g_state.captionCurrentIndex = -1;
    g_state.captionSourceText = text;
    if (text.empty())
    {
        return;
    }
    if (maxChars <= 0 || text.size() <= static_cast<size_t>(maxChars))
    {
        g_state.captionSegments.push_back(text);
        g_state.captionSegmentStartChar.push_back(0);
        return;
    }
    const size_t limit = static_cast<size_t>(maxChars);
    const size_t n = text.size();
    size_t i = 0;
    while (i < n)
    {
        while (i < n && text[i] == ' ')
        {
            ++i;
        }
        if (i >= n)
        {
            break;
        }
        const size_t startChar = i;
        size_t end = (i + limit < n) ? i + limit : n;
        if (end < n)
        {
            // Prefer a sentence/clause punctuation break past the segment midpoint,
            // else the last word boundary; else hard-split a single long word.
            size_t punct = std::string::npos;
            for (size_t p = i; p < end; ++p)
            {
                const char c = text[p];
                if (c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':')
                {
                    punct = p;
                }
            }
            const size_t space = text.rfind(' ', end);
            if (punct != std::string::npos && punct >= i + limit / 2)
            {
                end = punct + 1;
            }
            else if (space != std::string::npos && space > i)
            {
                end = space;
            }
        }
        std::string seg = text.substr(i, end - i);
        while (!seg.empty() && seg.back() == ' ')
        {
            seg.pop_back();
        }
        if (!seg.empty())
        {
            g_state.captionSegments.push_back(seg);
            g_state.captionSegmentStartChar.push_back(static_cast<DWORD>(startChar));
        }
        i = end;
    }
    if (g_state.captionSegments.empty())
    {
        g_state.captionSegments.push_back(text);
        g_state.captionSegmentStartChar.push_back(0);
    }
}

// Write one chunk's PCM into the streaming buffer (creating it on the first chunk of
// a new utterance) and queue its lip-sync for when that audio plays.
bool AppendStreamingChunk(const QueuedAudioChunk& chunk, const WavData& wav, const SpeakerSnapshot& speaker)
{
    // A single 3D buffer can sit at only ONE position, so it must serve ONE speaker.
    // In a group reply both NPCs stream under the SAME requestId, so the request-id
    // guard alone never restarts — every chunk pours into the first speaker's buffer
    // and (because UpdateStreamingVoice re-spatializes the whole buffer each frame)
    // the second NPC's words play from the first NPC's location. Also finalize +
    // restart when the resolved SPEAKER changes mid-request so each NPC gets its own
    // correctly-positioned buffer. By the time the next speaker's first chunk lands,
    // the previous speaker's audio has normally drained (the LLM-generation gap
    // between turns), so this doesn't clip the prior line in practice.
    const bool requestChanged = g_state.streamRequestId != chunk.requestId;
    const bool speakerChanged = !chunk.nonPositional
        && g_state.streamSpeaker.valid && speaker.valid
        && g_state.streamSpeaker.refId != speaker.refId;
    if (g_state.streamActive && (requestChanged || speakerChanged))
    {
        StopStreamingVoice(requestChanged ? "request_changed" : "speaker_changed");
    }
    if (!g_state.streamActive && !StartStreamingVoice(wav, speaker, chunk.requestId, chunk.nonPositional))
    {
        return false;
    }
    IDirectSoundBuffer8* buffer = g_state.streamBuffer;
    if (!buffer || wav.pcmData.empty())
    {
        return false;
    }
    const DWORD blockAlign = g_state.streamFormat.nBlockAlign ? g_state.streamFormat.nBlockAlign : 2;

    // Underrun recovery (e.g. opener->remainder gap): never write behind the play
    // cursor, or that audio is skipped. Keep a small lead ahead of it.
    DWORD playPos = 0;
    DWORD writePos = 0;
    if (g_state.streamStarted && SUCCEEDED(buffer->GetCurrentPosition(&playPos, &writePos)))
    {
        DWORD lead = g_state.streamFormat.nAvgBytesPerSec
            ? g_state.streamFormat.nAvgBytesPerSec * kStreamingVoiceLeadMs / 1000 : 0;
        lead -= lead % blockAlign;
        if (g_state.streamWriteCursor < playPos + lead)
        {
            g_state.streamWriteCursor = playPos + lead;
            g_state.streamWriteCursor -= g_state.streamWriteCursor % blockAlign;
        }
    }

    DWORD bytes = static_cast<DWORD>(wav.pcmData.size());
    if (g_state.streamWriteCursor + bytes > g_state.streamCapacityBytes)
    {
        // Utterance overran the buffer; clip the tail (rare for NPC lines).
        if (g_state.streamWriteCursor >= g_state.streamCapacityBytes)
        {
            return false;
        }
        bytes = g_state.streamCapacityBytes - g_state.streamWriteCursor;
        bytes -= bytes % blockAlign;
        if (!bytes)
        {
            return false;
        }
    }

    void* p1 = nullptr;
    void* p2 = nullptr;
    DWORD s1 = 0;
    DWORD s2 = 0;
    if (FAILED(buffer->Lock(g_state.streamWriteCursor, bytes, &p1, &s1, &p2, &s2, 0)))
    {
        return false;
    }
    if (p1 && s1)
    {
        std::memcpy(p1, wav.pcmData.data(), s1);
    }
    if (p2 && s2)
    {
        std::memcpy(p2, wav.pcmData.data() + s1, s2);
    }
    buffer->Unlock(p1, s1, p2, s2);

    const DWORD chunkStartMs = g_state.streamCumulativeMs;
    const DWORD durationMs = GetWavDurationMs(wav);
    g_state.streamWriteCursor += bytes;
    g_state.streamCumulativeMs += durationMs;
    g_state.streamLastAppendTick = GetTickCount();
    g_state.streamSpeaker = speaker;

    // Start playback once the first chunk is buffered.
    if (!g_state.streamStarted)
    {
        ApplyStreamingVoiceSpatial(speaker, chunk.nonPositional);
        buffer->SetCurrentPosition(0);
        if (SUCCEEDED(buffer->Play(0, 0, 0)))
        {
            g_state.streamStarted = true;
            g_state.streamPlayStartTick = GetTickCount();
            g_state.directSoundIdleSinceTick = 0;
        }
        // Conversation hold + remembered target (mirrors StartQueuedAudioPlayback).
        if (!chunk.nonPositional)
        {
            const std::string holdKey = chunk.speakerKey.empty() ? g_state.lastNpcKey : chunk.speakerKey;
            const std::string holdName = chunk.speakerName.empty() ? g_state.lastNpcName : chunk.speakerName;
            const bool skipHold = !chunk.requestId.empty()
                && g_state.movementActionRequestIds.find(chunk.requestId) != g_state.movementActionRequestIds.end();
            RememberNpcTarget(holdKey, holdName, speaker);
            if (!skipHold
                && (!g_state.conversationHold.active || g_state.conversationHold.speaker.refId != speaker.refId))
            {
                EngageConversationHold(holdKey, holdName, speaker);
            }
        }
    }

    // Queue this chunk's lip-sync to fire when its audio reaches the speakers.
    PendingStreamLipSync ls;
    ls.startMs = chunkStartMs;
    ls.durationMs = durationMs;
    ls.pcm = wav.pcmData;
    ls.format = wav.format;
    ls.requestId = chunk.requestId;
    ls.audioFile = chunk.audioFile;
    ls.subtitleText = chunk.subtitleText;
    ls.speaker = speaker;
    g_state.streamLipSyncQueue.push_back(std::move(ls));
    g_state.streamedAudioSeenForReply = true;

    // Build the caption segments from the line's full text on the first chunk that
    // carries it (display only; revealed by UpdateStreamingVoice).
    if (g_state.captionSegments.empty() && !chunk.subtitleText.empty())
    {
        g_state.captionMaxChars = chunk.captionMaxChars;
        BuildCaptionSegments(chunk.subtitleText, chunk.captionMaxChars);
    }

    TraceRequestEvent(chunk.requestId, "stream_chunk_appended",
        { { "audio_file", chunk.audioFile } },
        {
            { "chunk_index", static_cast<double>(chunk.chunkIndex) },
            { "write_cursor", static_cast<double>(g_state.streamWriteCursor) },
            { "start_ms", static_cast<double>(chunkStartMs) },
            { "duration_ms", static_cast<double>(durationMs) },
        });
    return true;
}

// Pop all pending chunk files into the streaming buffer (the single-buffer path's
// replacement for the static-buffer scheduler).
void DrainChunksToStreamingVoice()
{
    while (!g_state.pendingAudioChunks.empty())
    {
        // Resolve who this chunk belongs to BEFORE committing to it (positional
        // chunks resolve to the named NPC; non-positional are player-centered 2D).
        const QueuedAudioChunk& front = g_state.pendingAudioChunks.front();
        SpeakerSnapshot speaker = g_state.pendingSpeaker;
        if (front.nonPositional)
        {
            speaker = CaptureSpeakerSnapshot(GetPlayer());
        }
        else if (const auto resolved = ResolveSpeakerSnapshotForNpc(front.speakerKey, front.speakerName); resolved.has_value())
        {
            speaker = *resolved;
        }

        // Serialize speakers in a group reply: if this chunk belongs to a DIFFERENT
        // positional speaker than the one currently streaming AND that speaker's audio
        // hasn't finished playing, leave the chunk queued and retry next frame — else
        // starting the new buffer would cut the first NPC off mid-sentence. The whole
        // multi-NPC reply shares one requestId and `awaitingReply` stays true across
        // it, so the stream won't self-stop between speakers; gate on the play cursor.
        if (g_state.streamActive && g_state.streamStarted && !front.nonPositional
            && speaker.valid && g_state.streamSpeaker.valid
            && speaker.refId != g_state.streamSpeaker.refId)
        {
            DWORD playPos = 0;
            DWORD writePos = 0;
            if (g_state.streamBuffer
                && SUCCEEDED(g_state.streamBuffer->GetCurrentPosition(&playPos, &writePos))
                && playPos < g_state.streamWriteCursor)
            {
                break;
            }
        }

        const QueuedAudioChunk chunk = std::move(g_state.pendingAudioChunks.front());
        g_state.pendingAudioChunks.pop_front();
        if (!fs::exists(chunk.wavPath))
        {
            continue;
        }
        auto wav = LoadWavFile(chunk.wavPath);
        if (!wav.has_value())
        {
            continue;
        }
        AppendStreamingChunk(chunk, *wav, speaker);
        // The PCM is now copied into the streaming buffer, so the WAV file is no longer
        // needed — delete it (now a cheap native delete) to keep the audio dir from
        // growing unbounded (this folder is outside MO2's overwrite, so nothing else
        // prunes it).
        std::error_code removeEc;
        fs::remove(chunk.wavPath, removeEc);
    }
}

// Per-frame: fire scheduled lip-sync + subtitle, keep the buffer spatialized, and
// stop once the utterance has fully played out.
void UpdateStreamingVoice()
{
    if (!g_state.streamActive)
    {
        return;
    }
    const DWORD now = GetTickCount();

    if (g_state.streamStarted)
    {
        // Keep the voice spatialized EVERY frame so the 3D direction + distance volume
        // track the player as they move/turn. The single streaming buffer isn't in
        // activeSounds, so UpdateActiveSoundPositions doesn't cover it; updating only
        // per-chunk made the directional audio lag.
        ApplyStreamingVoiceSpatial(g_state.streamSpeaker, g_state.streamNonPositional);
        const DWORD elapsed = now - g_state.streamPlayStartTick;
        // Coalesce due mini-chunks into a lip-sync window, then drive ONE face
        // animation per window — NOT one per 200ms chunk (that restarted the FaceGen
        // animation ~5x/sec, causing lag, jank and crashes). The envelope comes from
        // the window's real PCM; visemes use the window's text slice (rough alignment
        // via the same char/time mapping as the captions).
        while (!g_state.streamLipSyncQueue.empty()
            && g_state.streamLipSyncQueue.front().startMs <= elapsed)
        {
            PendingStreamLipSync ls = std::move(g_state.streamLipSyncQueue.front());
            g_state.streamLipSyncQueue.pop_front();
            if (g_state.lipSyncAccumPcm.empty())
            {
                g_state.lipSyncAccumStartMs = ls.startMs;
                g_state.lipSyncAccumSpeaker = ls.speaker;
            }
            g_state.lipSyncAccumPcm.insert(g_state.lipSyncAccumPcm.end(), ls.pcm.begin(), ls.pcm.end());
            g_state.lipSyncAccumMs += ls.durationMs;
        }
        const bool flushLipSync = g_state.lipSyncAccumMs >= kLipSyncWindowMs
            || (g_state.streamLipSyncQueue.empty() && g_state.lipSyncAccumMs > 0);
        if (flushLipSync && !g_state.lipSyncAccumPcm.empty())
        {
            // Text slice for this window's visemes (rough word alignment by char fraction).
            std::string winText;
            const size_t totalChars = g_state.captionSourceText.size();
            const DWORD totalMs = g_state.streamCumulativeMs;
            if (totalChars > 0 && totalMs > 0)
            {
                unsigned long long a =
                    static_cast<unsigned long long>(g_state.lipSyncAccumStartMs) * totalChars / totalMs;
                unsigned long long b =
                    static_cast<unsigned long long>(g_state.lipSyncAccumStartMs + g_state.lipSyncAccumMs) * totalChars / totalMs;
                if (a > totalChars) a = totalChars;
                if (b > totalChars) b = totalChars;
                if (b > a) winText = g_state.captionSourceText.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
            }
            ApplyStreamingVoiceSpatial(g_state.lipSyncAccumSpeaker, false);
            WavData wav;
            wav.format = g_state.streamFormat;
            wav.pcmData = std::move(g_state.lipSyncAccumPcm);
            QueuedAudioChunk chunk;
            chunk.requestId = g_state.streamRequestId;
            chunk.subtitleText = winText;
            StartSpeechAnimation(wav, chunk, g_state.lipSyncAccumSpeaker, g_state.lipSyncAccumMs);
            g_state.lipSyncAccumPcm.clear();
            g_state.lipSyncAccumMs = 0;
        }
    }

    // Caption scheduler (DISPLAY ONLY): reveal each segment when the ACTUAL playback
    // position reaches that segment's text — its character share of the whole line,
    // measured against the real audio duration. Anchored to the play cursor so it
    // tracks the speech exactly and never drifts (the old fixed-rate estimate lagged
    // and accumulated desync). Falls back to caption_ms_per_char only if the play
    // cursor is unavailable. Audio is never gated or reshaped by this.
    if (g_state.streamStarted && g_debugConfig.subtitlesEnabled && !g_state.captionSegments.empty())
    {
        const size_t totalChars = g_state.captionSourceText.size();
        // Lock the total audio duration once the WHOLE reply is in, so the play-cursor
        // fraction is measured against a STABLE, COMPLETE total. Two conditions:
        //   1) !awaitingReply  - the backend has sent its final response, so no more
        //      audio chunks are coming. This is the critical guard: TTS slices arrive
        //      ~400-650 ms apart (each ~1.5 s slice takes that long to render), and
        //      once the off-VFS lag fix made chunk reads instant, those normal
        //      inter-chunk waits exceed the 400 ms timer below. Locking on the timer
        //      ALONE would fire during the opener->remainder gap and freeze totalMs at
        //      a PARTIAL (opener-only) duration -> the fraction overshoots 1.0 and the
        //      caption skips to the end. awaitingReply stays true across that gap.
        //   2) 400 ms since the last append - the final chunk has been appended and
        //      processed (the final response can land a few ms before the last chunk).
        // Using the still-growing streamCumulativeMs before this also made the target
        // wobble -> flashing / captions ending early.
        if (g_state.captionTotalMsLocked == 0 && g_state.streamCumulativeMs > 0
            && !g_state.awaitingReply
            && (now - g_state.streamLastAppendTick) > 400)
        {
            g_state.captionTotalMsLocked = g_state.streamCumulativeMs;
        }
        const DWORD totalMs = g_state.captionTotalMsLocked;
        const DWORD byteRate = g_state.streamFormat.nAvgBytesPerSec;
        DWORD playPos = 0;
        DWORD writePos = 0;
        const bool havePlay = g_state.streamBuffer && byteRate
            && SUCCEEDED(g_state.streamBuffer->GetCurrentPosition(&playPos, &writePos));
        const unsigned long long playMs =
            havePlay ? static_cast<unsigned long long>(playPos) * 1000ULL / byteRate : 0ULL;

        // Highest segment the play cursor has reached, MONOTONIC (never step back, so
        // it can't flash between two). Only advances past 0 once the total is locked;
        // until then segment 0 holds.
        int target = (g_state.captionCurrentIndex < 0) ? 0 : g_state.captionCurrentIndex;
        if (havePlay && totalMs > 0 && totalChars > 0)
        {
            for (size_t s = 0; s < g_state.captionSegmentStartChar.size(); ++s)
            {
                const DWORD startChar = g_state.captionSegmentStartChar[s];
                if (playMs * totalChars >= static_cast<unsigned long long>(startChar) * totalMs)
                {
                    if (static_cast<int>(s) > target)
                    {
                        target = static_cast<int>(s);
                    }
                }
                else
                {
                    break;
                }
            }
        }

        // Show on advance, and refresh ~1x/sec so the subtitle never expires
        // mid-segment (that looked like the caption "ending early").
        const bool advanced = target != g_state.captionCurrentIndex;
        if (advanced || (now - g_state.captionLastShowTick) >= 1000)
        {
            g_state.captionCurrentIndex = target;
            const std::string ascii = ToUiAscii(g_state.captionSegments[static_cast<size_t>(target)]);
            if (!ascii.empty() && ShowDialogSubtitle("", ascii, 2.0f))
            {
                g_state.subtitleShownForReply = true;
                g_state.replySubtitleText = g_state.captionSegments[static_cast<size_t>(target)];
                g_state.captionLastShowTick = now;
            }
        }
    }

    // End: the reply is finalized, all written audio has played, and a brief grace
    // has elapsed with no new appends. While awaiting the reply, keep the buffer
    // alive (the opener's remainder and later speakers may still append).
    if (g_state.streamStarted && g_state.streamLipSyncQueue.empty() && !g_state.awaitingReply)
    {
        DWORD playPos = 0;
        DWORD writePos = 0;
        if (g_state.streamBuffer
            && SUCCEEDED(g_state.streamBuffer->GetCurrentPosition(&playPos, &writePos))
            && playPos >= g_state.streamWriteCursor
            && now - g_state.streamLastAppendTick > kStreamingVoiceEndGraceMs)
        {
            StopStreamingVoice("played_out");
        }
    }
}

bool StartQueuedAudioPlayback(const QueuedAudioChunk& chunk, const SpeakerSnapshot& speaker)
{
    g_state.lastPlaybackDiagnostics.clear();

    auto wavData = LoadWavFile(chunk.wavPath);
    if (!wavData.has_value())
    {
        LogLine("Queued chunk WAV could not be loaded: %s", chunk.wavPath.string().c_str());
        return false;
    }

    const bool force2d = chunk.nonPositional;
    if (!PlayVoiceWav(chunk.wavPath, speaker, &*wavData, force2d))
    {
        LogLine("DirectSound playback failed for queued chunk %s.", chunk.wavPath.string().c_str());
        return false;
    }

    const DWORD durationMs = GetWavDurationMs(*wavData);
    const std::string holdNpcKey = chunk.speakerKey.empty() ? g_state.lastNpcKey : chunk.speakerKey;
    const std::string holdNpcName = chunk.speakerName.empty() ? g_state.lastNpcName : chunk.speakerName;
    const bool skipConversationHold = !chunk.requestId.empty()
        && g_state.movementActionRequestIds.find(chunk.requestId) != g_state.movementActionRequestIds.end();
    if (!chunk.nonPositional)
    {
        RememberNpcTarget(holdNpcKey, holdNpcName, speaker);
        if (skipConversationHold)
        {
            TraceRequestEvent(chunk.requestId, "conversation_hold_skipped_for_movement_action",
                {
                    { "speaker_key", holdNpcKey },
                    { "speaker_name", holdNpcName },
                },
                {
                    { "speaker_ref_id", static_cast<double>(speaker.refId) },
                },
                {});
        }
        else if (!g_state.conversationHold.active || g_state.conversationHold.speaker.refId != speaker.refId)
        {
            EngageConversationHold(holdNpcKey, holdNpcName, speaker);
        }
        else
        {
            g_state.conversationHold.npcKey = holdNpcKey;
            g_state.conversationHold.npcName = holdNpcName;
            g_state.conversationHold.speaker = speaker;
            g_state.conversationHold.releaseTick = 0;
        }
    }
    else if (!chunk.requestId.empty())
    {
        TraceRequestEvent(chunk.requestId, "conversation_hold_skipped_for_non_positional_audio",
            {
                { "speaker_key", holdNpcKey },
                { "speaker_name", holdNpcName },
            },
            {
                { "speaker_ref_id", static_cast<double>(speaker.refId) },
            },
            {});
    }
    g_state.activeSpeechUntilTick = GetTickCount() + (durationMs ? durationMs + kStreamedSpeechEndPaddingMs : 900);
    const bool used3d = g_debugConfig.directSound3dEnabled && !chunk.nonPositional;
    TraceRequestEvent(chunk.requestId, "audio_playback_started",
        {
            { "audio_file", chunk.audioFile },
            { "speaker_key", chunk.speakerKey },
            { "speaker_name", chunk.speakerName },
            { "published_at", chunk.publishedAtIso },
        },
        {
            { "chunk_index", static_cast<double>(chunk.chunkIndex) },
            { "duration_ms", static_cast<double>(durationMs) },
            { "speaker_ref_id", static_cast<double>(speaker.refId) },
            { "subtitle_length", static_cast<double>(chunk.subtitleText.size()) },
            { "streaming_chunk_overlap_ms", static_cast<double>(g_debugConfig.streamingChunkOverlapMs) },
        },
        {
            { "directsound_3d", used3d },
            { "directsound_software_buffer", g_debugConfig.directSoundSoftwareBufferEnabled },
            { "non_positional_audio", chunk.nonPositional },
        });
    if (!chunk.nonPositional)
    {
        StartSpeechAnimation(*wavData, chunk, speaker, durationMs);
    }

    const std::string previousReplySubtitle = g_state.replySubtitleText;
    std::string subtitleSource = chunk.subtitleText;
    const bool reusedReplySubtitle = subtitleSource.empty() && !g_state.replySubtitleText.empty();
    if (!subtitleSource.empty())
    {
        g_state.replySubtitleText = subtitleSource;
    }
    else
    {
        subtitleSource = g_state.replySubtitleText;
    }

    const std::string subtitleText = ToUiAscii(subtitleSource);
    const bool subtitleChanged = subtitleSource != previousReplySubtitle;
    const float subtitleSeconds = SubtitleDuration(subtitleText);
    if (g_debugConfig.subtitlesEnabled
        && !subtitleText.empty()
        && (!g_state.subtitleShownForReply || subtitleChanged)
        && ShowDialogSubtitle("", subtitleText, subtitleSeconds))
    {
        g_state.subtitleShownForReply = true;
        if (reusedReplySubtitle)
        {
            LogLine("Reused previous reply subtitle for continuation chunk %d.", chunk.chunkIndex);
        }
    }

    std::ostringstream diag;
    diag << "playback_mode=" << (chunk.nonPositional ? "directsound_2d_non_positional" : (g_debugConfig.directSound3dEnabled ? "directsound_3d_stream" : "directsound_2d_stream")) << "\n";
    diag << "playback_software_buffer=" << (g_debugConfig.directSoundSoftwareBufferEnabled ? 1 : 0) << "\n";
    diag << "playback_non_positional_audio=" << (chunk.nonPositional ? 1 : 0) << "\n";
    diag << "playback_chunk_index=" << chunk.chunkIndex << "\n";
    diag << "playback_audio_file=" << EscapeForDiag(chunk.audioFile) << "\n";
    diag << "playback_duration_ms=" << durationMs << "\n";
    diag << "playback_result=started\n";
    g_state.lastPlaybackDiagnostics = diag.str();

    return true;
}

bool ExecuteConsoleCommand(TESObjectREFR* callingRef, const std::string& command)
{
    if (!callingRef || !EnsureConsoleCommandHelper())
    {
        return false;
    }

    if (!g_scriptInterface->CallFunctionAlt(g_consoleCommandScript, callingRef, 1, command.c_str()))
    {
        LogLine("CallFunctionAlt failed for Console helper: %s", command.c_str());
        return false;
    }

    return true;
}

std::pair<std::string, std::string> ResolveRefVoiceTypeMetadata(TESObjectREFR* ref)
{
    if (!ref || !ref->baseForm || ref->baseForm->typeID != kFormType_TESNPC)
    {
        return std::make_pair(std::string(), std::string());
    }

    auto* actorBase = static_cast<TESActorBase*>(ref->baseForm);
    BGSVoiceType* voiceType = actorBase->baseData.GetVoiceType();
    if (!voiceType)
    {
        voiceType = actorBase->baseData.voiceType;
    }
    if (!voiceType)
    {
        return std::make_pair(std::string(), std::string());
    }

    const std::string voiceTypeName = GetFormNameSafe(voiceType);
    return std::make_pair(Slugify(voiceTypeName), voiceTypeName);
}

void ConsumeAudioChunks()
{
    std::error_code ec;
    if (!fs::exists(OutboxChunkDir(), ec))
    {
        return;
    }

    std::vector<fs::path> chunkPaths;
    for (const auto& entry : fs::directory_iterator(OutboxChunkDir(), ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
        {
            chunkPaths.push_back(entry.path());
        }
    }

    std::sort(chunkPaths.begin(), chunkPaths.end());

    for (const auto& chunkPath : chunkPaths)
    {
        std::ifstream in(chunkPath, std::ios::binary);
        if (!in)
        {
            continue;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lines.push_back(line);
        }

        if (lines.size() < 5)
        {
            fs::remove(chunkPath, ec);
            continue;
        }

        const std::string requestId = Trim(lines[0]);
        const int chunkIndex = std::atoi(Trim(lines[1]).c_str());
        if (requestId != g_state.activeRequestId || chunkIndex <= g_state.lastAudioChunkIndex)
        {
            fs::remove(chunkPath, ec);
            continue;
        }

        g_state.lastBridgeActivityTick = GetTickCount();
        g_state.sawBridgeActivity = true;

        const std::string speakerKey = lines.size() > 2 ? Trim(lines[2]) : g_state.pendingNpcKey;
        const std::string speakerName = lines.size() > 3 ? Trim(lines[3]) : g_state.pendingNpcName;
        const std::string audioFile = Trim(lines[4]);
        const std::string subtitleText = lines.size() > 5 ? Trim(lines[5]) : "";
        const std::string publishedAtIso = lines.size() > 6 ? Trim(lines[6]) : "";
        bool nonPositionalAudio = ToLowerAscii(speakerKey) == "todd";
        int captionMaxChars = -1; // display-only caption split hint from the backend
        for (size_t metadataIndex = 7; metadataIndex < lines.size(); ++metadataIndex)
        {
            const std::string token = Trim(lines[metadataIndex]);
            const size_t equals = token.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }
            const std::string mkey = Trim(token.substr(0, equals));
            const std::string mval = Trim(token.substr(equals + 1));
            nonPositionalAudio = IsNonPositionalChunkMetadata(mkey, mval, nonPositionalAudio);
            if (mkey == "caption_max_chars")
            {
                captionMaxChars = std::atoi(mval.c_str());
            }
        }

        g_state.lastAudioChunkIndex = chunkIndex;
        if (!audioFile.empty())
        {
            TraceRequestEvent(requestId, "audio_chunk_meta_seen",
                {
                    { "audio_file", audioFile },
                    { "speaker_key", speakerKey },
                    { "speaker_name", speakerName },
                    { "published_at", publishedAtIso },
                },
                {
                    { "chunk_index", static_cast<double>(chunkIndex) },
                    { "subtitle_length", static_cast<double>(subtitleText.size()) },
                },
                {
                    { "non_positional_audio", nonPositionalAudio },
                });
            g_state.pendingAudioChunks.push_back(QueuedAudioChunk{
                requestId,
                AudioDir() / audioFile,
                audioFile,
                speakerKey,
                speakerName,
                subtitleText,
                publishedAtIso,
                chunkIndex,
                nonPositionalAudio,
                captionMaxChars,
            });
            g_state.streamedAudioSeenForReply = true;
        }
        fs::remove(chunkPath, ec);
    }
}

void PlayQueuedAudioChunk()
{
    // Phase 3: feed the single continuous streaming buffer instead of the static
    // per-chunk scheduler. UpdateStreamingVoice (each frame) drives lip-sync + end.
    if (g_debugConfig.singleBufferStreaming)
    {
        DrainChunksToStreamingVoice();
        return;
    }

    const DWORD now = GetTickCount();
    if (g_state.activeSpeechUntilTick && now < g_state.activeSpeechUntilTick)
    {
        if (g_state.pendingAudioChunks.empty())
        {
            return;
        }

        const DWORD remainingMs = g_state.activeSpeechUntilTick - now;
        if (remainingMs > g_debugConfig.streamingChunkOverlapMs)
        {
            return;
        }
    }

    g_state.activeSpeechUntilTick = 0;
    while (!g_state.pendingAudioChunks.empty())
    {
        const QueuedAudioChunk chunk = std::move(g_state.pendingAudioChunks.front());
        g_state.pendingAudioChunks.pop_front();

        if (!fs::exists(chunk.wavPath))
        {
            LogLine("Queued chunk WAV missing: %s", chunk.wavPath.string().c_str());
            TraceRequestEvent(chunk.requestId, "audio_chunk_missing",
                {
                    { "audio_file", chunk.audioFile },
                },
                {
                    { "chunk_index", static_cast<double>(chunk.chunkIndex) },
                });
            continue;
        }

        SpeakerSnapshot resolvedSpeaker = g_state.pendingSpeaker;
        if (chunk.nonPositional)
        {
            resolvedSpeaker = CaptureSpeakerSnapshot(GetPlayer());
        }
        else if (const auto chunkSpeaker = ResolveSpeakerSnapshotForNpc(chunk.speakerKey, chunk.speakerName); chunkSpeaker.has_value())
        {
            resolvedSpeaker = *chunkSpeaker;
        }

        if (StartQueuedAudioPlayback(chunk, resolvedSpeaker))
        {
            return;
        }
    }
}

void ResetRuntimeState()
{
    LoadDebugConfigIfNeeded(true);
    AbortVoiceCapture("runtime_reset_voice", false);
    ReleaseConversationHold("runtime_reset");
    StopSpeechAnimation();
    ClearDialogSubtitle();
    g_state.awaitingInput = false;
    g_state.bridgeTextInputOwned = false;
    g_state.awaitingReply = false;
    g_state.awaitingVoiceReply = false;
    g_state.inputMenuSeenVisible = false;
    g_state.inputStartedTick = 0;
    g_state.staleTextInputCloseRetryTick = 0;
    g_state.gameWindowFocusedLastFrame = false;
    g_state.ignoreHotkeysUntilTick = 0;
    ClearTextInputKeyWatcher();
    g_state.pendingNpcKey.clear();
    g_state.pendingNpcName.clear();
    g_state.pendingSpeaker = {};
    g_state.replyStartedTick = 0;
    g_state.lastBridgeActivityTick = 0;
    g_state.sawBridgeActivity = false;
    g_state.activeRequestId.clear();
    g_state.lastAudioChunkIndex = -1;
    g_state.subtitleShownForReply = false;
    g_state.activeSpeechUntilTick = 0;
    g_state.replySubtitleText.clear();
    g_state.streamedAudioSeenForReply = false;
    g_state.pendingAudioChunks.clear();
    StopStreamingVoice("reply_state_reset");
    g_state.movementActionRequestIds.clear();
    g_state.lastNpcKey.clear();
    g_state.lastNpcName.clear();
    g_state.lastNpcSpeaker = {};
    g_state.npcSpeakersByKey.clear();
    g_state.keyDownLastFrame = false;
    g_state.voiceCapture.keyDownLastFrame = false;
    g_state.voiceCapture.adminKeyDownLastFrame = false;
    g_state.voiceCapture.adminMode = false;
    g_state.lastPlaybackDiagnostics.clear();
    g_state.lastRuntimeHeartbeatTick = 0;
    g_state.runtimeHeartbeatFrame = 0;
    g_state.saveStateSyncPending = false;
    g_state.saveStateSyncEventId.clear();
    g_state.saveStateSyncType.clear();
    g_state.saveStateSyncLastPollTick = 0;
    g_state.saveStateSyncHudMessageTick = 0;

    std::error_code ec;
    fs::remove(UiSubmitPath(), ec);
    fs::remove(ScriptRunnerTracePath(), ec);

    CleanupFinishedSounds();
    for (auto& sound : g_state.activeSounds)
    {
        if (sound.buffer3d)
        {
            sound.buffer3d->Release();
            sound.buffer3d = nullptr;
        }
        if (sound.buffer)
        {
            sound.buffer->Stop();
            sound.buffer->Release();
            sound.buffer = nullptr;
        }
    }
    g_state.activeSounds.clear();
    ShutdownDirectSound();
    WriteRuntimeHeartbeatIfNeeded(true);

}

void ConsumeReply()
{
    const auto response = ReadResponse();
    if (!response.has_value())
    {
        return;
    }

    std::ostringstream diag;
    diag << "response_ok=" << (response->ok ? 1 : 0) << "\n";
    diag << "response_player_text=" << EscapeForDiag(response->playerText) << "\n";
    diag << "response_text=" << EscapeForDiag(response->text) << "\n";
    diag << "response_error=" << EscapeForDiag(response->error) << "\n";
    diag << "response_status=" << response->statusCode << "\n";
    diag << "game_master_action=" << EscapeForDiag(response->gameMasterAction) << "\n";
    diag << "game_master_confidence=" << response->gameMasterConfidence << "\n";
    diag << "game_master_should_trigger=" << (response->gameMasterShouldTrigger ? 1 : 0) << "\n";
    diag << "action_npc_key=" << EscapeForDiag(ActionNpcKey(*response)) << "\n";
    diag << "action_npc_name=" << EscapeForDiag(ActionNpcName(*response)) << "\n";
    diag << "non_positional_audio=" << (response->nonPositionalAudio ? 1 : 0) << "\n";
    g_state.lastBridgeActivityTick = GetTickCount();
    g_state.sawBridgeActivity = true;

    if (!response->isFinal)
    {
        ShowRecognizedPlayerSubtitleIfNeeded(*response);
        TraceRequestEvent(response->requestId.empty() ? g_state.activeRequestId : response->requestId, "response_partial_seen",
            {
                { "npc_key", response->npcKey },
            },
            {
                { "text_length", static_cast<double>(response->text.size()) },
                { "audio_chunk_index", static_cast<double>(response->audioChunkIndex) },
            });
        diag << "partial_shown=0\n";
        WriteDiagnostics(diag.str());
        return;
    }

    if (!response->ok)
    {
        ClearOutboxArtifacts("response_failed");
        g_state.awaitingReply = false;
        g_state.replyStartedTick = 0;
        g_state.lastBridgeActivityTick = 0;
        g_state.sawBridgeActivity = false;
        g_state.activeRequestId.clear();
        g_state.awaitingVoiceReply = false;
        TraceRequestEvent(response->requestId, "response_failed",
            {
                { "error", response->error },
            });
        const std::string errorText = response->error.empty() ? "Bridge error." : ("Bridge error: " + ToUiAscii(response->error));
        ShowHudMessage(errorText);
        diag << "played_audio=0\n";
        WriteDiagnostics(diag.str());
        return;
    }

    const std::string speaker = ToUiAscii(response->npcName.empty() ? g_state.pendingNpcName : response->npcName);
    const std::string line = ToUiAscii(response->text);
    ShowRecognizedPlayerSubtitleIfNeeded(*response);
    g_state.awaitingVoiceReply = false;
    if (g_debugConfig.drainQueuedChunksAfterFinal)
    {
        ConsumeAudioChunks();
    }
    TraceRequestEvent(response->requestId, "response_final_seen",
        {
            { "npc_key", response->npcKey },
            { "npc_name", response->npcName },
            { "audio_file", response->audioFile },
            { "action_npc_key", ActionNpcKey(*response) },
            { "action_npc_name", ActionNpcName(*response) },
        },
        {
            { "text_length", static_cast<double>(response->text.size()) },
            { "audio_chunk_index", static_cast<double>(response->audioChunkIndex) },
        },
        {
            { "response_ok", response->ok },
            { "non_positional_audio", response->nonPositionalAudio },
        });
    if (!g_state.pendingAudioChunks.empty())
    {
        TraceRequestEvent(response->requestId, "audio_queue_pending_after_final",
            {},
            {
                { "pending_chunks", static_cast<double>(g_state.pendingAudioChunks.size()) },
            },
            {
                { "drain_after_final_enabled", g_debugConfig.drainQueuedChunksAfterFinal },
            });
    }
    const bool suppressedGameMasterAction = false;
    std::string triggeredGameMasterAction;
    const bool triggeredGameMaster = TriggerGameMasterAction(*response, &triggeredGameMasterAction);
    const bool triggeredCombat = triggeredGameMaster && triggeredGameMasterAction == "ATTACK";
    const bool triggeredFollow = triggeredGameMaster && triggeredGameMasterAction == "FOLLOW";
    const bool triggeredStopFollow = triggeredGameMaster && triggeredGameMasterAction == "STOP_FOLLOW";
    if (triggeredCombat)
    {
        InterruptBridgeReplyAndPlayback("game_master_attack");
    }
    bool playedAudio = false;
    const bool alreadyStreamingAudio = g_state.streamedAudioSeenForReply
        || !g_state.pendingAudioChunks.empty()
        || HasPendingChunkFiles()
        || (g_state.activeSpeechUntilTick && GetTickCount() < g_state.activeSpeechUntilTick);
    if (!triggeredCombat && !response->audioFile.empty())
    {
        if (!alreadyStreamingAudio)
        {
            const QueuedAudioChunk finalChunk{
                response->requestId,
                AudioDir() / response->audioFile,
                response->audioFile,
                response->npcKey,
                speaker,
                line,
                "",
                response->audioChunkIndex,
                response->nonPositionalAudio,
            };
            SpeakerSnapshot finalSpeaker = g_state.pendingSpeaker;
            if (response->nonPositionalAudio)
            {
                finalSpeaker = CaptureSpeakerSnapshot(GetPlayer());
            }
            else if (const auto resolvedFinalSpeaker = ResolveSpeakerSnapshotForNpc(response->npcKey, speaker); resolvedFinalSpeaker.has_value())
            {
                finalSpeaker = *resolvedFinalSpeaker;
            }
            playedAudio = StartQueuedAudioPlayback(finalChunk, finalSpeaker);
            if (!playedAudio)
            {
                LogLine("Reply playback failed for %s.", response->audioFile.c_str());
            }
        }
        else
        {
            playedAudio = true;
            TraceRequestEvent(response->requestId, "final_audio_already_streaming",
                {
                    { "audio_file", response->audioFile },
                },
                {
                    { "audio_chunk_index", static_cast<double>(response->audioChunkIndex) },
                });
        }
        if (playedAudio && g_debugConfig.subtitlesEnabled && !g_state.subtitleShownForReply)
        {
            if (ShowDialogSubtitle("", line, SubtitleDuration(line)))
            {
                g_state.subtitleShownForReply = true;
            }
        }
    }

    ClearOutboxArtifacts("response_final");
    g_state.awaitingReply = false;
    g_state.replyStartedTick = 0;
    g_state.lastBridgeActivityTick = 0;
    g_state.sawBridgeActivity = false;
    g_state.activeRequestId.clear();

    diag << "game_master_action_suppressed=" << (suppressedGameMasterAction ? 1 : 0) << "\n";
    diag << "triggered_combat=" << (triggeredCombat ? 1 : 0) << "\n";
    diag << "triggered_follow=" << (triggeredFollow ? 1 : 0) << "\n";
    diag << "triggered_stop_follow=" << (triggeredStopFollow ? 1 : 0) << "\n";
    diag << "played_audio=" << (playedAudio ? 1 : 0) << "\n";
    if (!g_state.lastPlaybackDiagnostics.empty())
    {
        diag << g_state.lastPlaybackDiagnostics;
    }
    WriteDiagnostics(diag.str());
    WriteRuntimeHeartbeatIfNeeded(true);
}

bool HasPendingChunkFiles()
{
    std::error_code ec;
    if (!fs::exists(OutboxChunkDir(), ec))
    {
        return false;
    }

    for (const auto& entry : fs::directory_iterator(OutboxChunkDir(), ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
        {
            return true;
        }
    }

    return false;
}

void RecoverStaleReplyState()
{
    if (!g_state.awaitingReply)
    {
        return;
    }

    const bool hasDiskActivity = fs::exists(InboxPath()) || fs::exists(OutboxPath()) || HasPendingChunkFiles();
    const bool hasAudioActivity = !g_state.pendingAudioChunks.empty()
        || !g_state.activeSounds.empty()
        || (g_state.activeSpeechUntilTick && GetTickCount() < g_state.activeSpeechUntilTick);
    if (hasDiskActivity || hasAudioActivity)
    {
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD lastActivityTick = g_state.lastBridgeActivityTick ? g_state.lastBridgeActivityTick : g_state.replyStartedTick;
    const DWORD timeoutMs = g_state.sawBridgeActivity ? 60000 : 45000;
    if (!lastActivityTick || now - lastActivityTick < timeoutMs)
    {
        return;
    }

    LogLine("Recovering stale bridge reply state for request %s after %lu ms without new bridge activity.", g_state.activeRequestId.c_str(), static_cast<unsigned long>(timeoutMs));
    ShowHudMessage("Recovered a stale bridge reply state.");
    ReleaseConversationHold("stale_reply_recovered");
    g_state.awaitingReply = false;
    g_state.awaitingVoiceReply = false;
    g_state.replyStartedTick = 0;
    g_state.lastBridgeActivityTick = 0;
    g_state.sawBridgeActivity = false;
    g_state.activeRequestId.clear();
    g_state.lastAudioChunkIndex = -1;
    g_state.subtitleShownForReply = false;
    g_state.activeSpeechUntilTick = 0;
    g_state.replySubtitleText.clear();
    g_state.streamedAudioSeenForReply = false;
    StopSpeechAnimation();
    ClearDialogSubtitle();
    g_state.pendingAudioChunks.clear();
    StopStreamingVoice("reply_state_reset");
    g_state.pendingNpcKey.clear();
    g_state.pendingNpcName.clear();
    g_state.pendingSpeaker = {};
}

void ForceCloseTextInputMenu(const char* reason);

bool OpenInGameTextInput(const std::string& npcName)
{
    if (!EnsureOpenTextInputScript())
    {
        return false;
    }

    std::error_code ec;
    fs::remove(UiSubmitPath(), ec);
    if (g_state.bridgeTextInputOwned || IsTextInputMenuActive())
    {
        ForceCloseTextInputMenu("opening new bridge text input");
        if (IsTextInputMenuActive())
        {
            LogLine("Refusing to open a new bridge TextEditMenu while the previous TextEditMenu is still visible.");
            g_state.bridgeTextInputOwned = true;
            g_state.staleTextInputCloseRetryTick = GetTickCount() + 250;
            return false;
        }
        g_state.bridgeTextInputOwned = false;
    }

    if (!g_scriptInterface->CallFunctionAlt(g_openTextInputScript, GetPlayer(), 1, npcName.c_str()))
    {
        LogLine("CallFunctionAlt failed while opening TextEditMenu.");
        return false;
    }

    g_state.bridgeTextInputOwned = true;
    return true;
}

void ForceCloseTextInputMenu(const char* reason)
{
    if (!IsTextInputMenuActive())
    {
        g_state.bridgeTextInputOwned = false;
        return;
    }

    PlayerCharacter* player = GetPlayer();
    if (!player)
    {
        LogLine("TextEditMenu stayed visible after %s, but PlayerRef was unavailable.", reason ? reason : "input");
        return;
    }

    if (EnsureCloseTextInputScript() && g_scriptInterface->CallFunctionAlt(g_closeTextInputScript, player, 0))
    {
        LogLine("Requested TextEditMenu close after %s.", reason ? reason : "input");
        if (!IsTextInputMenuActive())
        {
            g_state.bridgeTextInputOwned = false;
            return;
        }
    }

    LogLine("CloseActiveMenu helper unavailable or failed after %s; trying CloseAllMenus fallback.", reason ? reason : "input");
    if (ExecuteConsoleCommand(player, "CloseAllMenus"))
    {
        LogLine("Requested TextEditMenu close with CloseAllMenus fallback after %s.", reason ? reason : "input");
        g_state.bridgeTextInputOwned = IsTextInputMenuActive();
        return;
    }

    LogLine("TextEditMenu stayed visible after %s; all close helpers failed.", reason ? reason : "input");
    g_state.bridgeTextInputOwned = IsTextInputMenuActive();
}

void ResetTextInputKeyWatcher()
{
    g_state.inputEnterDownLastFrame = (GetAsyncKeyState(kChatVirtualKey) & 0x8000) != 0;
    g_state.inputEscapeDownLastFrame = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    g_state.inputEmptyEnterCancelTick = 0;
}

void ClearTextInputKeyWatcher()
{
    g_state.inputEnterDownLastFrame = false;
    g_state.inputEscapeDownLastFrame = false;
    g_state.inputEmptyEnterCancelTick = 0;
}

void PrimeHotkeyEdgeStateFromKeyboard()
{
    g_state.keyDownLastFrame = (GetAsyncKeyState(kChatVirtualKey) & 0x8000) != 0;
    g_state.adminKeyDownLastFrame = (GetAsyncKeyState(kAdminChatVirtualKey) & 0x8000) != 0;
    g_state.voiceCapture.keyDownLastFrame = (GetAsyncKeyState(kVoiceChatVirtualKey) & 0x8000) != 0;
    g_state.voiceCapture.adminKeyDownLastFrame = (GetAsyncKeyState(kAdminVoiceChatVirtualKey) & 0x8000) != 0;
    ResetTextInputKeyWatcher();
}

void SuppressBridgeHotkeysAfterTextInputClose(DWORD milliseconds = 350)
{
    PrimeHotkeyEdgeStateFromKeyboard();
    g_state.ignoreHotkeysUntilTick = GetTickCount() + milliseconds;
}

bool RecoverStaleTextInputMenu(const char* reason)
{
    if (g_state.awaitingInput)
    {
        g_state.staleTextInputCloseRetryTick = 0;
        return false;
    }

    if (!g_state.bridgeTextInputOwned && !IsTextInputMenuActive())
    {
        g_state.staleTextInputCloseRetryTick = 0;
        return false;
    }

    if (!IsTextInputMenuActive())
    {
        g_state.bridgeTextInputOwned = false;
        g_state.staleTextInputCloseRetryTick = 0;
        return false;
    }

    const DWORD now = GetTickCount();
    if (!g_state.staleTextInputCloseRetryTick || now >= g_state.staleTextInputCloseRetryTick)
    {
        ForceCloseTextInputMenu(reason ? reason : "stale text input");
        g_state.bridgeTextInputOwned = IsTextInputMenuActive();
        g_state.staleTextInputCloseRetryTick = now + 500;
    }

    PrimeHotkeyEdgeStateFromKeyboard();
    g_state.ignoreHotkeysUntilTick = now + 250;
    return true;
}

void CancelAwaitingTextInput(const char* reason, const char* diagnosticKey)
{
    std::error_code ec;
    fs::remove(UiSubmitPath(), ec);
    g_state.awaitingInput = false;
    ForceCloseTextInputMenu(reason ? reason : "input cancelled");
    g_state.bridgeTextInputOwned = IsTextInputMenuActive();
    g_state.inputMenuSeenVisible = false;
    ClearTextInputKeyWatcher();
    g_state.inputStartedTick = 0;
    g_state.staleTextInputCloseRetryTick = g_state.bridgeTextInputOwned ? GetTickCount() + 250 : 0;
    g_state.pendingNpcKey.clear();
    g_state.pendingNpcName.clear();
    g_state.pendingSpeaker = {};
    fs::remove(UiSubmitPath(), ec);
    SuppressBridgeHotkeysAfterTextInputClose();
    ReleaseConversationHold(reason);
    LogLine("Cancelled text input state after %s.", reason ? reason : "input");
    WriteDiagnostics(std::string(diagnosticKey ? diagnosticKey : "input_cancelled") + "=1\n");
}

bool ConsumeTextInputMenuCloseHotkeys(bool menuVisible)
{
    const bool enterDown = (GetAsyncKeyState(kChatVirtualKey) & 0x8000) != 0;
    const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool enterPressed = enterDown && !g_state.inputEnterDownLastFrame;
    const bool escapePressed = escapeDown && !g_state.inputEscapeDownLastFrame;
    g_state.inputEnterDownLastFrame = enterDown;
    g_state.inputEscapeDownLastFrame = escapeDown;

    if (!menuVisible)
    {
        g_state.inputEmptyEnterCancelTick = 0;
        if (escapePressed || (escapeDown && !g_state.inputStartedTick))
        {
            std::error_code ec;
            fs::remove(UiSubmitPath(), ec);
            CancelAwaitingTextInput("input_escape_hidden", "input_cancelled");
            return true;
        }
        return false;
    }

    if (escapePressed)
    {
        std::error_code ec;
        fs::remove(UiSubmitPath(), ec);
        CancelAwaitingTextInput("input_escape", "input_cancelled");
        ForceCloseTextInputMenu("input escape");
        return true;
    }

    std::error_code ec;
    const bool submitFileExists = fs::exists(UiSubmitPath(), ec);
    if (enterPressed && !submitFileExists)
    {
        g_state.inputEmptyEnterCancelTick = GetTickCount() + kTextInputEmptySubmitGraceMs;
    }

    if (!g_state.inputEmptyEnterCancelTick)
    {
        return false;
    }

    ec.clear();
    if (fs::exists(UiSubmitPath(), ec))
    {
        g_state.inputEmptyEnterCancelTick = 0;
        return false;
    }

    if (GetTickCount() < g_state.inputEmptyEnterCancelTick)
    {
        return false;
    }

    CancelAwaitingTextInput("input_empty", "input_empty");
    ForceCloseTextInputMenu("empty input");
    return true;
}

void ConsumeSubmittedInput()
{
    if (!g_state.awaitingInput)
    {
        return;
    }

    const bool menuVisible = IsTextInputMenuActive();
    if (menuVisible)
    {
        g_state.inputMenuSeenVisible = true;
    }

    if (ConsumeTextInputMenuCloseHotkeys(menuVisible))
    {
        return;
    }

    const auto submitted = ReadSubmittedInput();
    if (!submitted.has_value())
    {
        if (!menuVisible)
        {
            const DWORD now = GetTickCount();
            const bool invisibleTooLong = g_state.inputStartedTick
                && (now - g_state.inputStartedTick) >= kTextInputInvisibleRecoveryMs;
            if (g_state.inputMenuSeenVisible || invisibleTooLong)
            {
                CancelAwaitingTextInput("input_cancelled", "input_cancelled");
            }
        }
        return;
    }

    std::error_code ec;
    fs::remove(UiSubmitPath(), ec);

    g_state.awaitingInput = false;
    g_state.inputMenuSeenVisible = false;
    ClearTextInputKeyWatcher();
    g_state.inputStartedTick = 0;
    g_state.staleTextInputCloseRetryTick = 0;

    if (menuVisible)
    {
        ForceCloseTextInputMenu("submitted input");
        g_state.bridgeTextInputOwned = IsTextInputMenuActive();
        ec.clear();
        fs::remove(UiSubmitPath(), ec);
    }
    else
    {
        g_state.bridgeTextInputOwned = false;
    }

    if (submitted->empty())
    {
        g_state.pendingNpcKey.clear();
        g_state.pendingNpcName.clear();
        g_state.pendingSpeaker = {};
        SuppressBridgeHotkeysAfterTextInputClose();
        ReleaseConversationHold("input_empty");
        WriteDiagnostics("input_empty=1\n");
        return;
    }

    ClearIdleOutboxArtifacts("text_submit_idle_stale_response");
    if (HasQueuedOrPlayingReply())
    {
        InterruptBridgeReplyAndPlayback("text_submit_interrupt");
    }

    const LocationSnapshot location = CapturePlayerLocation();
    PlayerCharacter* player = GetPlayer();
    const std::string requestMetadata = BuildTextRequestMetadata(player, &g_state.pendingSpeaker);
    LogLine("Text request metadata: %s", requestMetadata.empty() ? "<empty>" : requestMetadata.c_str());

    if (!WriteRequest(g_state.pendingNpcKey, g_state.pendingNpcName, submitted.value(), location, requestMetadata))
    {
        ShowHudMessage("Bridge request write failed.");
        g_state.pendingNpcKey.clear();
        g_state.pendingNpcName.clear();
        g_state.pendingSpeaker = {};
        ReleaseConversationHold("request_write_failed");
        return;
    }

    g_state.awaitingReply = true;
    g_state.awaitingVoiceReply = false;
    RememberNpcTarget(g_state.pendingNpcKey, g_state.pendingNpcName, g_state.pendingSpeaker);

    std::ostringstream diag;
    diag << "request=1\n";
    diag << "npc_key=" << g_state.pendingNpcKey << "\n";
    diag << "npc_name=" << EscapeForDiag(g_state.pendingNpcName) << "\n";
    diag << "player_text=" << EscapeForDiag(submitted.value()) << "\n";
    diag << "location_major=" << EscapeForDiag(location.major) << "\n";
    diag << "location_minor=" << EscapeForDiag(location.minor) << "\n";
    diag << "location_cell=" << EscapeForDiag(location.cell) << "\n";
    diag << "location_worldspace=" << EscapeForDiag(location.worldspace) << "\n";
    diag << "location_region=" << EscapeForDiag(location.region) << "\n";
    diag << "request_metadata=" << EscapeForDiag(requestMetadata) << "\n";
    WriteDiagnostics(diag.str());

}

void CloseVoiceCaptureHandle()
{
    auto& capture = g_state.voiceCapture;
    if (capture.waveIn)
    {
        waveInStop(capture.waveIn);
        waveInReset(capture.waveIn);
        for (auto& buffer : capture.buffers)
        {
            if (buffer.header.dwFlags & WHDR_PREPARED)
            {
                waveInUnprepareHeader(capture.waveIn, &buffer.header, sizeof(WAVEHDR));
            }
        }
        waveInClose(capture.waveIn);
        capture.waveIn = nullptr;
    }
    capture.buffers.clear();
}

bool StartVoiceCaptureWithResolvedTarget(const ResolvedNpcTarget& target)
{
    if (!target.ref || target.npcKey.empty() || target.npcName.empty())
    {
        return false;
    }

    ClearIdleOutboxArtifacts("voice_capture_idle_stale_response");
    if (HasQueuedOrPlayingReply())
    {
        InterruptBridgeReplyAndPlayback("voice_capture_interrupt");
    }

    AbortVoiceCapture("start_new_voice_capture", false);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kVoiceCaptureChannels;
    format.nSamplesPerSec = kVoiceCaptureSampleRate;
    format.wBitsPerSample = kVoiceCaptureBitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN waveIn = nullptr;
    const MMRESULT openResult = waveInOpen(&waveIn, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR || !waveIn)
    {
        ShowHudMessage("Microphone unavailable.");
        LogLine("Failed to open microphone input: %u.", static_cast<unsigned>(openResult));
        return false;
    }

    auto& capture = g_state.voiceCapture;
    capture.active = false;
    capture.transcribing = false;
    capture.adminMode = false;
    capture.npcKey = target.npcKey;
    capture.npcName = target.npcName;
    capture.speaker = CaptureSpeakerSnapshot(target.ref);
    capture.waveIn = waveIn;
    capture.startedTick = GetTickCount();
    capture.subtitleRefreshTick = 0;
    capture.capturedPcm.clear();
    capture.buffers.clear();

    const DWORD bytesPerBuffer = (kVoiceCaptureSampleRate * (kVoiceCaptureBitsPerSample / 8) * kVoiceCaptureChannels * kVoiceCaptureBufferMs) / 1000;
    bool bufferFailure = false;
    capture.buffers.resize(kVoiceCaptureBufferCount);
    for (size_t i = 0; i < kVoiceCaptureBufferCount; ++i)
    {
        VoiceCaptureBuffer& buffer = capture.buffers[i];
        buffer.storage.resize(bytesPerBuffer);
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.storage.data());
        buffer.header.dwBufferLength = bytesPerBuffer;
        const MMRESULT prepareResult = waveInPrepareHeader(capture.waveIn, &buffer.header, sizeof(WAVEHDR));
        const MMRESULT addResult = prepareResult == MMSYSERR_NOERROR
            ? waveInAddBuffer(capture.waveIn, &buffer.header, sizeof(WAVEHDR))
            : prepareResult;
        if (prepareResult != MMSYSERR_NOERROR || addResult != MMSYSERR_NOERROR)
        {
            LogLine("Failed to prepare microphone buffer %u: prepare=%u add=%u.", static_cast<unsigned>(i), static_cast<unsigned>(prepareResult), static_cast<unsigned>(addResult));
            bufferFailure = true;
            break;
        }
    }

    if (bufferFailure || waveInStart(capture.waveIn) != MMSYSERR_NOERROR)
    {
        CloseVoiceCaptureHandle();
        capture.npcKey.clear();
        capture.npcName.clear();
        capture.speaker = {};
        ShowHudMessage("Failed to start microphone capture.");
        return false;
    }

    capture.active = true;
    RememberNpcTarget(target.npcKey, target.npcName, capture.speaker);
    EngageConversationHold(target.npcKey, target.npcName, capture.speaker);
    capture.subtitleRefreshTick = 0;
    ShowHudMessage("Listening...");
    return true;
}

bool StartAmbientVoiceCapture()
{
    ClearIdleOutboxArtifacts("voice_capture_idle_stale_response");
    if (HasQueuedOrPlayingReply())
    {
        InterruptBridgeReplyAndPlayback("voice_capture_interrupt");
    }

    AbortVoiceCapture("start_new_voice_capture", false);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kVoiceCaptureChannels;
    format.nSamplesPerSec = kVoiceCaptureSampleRate;
    format.wBitsPerSample = kVoiceCaptureBitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN waveIn = nullptr;
    const MMRESULT openResult = waveInOpen(&waveIn, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR || !waveIn)
    {
        ShowHudMessage("Microphone unavailable.");
        LogLine("Failed to open microphone input: %u.", static_cast<unsigned>(openResult));
        return false;
    }

    auto& capture = g_state.voiceCapture;
    capture.active = false;
    capture.transcribing = false;
    capture.adminMode = false;
    capture.npcKey.clear();
    capture.npcName.clear();
    capture.speaker = {};
    capture.waveIn = waveIn;
    capture.startedTick = GetTickCount();
    capture.subtitleRefreshTick = 0;
    capture.capturedPcm.clear();
    capture.buffers.clear();

    const DWORD bytesPerBuffer = (kVoiceCaptureSampleRate * (kVoiceCaptureBitsPerSample / 8) * kVoiceCaptureChannels * kVoiceCaptureBufferMs) / 1000;
    bool bufferFailure = false;
    capture.buffers.resize(kVoiceCaptureBufferCount);
    for (size_t i = 0; i < kVoiceCaptureBufferCount; ++i)
    {
        VoiceCaptureBuffer& buffer = capture.buffers[i];
        buffer.storage.resize(bytesPerBuffer);
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.storage.data());
        buffer.header.dwBufferLength = bytesPerBuffer;
        const MMRESULT prepareResult = waveInPrepareHeader(capture.waveIn, &buffer.header, sizeof(WAVEHDR));
        const MMRESULT addResult = prepareResult == MMSYSERR_NOERROR
            ? waveInAddBuffer(capture.waveIn, &buffer.header, sizeof(WAVEHDR))
            : prepareResult;
        if (prepareResult != MMSYSERR_NOERROR || addResult != MMSYSERR_NOERROR)
        {
            LogLine("Failed to prepare microphone buffer %u: prepare=%u add=%u.", static_cast<unsigned>(i), static_cast<unsigned>(prepareResult), static_cast<unsigned>(addResult));
            bufferFailure = true;
            break;
        }
    }

    if (bufferFailure || waveInStart(capture.waveIn) != MMSYSERR_NOERROR)
    {
        CloseVoiceCaptureHandle();
        capture.npcKey.clear();
        capture.npcName.clear();
        capture.speaker = {};
        ShowHudMessage("Failed to start microphone capture.");
        return false;
    }

    capture.active = true;
    capture.subtitleRefreshTick = 0;
    ShowHudMessage("Listening...");
    return true;
}

bool StartAdminVoiceCapture()
{
    ClearIdleOutboxArtifacts("admin_voice_capture_idle_stale_response");
    if (HasQueuedOrPlayingReply())
    {
        InterruptBridgeReplyAndPlayback("admin_voice_capture_interrupt");
    }

    AbortVoiceCapture("start_new_admin_voice_capture", false);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kVoiceCaptureChannels;
    format.nSamplesPerSec = kVoiceCaptureSampleRate;
    format.wBitsPerSample = kVoiceCaptureBitsPerSample;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN waveIn = nullptr;
    const MMRESULT openResult = waveInOpen(&waveIn, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (openResult != MMSYSERR_NOERROR || !waveIn)
    {
        ShowHudMessage("Microphone unavailable.");
        LogLine("Failed to open microphone input for Todd voice: %u.", static_cast<unsigned>(openResult));
        return false;
    }

    auto& capture = g_state.voiceCapture;
    capture.active = false;
    capture.transcribing = false;
    capture.adminMode = true;
    capture.npcKey = kAdminNpcKey;
    capture.npcName = kAdminNpcName;
    capture.speaker = {};
    capture.waveIn = waveIn;
    capture.startedTick = GetTickCount();
    capture.subtitleRefreshTick = 0;
    capture.capturedPcm.clear();
    capture.buffers.clear();

    const DWORD bytesPerBuffer = (kVoiceCaptureSampleRate * (kVoiceCaptureBitsPerSample / 8) * kVoiceCaptureChannels * kVoiceCaptureBufferMs) / 1000;
    bool bufferFailure = false;
    capture.buffers.resize(kVoiceCaptureBufferCount);
    for (size_t i = 0; i < kVoiceCaptureBufferCount; ++i)
    {
        VoiceCaptureBuffer& buffer = capture.buffers[i];
        buffer.storage.resize(bytesPerBuffer);
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.storage.data());
        buffer.header.dwBufferLength = bytesPerBuffer;
        const MMRESULT prepareResult = waveInPrepareHeader(capture.waveIn, &buffer.header, sizeof(WAVEHDR));
        const MMRESULT addResult = prepareResult == MMSYSERR_NOERROR
            ? waveInAddBuffer(capture.waveIn, &buffer.header, sizeof(WAVEHDR))
            : prepareResult;
        if (prepareResult != MMSYSERR_NOERROR || addResult != MMSYSERR_NOERROR)
        {
            LogLine("Failed to prepare Todd microphone buffer %u: prepare=%u add=%u.", static_cast<unsigned>(i), static_cast<unsigned>(prepareResult), static_cast<unsigned>(addResult));
            bufferFailure = true;
            break;
        }
    }

    if (bufferFailure || waveInStart(capture.waveIn) != MMSYSERR_NOERROR)
    {
        CloseVoiceCaptureHandle();
        capture.adminMode = false;
        capture.npcKey.clear();
        capture.npcName.clear();
        capture.speaker = {};
        ShowHudMessage("Failed to start Todd voice capture.");
        return false;
    }

    capture.active = true;
    capture.subtitleRefreshTick = 0;
    ShowHudMessage("Listening to Todd...");
    return true;
}

void PollVoiceCaptureBuffers()
{
    auto& capture = g_state.voiceCapture;
    if (!capture.active || !capture.waveIn)
    {
        return;
    }

    for (auto& buffer : capture.buffers)
    {
        if ((buffer.header.dwFlags & WHDR_DONE) == 0)
        {
            continue;
        }

        if (buffer.header.dwBytesRecorded > 0)
        {
            const BYTE* begin = reinterpret_cast<const BYTE*>(buffer.header.lpData);
            capture.capturedPcm.insert(capture.capturedPcm.end(), begin, begin + buffer.header.dwBytesRecorded);
        }

        buffer.header.dwBytesRecorded = 0;
        buffer.header.dwFlags &= ~WHDR_DONE;
        if (capture.active && capture.waveIn)
        {
            const MMRESULT addResult = waveInAddBuffer(capture.waveIn, &buffer.header, sizeof(WAVEHDR));
            if (addResult != MMSYSERR_NOERROR)
            {
                LogLine("Failed to recycle microphone buffer: %u.", static_cast<unsigned>(addResult));
                AbortVoiceCapture("voice_capture_buffer_recycle_failed");
                return;
            }
        }
    }
}

void AbortVoiceCapture(const char* reason, bool releaseHold)
{
    auto& capture = g_state.voiceCapture;
    if (!capture.active && !capture.waveIn)
    {
        capture.transcribing = false;
        capture.subtitleRefreshTick = 0;
        capture.adminMode = false;
        capture.npcKey.clear();
        capture.npcName.clear();
        capture.speaker = {};
        capture.capturedPcm.clear();
        return;
    }

    capture.active = false;
    capture.transcribing = false;
    capture.subtitleRefreshTick = 0;
    capture.adminMode = false;
    CloseVoiceCaptureHandle();
    capture.capturedPcm.clear();
    capture.npcKey.clear();
    capture.npcName.clear();
    capture.speaker = {};
    if (releaseHold)
    {
        ReleaseConversationHold(reason ? reason : "voice_capture_cancelled");
    }
    ClearDialogSubtitle();
}

void FinishVoiceCaptureAndSubmit()
{
    auto& capture = g_state.voiceCapture;
    if (!capture.waveIn)
    {
        AbortVoiceCapture("voice_capture_finish_without_device");
        return;
    }

    capture.active = false;
    capture.transcribing = true;
    capture.subtitleRefreshTick = 0;
    ShowHudMessage("Transcribing...");

    waveInStop(capture.waveIn);
    PollVoiceCaptureBuffers();
    waveInReset(capture.waveIn);
    for (auto& buffer : capture.buffers)
    {
        if (buffer.header.dwBytesRecorded > 0)
        {
            const BYTE* begin = reinterpret_cast<const BYTE*>(buffer.header.lpData);
            capture.capturedPcm.insert(capture.capturedPcm.end(), begin, begin + buffer.header.dwBytesRecorded);
            buffer.header.dwBytesRecorded = 0;
        }
    }
    CloseVoiceCaptureHandle();

    const DWORD blockAlign = (kVoiceCaptureBitsPerSample / 8) * kVoiceCaptureChannels;
    const DWORD audioMs = blockAlign
        ? static_cast<DWORD>((static_cast<unsigned long long>(capture.capturedPcm.size()) * 1000ull) / (static_cast<unsigned long long>(kVoiceCaptureSampleRate) * blockAlign))
        : 0;

    if (capture.capturedPcm.empty() || audioMs < kVoiceCaptureMinimumMs)
    {
        ShowHudMessage("Didn't catch that.");
        AbortVoiceCapture("voice_capture_too_short");
        return;
    }

    const std::vector<BYTE> wavBytes = BuildWaveBytesFromPcm(capture.capturedPcm);
    const LocationSnapshot location = CapturePlayerLocation();
    const bool adminMode = capture.adminMode;
    g_state.pendingNpcKey = capture.npcKey;
    g_state.pendingNpcName = capture.npcName;
    g_state.pendingSpeaker = capture.speaker;

    if (!WriteVoiceRequest(capture.npcKey, capture.npcName, capture.speaker, wavBytes, location, adminMode))
    {
        ShowHudMessage("Voice request write failed.");
        g_state.pendingNpcKey.clear();
        g_state.pendingNpcName.clear();
        g_state.pendingSpeaker = {};
        AbortVoiceCapture("voice_request_write_failed");
        return;
    }

    g_state.awaitingReply = true;
    g_state.awaitingVoiceReply = true;
    RememberNpcTarget(capture.npcKey, capture.npcName, capture.speaker);

    std::ostringstream diag;
    diag << "voice_request=1\n";
    diag << "voice_target=" << (adminMode ? "admin_todd" : "live_chat") << "\n";
    diag << "npc_key=" << capture.npcKey << "\n";
    diag << "npc_name=" << EscapeForDiag(capture.npcName) << "\n";
    diag << "audio_ms=" << audioMs << "\n";
    diag << "audio_bytes=" << wavBytes.size() << "\n";
    diag << "location_major=" << EscapeForDiag(location.major) << "\n";
    diag << "location_minor=" << EscapeForDiag(location.minor) << "\n";
    WriteDiagnostics(diag.str());

    capture.transcribing = false;
    capture.subtitleRefreshTick = 0;
    capture.adminMode = false;
    capture.capturedPcm.clear();
    capture.npcKey.clear();
    capture.npcName.clear();
    capture.speaker = {};
}

void StartChatWithResolvedTarget(const ResolvedNpcTarget& target)
{
    if (!target.ref || target.npcKey.empty() || target.npcName.empty())
    {
        return;
    }

    g_state.awaitingInput = true;
    g_state.inputMenuSeenVisible = false;
    g_state.inputStartedTick = GetTickCount();
    g_state.staleTextInputCloseRetryTick = 0;
    ResetTextInputKeyWatcher();
    g_state.pendingNpcKey = target.npcKey;
    g_state.pendingNpcName = target.npcName;
    g_state.pendingSpeaker = CaptureSpeakerSnapshot(target.ref);
    RememberNpcTarget(target.npcKey, target.npcName, g_state.pendingSpeaker);

    if (!OpenInGameTextInput("Speak to " + target.npcName))
    {
        g_state.awaitingInput = false;
        g_state.inputStartedTick = 0;
        ClearTextInputKeyWatcher();
        g_state.pendingNpcKey.clear();
        g_state.pendingNpcName.clear();
        g_state.pendingSpeaker = {};
        ShowHudMessage("Failed to open in-game chat.");
        return;
    }

    EngageConversationHold(target.npcKey, target.npcName, g_state.pendingSpeaker);

    std::ostringstream diag;
    diag << "input_open=1\n";
    diag << "npc_key=" << target.npcKey << "\n";
    diag << "npc_name=" << EscapeForDiag(target.npcName) << "\n";
    diag << "distance_m=" << (std::sqrt(target.distanceSquared) / kGameUnitsPerMeter) << "\n";
    WriteDiagnostics(diag.str());
}

void StartAmbientChatInput()
{
    g_state.awaitingInput = true;
    g_state.inputMenuSeenVisible = false;
    g_state.inputStartedTick = GetTickCount();
    g_state.staleTextInputCloseRetryTick = 0;
    ResetTextInputKeyWatcher();
    g_state.pendingNpcKey.clear();
    g_state.pendingNpcName.clear();
    g_state.pendingSpeaker = {};

    if (!OpenInGameTextInput("Speak"))
    {
        g_state.awaitingInput = false;
        g_state.inputStartedTick = 0;
        ClearTextInputKeyWatcher();
        g_state.pendingNpcKey.clear();
        g_state.pendingNpcName.clear();
        g_state.pendingSpeaker = {};
        ShowHudMessage("Failed to open in-game chat.");
        return;
    }

    WriteDiagnostics("input_open=1\nchat_target=ambient_group\n");
}

void StartAdminChatInput()
{
    g_state.awaitingInput = true;
    g_state.inputMenuSeenVisible = false;
    g_state.inputStartedTick = GetTickCount();
    g_state.staleTextInputCloseRetryTick = 0;
    ResetTextInputKeyWatcher();
    g_state.pendingNpcKey = kAdminNpcKey;
    g_state.pendingNpcName = kAdminNpcName;
    g_state.pendingSpeaker = {};

    if (!OpenInGameTextInput(kAdminNpcName))
    {
        g_state.awaitingInput = false;
        g_state.inputStartedTick = 0;
        ClearTextInputKeyWatcher();
        g_state.pendingNpcKey.clear();
        g_state.pendingNpcName.clear();
        g_state.pendingSpeaker = {};
        ShowHudMessage("Failed to open Todd chat.");
        return;
    }

    std::ostringstream diag;
    diag << "input_open=1\n";
    diag << "chat_target=admin_todd\n";
    diag << "npc_key=" << kAdminNpcKey << "\n";
    diag << "npc_name=" << kAdminNpcName << "\n";
    WriteDiagnostics(diag.str());
}

void UpdateVoiceCaptureHotkey()
{
    auto& capture = g_state.voiceCapture;
    if (!GameWindowHasFocus())
    {
        if (capture.active)
        {
            AbortVoiceCapture("voice_capture_focus_lost");
        }
        PrimeHotkeyEdgeStateFromKeyboard();
        return;
    }

    if (g_state.ignoreHotkeysUntilTick && GetTickCount() < g_state.ignoreHotkeysUntilTick)
    {
        PrimeHotkeyEdgeStateFromKeyboard();
        return;
    }
    g_state.ignoreHotkeysUntilTick = 0;

    const bool keyDown = (GetAsyncKeyState(kVoiceChatVirtualKey) & 0x8000) != 0;
    const bool adminKeyDown = (GetAsyncKeyState(kAdminVoiceChatVirtualKey) & 0x8000) != 0;
    const bool pressedNow = keyDown && !capture.keyDownLastFrame;
    const bool adminPressedNow = adminKeyDown && !capture.adminKeyDownLastFrame;
    const bool releasedNow = !keyDown && capture.keyDownLastFrame;
    const bool adminReleasedNow = !adminKeyDown && capture.adminKeyDownLastFrame;
    capture.keyDownLastFrame = keyDown;
    capture.adminKeyDownLastFrame = adminKeyDown;

    if (capture.active)
    {
        const bool submitNow = capture.adminMode ? adminReleasedNow : releasedNow;
        if (submitNow)
        {
            FinishVoiceCaptureAndSubmit();
        }
        return;
    }

    if (!pressedNow && !adminPressedNow)
    {
        return;
    }

    if (g_state.saveStateSyncPending)
    {
        if (!g_state.saveStateSyncHudMessageTick || (GetTickCount() - g_state.saveStateSyncHudMessageTick) >= kSaveStateSyncHudCooldownMs)
        {
            ShowHudMessage("Bridge syncing save state. Wait a moment.");
            g_state.saveStateSyncHudMessageTick = GetTickCount();
        }
        return;
    }

    RecoverStaleReplyState();

    if (g_state.awaitingInput || capture.transcribing)
    {
        return;
    }

    if (IsTextInputMenuActive())
    {
        return;
    }

    if (adminPressedNow)
    {
        StartAdminVoiceCapture();
        return;
    }

    PlayerCharacter* player = GetPlayer();
    const auto target = FindFocusedMappedNpcForChat(player);
    if (target.has_value())
    {
        StartVoiceCaptureWithResolvedTarget(*target);
        return;
    }

    if (FindNearbyMappedNpcsForGroupChat(player, kGroupChatNearbyRadiusMeters).empty())
    {
        ShowHudMessage("No mapped NPC within 10 meters.");
        return;
    }

    StartAmbientVoiceCapture();
}

void OnMainGameLoop()
{
    const bool gameWindowFocused = GameWindowHasFocus();
    const DWORD now = GetTickCount();
    if (!gameWindowFocused)
    {
        if (g_state.gameWindowFocusedLastFrame)
        {
            LogLine("Game window lost focus; clearing bridge hotkey state.");
        }
        g_state.gameWindowFocusedLastFrame = false;
        g_state.ignoreHotkeysUntilTick = now + 500;
        if (g_state.awaitingInput)
        {
            CancelAwaitingTextInput("input_focus_lost", "input_cancelled");
        }
        else if (g_state.bridgeTextInputOwned || IsTextInputMenuActive())
        {
            std::error_code ec;
            fs::remove(UiSubmitPath(), ec);
            ForceCloseTextInputMenu("focus lost stale text input");
            g_state.bridgeTextInputOwned = IsTextInputMenuActive();
            g_state.staleTextInputCloseRetryTick = now + 250;
        }
        PrimeHotkeyEdgeStateFromKeyboard();
        if (g_state.voiceCapture.active)
        {
            AbortVoiceCapture("voice_capture_focus_lost", false);
        }
        return;
    }

    if (!g_state.gameWindowFocusedLastFrame)
    {
        g_state.gameWindowFocusedLastFrame = true;
        g_state.ignoreHotkeysUntilTick = now + 500;
        PrimeHotkeyEdgeStateFromKeyboard();
        if ((g_state.bridgeTextInputOwned || IsTextInputMenuActive()) && !g_state.awaitingInput)
        {
            ForceCloseTextInputMenu("focus regained stale text input");
            g_state.bridgeTextInputOwned = IsTextInputMenuActive();
            g_state.staleTextInputCloseRetryTick = now + 250;
        }
        LogLine("Game window regained focus; bridge hotkeys suppressed briefly.");
    }

    if (g_state.dialogSubtitleActive && g_state.dialogSubtitleHideTick && GetTickCount() >= g_state.dialogSubtitleHideTick)
    {
        ClearDialogSubtitle();
    }

    if (g_state.voiceCapture.active)
    {
        const DWORD now = GetTickCount();
        if (!g_state.voiceCapture.subtitleRefreshTick || now >= g_state.voiceCapture.subtitleRefreshTick)
        {
            g_state.voiceCapture.subtitleRefreshTick = now + 700;
        }
    }

    UpdateActiveSoundPositions();
    CleanupFinishedSounds();
    PollSaveStateSyncAck();

    if (!g_state.loadedIntoGame || !GetPlayer())
    {
        return;
    }

    UpdateVoiceBootstrapStatus();
    LoadDebugConfigIfNeeded(false);
    PollNativeActionCommands();
    // --- perf instrumentation (temporary): time the lip-sync + streaming phases to
    // locate the in-game speech lag, logged via "frame_perf_slow" trace events. ---
    static double s_perfAnimMs = 0.0;
    LARGE_INTEGER s_perfFreq;
    QueryPerformanceFrequency(&s_perfFreq);
    LARGE_INTEGER _pa0;
    QueryPerformanceCounter(&_pa0);
    UpdateSpeechAnimation();
    LARGE_INTEGER _pa1;
    QueryPerformanceCounter(&_pa1);
    s_perfAnimMs = static_cast<double>(_pa1.QuadPart - _pa0.QuadPart) * 1000.0
        / static_cast<double>(s_perfFreq.QuadPart);
    UpdateConversationHold();
    PollVoiceCaptureBuffers();
    UpdateVoiceCaptureHotkey();

    if (g_state.awaitingInput)
    {
        ConsumeSubmittedInput();
    }

    if (RecoverStaleTextInputMenu("stale text input"))
    {
        return;
    }

    LARGE_INTEGER _pc0;
    QueryPerformanceCounter(&_pc0);
    if (g_state.awaitingReply
        || (g_debugConfig.drainQueuedChunksAfterFinal
            && (!g_state.pendingAudioChunks.empty() || HasPendingChunkFiles())))
    {
        ConsumeAudioChunks();
        PlayQueuedAudioChunk();
    }
    LARGE_INTEGER _pc1;
    QueryPerformanceCounter(&_pc1);
    // Phase 3: drive the single streaming buffer every frame (lip-sync scheduling +
    // end-detection) even after chunk delivery stops, while it plays out.
    UpdateStreamingVoice();
    LARGE_INTEGER _pc2;
    QueryPerformanceCounter(&_pc2);
    // --- perf instrumentation (temporary): log when any speech phase spikes. ---
    {
        const double _chunksMs = static_cast<double>(_pc1.QuadPart - _pc0.QuadPart) * 1000.0
            / static_cast<double>(s_perfFreq.QuadPart);
        const double _streamMs = static_cast<double>(_pc2.QuadPart - _pc1.QuadPart) * 1000.0
            / static_cast<double>(s_perfFreq.QuadPart);
        double _maxMs = s_perfAnimMs;
        if (_chunksMs > _maxMs) _maxMs = _chunksMs;
        if (_streamMs > _maxMs) _maxMs = _streamMs;
        static DWORD s_perfLastLog = 0;
        const DWORD _pnow = GetTickCount();
        if (g_state.streamActive && _maxMs > 6.0 && (_pnow - s_perfLastLog) > 400)
        {
            s_perfLastLog = _pnow;
            TraceRequestEvent(g_state.activeRequestId, "frame_perf_slow", {},
                {
                    { "anim_ms", s_perfAnimMs },
                    { "chunks_ms", _chunksMs },
                    { "stream_ms", _streamMs },
                });
        }
    }

    if (g_state.awaitingReply)
    {
        ConsumeReply();
        RecoverStaleReplyState();
    }

    WriteRuntimeHeartbeatIfNeeded(false);

    if (g_state.ignoreHotkeysUntilTick && GetTickCount() < g_state.ignoreHotkeysUntilTick)
    {
        PrimeHotkeyEdgeStateFromKeyboard();
        return;
    }
    g_state.ignoreHotkeysUntilTick = 0;

    if (g_state.awaitingInput || g_state.voiceCapture.active || IsTextInputMenuActive())
    {
        g_state.keyDownLastFrame = (GetAsyncKeyState(kChatVirtualKey) & 0x8000) != 0;
        g_state.adminKeyDownLastFrame = (GetAsyncKeyState(kAdminChatVirtualKey) & 0x8000) != 0;
        return;
    }

    const bool adminKeyDown = (GetAsyncKeyState(kAdminChatVirtualKey) & 0x8000) != 0;
    const bool adminPressedNow = adminKeyDown && !g_state.adminKeyDownLastFrame;
    g_state.adminKeyDownLastFrame = adminKeyDown;

    const bool keyDown = (GetAsyncKeyState(kChatVirtualKey) & 0x8000) != 0;
    const bool pressedNow = keyDown && !g_state.keyDownLastFrame;
    g_state.keyDownLastFrame = keyDown;

    if (adminPressedNow || pressedNow)
    {
        ClearIdleOutboxArtifacts("chat_hotkey_idle_stale_response");
        if (HasQueuedOrPlayingReply())
        {
            InterruptBridgeReplyAndPlayback(adminPressedNow ? "admin_chat_hotkey_interrupt" : "chat_hotkey_interrupt");
        }
    }

    if (adminPressedNow)
    {
        if (g_state.saveStateSyncPending)
        {
            if (!g_state.saveStateSyncHudMessageTick || (GetTickCount() - g_state.saveStateSyncHudMessageTick) >= kSaveStateSyncHudCooldownMs)
            {
                ShowHudMessage("Bridge syncing save state. Wait a moment.");
                g_state.saveStateSyncHudMessageTick = GetTickCount();
            }
            return;
        }

        StartAdminChatInput();
        return;
    }

    if (!pressedNow)
    {
        return;
    }

    if (g_state.saveStateSyncPending)
    {
        if (!g_state.saveStateSyncHudMessageTick || (GetTickCount() - g_state.saveStateSyncHudMessageTick) >= kSaveStateSyncHudCooldownMs)
        {
            ShowHudMessage("Bridge syncing save state. Wait a moment.");
            g_state.saveStateSyncHudMessageTick = GetTickCount();
        }
        return;
    }

    if (g_state.awaitingInput)
    {
        return;
    }

    PlayerCharacter* player = GetPlayer();
    const auto target = FindFocusedMappedNpcForChat(player);
    if (target.has_value())
    {
        StartChatWithResolvedTarget(*target);
        return;
    }

    if (FindNearbyMappedNpcsForGroupChat(player, kGroupChatNearbyRadiusMeters).empty())
    {
        ShowHudMessage("No mapped NPC within 10 meters.");
        return;
    }

    StartAmbientChatInput();
}

void HandleNvseMessage(NVSEMessagingInterface::Message* msg)
{
    switch (msg->type)
    {
    case NVSEMessagingInterface::kMessage_DeferredInit:
    case NVSEMessagingInterface::kMessage_PostLoad:
        EnsureBridgeDirectories();
        MaybeRequestBridgeStackStartup("plugin_init");
        LogLine("FNV bridge native plugin initialized.");
        break;

    case NVSEMessagingInterface::kMessage_SaveGame:
    {
        const std::string savePath = msg && msg->data ? reinterpret_cast<const char*>(msg->data) : "";
        if (!savePath.empty())
        {
            DispatchSaveStateEvent("save", savePath, false);
        }
        break;
    }

    case NVSEMessagingInterface::kMessage_NewGame:
        g_state.loadedIntoGame = true;
        ResetRuntimeState();
        EnsureBridgeDirectories();
        MaybeRequestBridgeStackStartup("new_game");
        g_state.pendingLoadSavePath.clear();
        DispatchSaveStateEvent("new_game", "", true);
        LogLine("Game session ready.");
        break;

    case NVSEMessagingInterface::kMessage_ExitToMainMenu:
    case NVSEMessagingInterface::kMessage_ExitGame:
        g_state.loadedIntoGame = false;
        ResetRuntimeState();
        g_state.pendingLoadSavePath.clear();
        LogLine("Game session reset.");
        break;

    case NVSEMessagingInterface::kMessage_PreLoadGame:
        g_state.loadedIntoGame = false;
        ResetRuntimeState();
        g_state.pendingLoadSavePath = msg && msg->data ? reinterpret_cast<const char*>(msg->data) : "";
        LogLine("Preparing load for %s.", g_state.pendingLoadSavePath.c_str());
        break;

    case NVSEMessagingInterface::kMessage_PostLoadGame:
    {
        const bool loadSucceeded = msg && msg->data != nullptr;
        if (!loadSucceeded)
        {
            g_state.loadedIntoGame = false;
            ResetRuntimeState();
            g_state.pendingLoadSavePath.clear();
            LogLine("Game load failed.");
            break;
        }

        g_state.loadedIntoGame = true;
        ResetRuntimeState();
        EnsureBridgeDirectories();
        MaybeRequestBridgeStackStartup("post_load_game");
        std::string savePath = g_state.pendingLoadSavePath;
        if (savePath.empty() && g_serialization && g_serialization->GetSavePath)
        {
            const char* currentSavePath = g_serialization->GetSavePath();
            if (currentSavePath)
            {
                savePath = currentSavePath;
            }
        }
        g_state.pendingLoadSavePath.clear();
        DispatchSaveStateEvent("load", savePath, true);
        LogLine("Game session ready.");
        break;
    }

    case NVSEMessagingInterface::kMessage_MainGameLoop:
        OnMainGameLoop();
        break;

    default:
        break;
    }
}
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = kPluginName;
    info->version = kPluginVersion;

    if (nvse->nvseVersion < kMinimumNvseVersion)
    {
        return false;
    }

    if (nvse->isEditor)
    {
        return false;
    }

    if (nvse->runtimeVersion < RUNTIME_VERSION_1_4_0_525 || nvse->isNogore)
    {
        return false;
    }

    return true;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* nvse)
{
    g_nvse = nvse;
    g_pluginHandle = nvse->GetPluginHandle();
    g_serialization = static_cast<NVSESerializationInterface*>(nvse->QueryInterface(kInterface_Serialization));
    g_messaging = static_cast<NVSEMessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
    g_scriptInterface = static_cast<NVSEScriptInterface*>(nvse->QueryInterface(kInterface_Script));
    if (!g_messaging || !g_scriptInterface)
    {
        return false;
    }

    g_messaging->RegisterListener(g_pluginHandle, "NVSE", HandleNvseMessage);
    EnsureBridgeDirectories();
    EnsureInputCallbackScript();
    EnsureDialoguePlaybackScripts();
    LogLine("FNV bridge native plugin loaded.");
    return true;
}
