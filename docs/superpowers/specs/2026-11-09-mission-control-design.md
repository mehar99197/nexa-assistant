# M18 — Mission Control: tasks, agents by role, a model per task

Date: 2026-11-09 · Status: approved by the owner (plan review) · Branch: `m18-mission-control`

## Why

The owner's mock-up *Nexa Mission Control* (an agents rail, the core with microphone and process metrics, a mission
board of running and queued tasks, a live activity stream with approval cards) and their request: add several tasks
at once, run several agents at the same time, choose the model per task, and when one task needs several agents, give
each agent the model that fits its role. Decisions taken with the owner: Mission Control is the **default design**;
**every request is a task** (typed, spoken, from the phone, or added several at a time); a task's transcript opens on
click; the other eight designs stay and show tasks minimally; a lead agent of one role **delegates** sub-tasks to other
roles, each on the model its role is given (Settings → Mission, *Auto* derived from what the providers can do).

Today (M17) Nexa has one `Agent`, one `Conversation` and one provider chain; a second request while one runs is
dropped; stopping one tool stops every tool; nothing carries a task id; there are no metrics.

## Design


### B.1 The shape of the change

"Run one request" moves out of `AppController` into a **`TaskRunner`** (its own `Agent`, `Conversation`, provider
instance, model, metrics); "which requests run now" into a **`MissionControl`** mediator (queue, slots, lifecycle,
approvals, activity). `AppController` keeps the rest (control intents, system prompt, core state, voice wiring,
config) and becomes the **`ITaskEnvironment`** the board asks for providers and prompts. Its `reply*` signals stay
and mean **"the selected task"** (ChatView, VoicePipeline, TrayIcon and the remote keep working); new
`taskReply*(TaskId, …)` signals carry every task. The executor, the confirmer and the audit log learn a **task id**;
`cancelAll()` gets a sibling `cancelTask()` so pausing one task leaves the others' tools alone.

### B.2 `core/mission/` (new; no Qt Widgets)

- `AgentRole.{h,cc}` — `enum class AgentRole { Web, Code, Screen, Phone, General }`, `roleName/roleKey/roleFromKey`,
  `agentIdFor(role, ordinal)` → "BROWSER-01" / "CODE-02" / "SCREEN-04" / "PHONE-03" / "GEN-05"; `enum class Lane`
  = the five roles + Voice (the activity chips).
- `RoleClassifier.{h,cc}` (pure) — `classify(text)` by word-boundary regexes, precedence Phone > Screen > Web >
  Code > General ("on my phone…", "screen / click / window / see this", "website / url / google / log in / fill the
  form", "code / refactor / bug / unit tests / repo / cmake"); `forTool(name)` (android_* → Phone, browser_* →
  Web, screenshot/click/type → Screen). Negatives stay General: "open calculator", "volume 50", "tell me a joke".
- `ModelFit.{h,cc}` (pure) — `struct ModelChoice { provider, model; isAuto() }` (lives in `Config.h`);
  `struct ProviderFacts { id, kind, name, defaultModel, vision, imageModels, isFirstChoice }`;
  `resolve(role, MissionConfig, facts)` = the config's entry if its provider still exists, else `defaultFor`:
  Code → first present of codex, opencode, agy; Screen/Phone → the first choice if it sees, else the first provider
  with vision (an image model if `defaultModel` is blind); Web/General → Auto (the chain + current model).
  `needsOwnInstance(kind)` = `ProviderRegistry::isProgramKind`.
- `Task.{h,cc}` — `using TaskId = quint64`; `enum class TaskState { Queued, Running, AwaitingYou, Delegating,
  Paused, Done, Failed, Stopped }`; `struct Task { id, title, text, source, role, agentId, model, providerName,
  state, subStatus, progress, created/started/ended, steps, toolCalls, fastPath, parent, depth, sparkline(24),
  lastError }`; `titleFor(text)`, `estimateProgress(steps, toolCalls, state)` (monotonic, 100 at Done).
- `TaskTranscript.{h,cc}` (pure) — the events the drawer replays (User, ReplyStart, Route, Via, Delta, Action,
  Notice, Error, Finished), consecutive deltas merged.
- `ActivityFeed.{h,cc}` (pure ring buffer, capacity 500) — `ActivityEvent { time, kind, lane, task, agentId, text,
  ok }`, `push`, `events(optional<Lane>)`.
- `TaskRunner.{h,cc}` (Facade over Agent + Conversation + provider) —
  `TaskRunner(id, ToolExecutor*, ToolRegistry*, shared_ptr<IAiProvider>, model, maxSteps, hideDelegate)`;
  `start(systemPrompt, text, source, images)`, `startFastPath(...)` (= today's `AppController::runFastPath`,
  moved), `followUp`, `pause()` (= `agent->cancel("Paused by the user.")`, conversation kept), `resume()`
  (adds "Continue from where you paused." and runs again), `stop()`; signals `textDelta/route/rerouted/
  toolStarted/toolFinished/notice/finished/failed(TaskId, …)`, `metricsChanged(TaskId, Metrics{firstTokenMs,
  tokensPerSecond, steps})` (chars/4 per second since the first token; ≤ 4 Hz).
- `DelegateTool.{h,cc}` (ITool, Command) — `delegate_task { role, task, context }`; calls
  `MissionControl::delegate(parent, role, text, context, done)`; refused at depth ≥ 1 ("not available in a
  sub-task"); `cancel()` stops the child.
- `MissionControl.{h,cc}` (Mediator) —
  ```cpp
  class ITaskEnvironment {  // AppController implements it
      virtual Result<std::shared_ptr<IAiProvider>> createProvider(const QString &providerId) = 0;  // own instance
      virtual std::shared_ptr<IAiProvider> sharedProvider() = 0;  virtual QString sharedModel() = 0;  // the chain
      virtual QList<ProviderFacts> providerFacts() = 0;
      virtual QString systemPrompt(const QString &source, AgentRole role) = 0;
      virtual std::optional<IntentMatch> fastPath(const QString &text, const QString &source) = 0;
      virtual int maxSteps() = 0;
  };
  struct TaskRequest { text, source, images, optional<ModelChoice> model, optional<AgentRole> role, optional<TaskId> parent; };
  TaskId add(const TaskRequest &);  QList<TaskId> addMany(lines, source, optional<ModelChoice>);
  void followUp(TaskId, text, source);  void pause(TaskId); void resume(TaskId); void stop(TaskId); void remove(TaskId);
  void pauseAll(); void stopAll();  void approve(quint64 actionId); void deny(quint64 actionId);
  void confirm(const ConfirmRequest &, std::function<void(bool)>);   // bound as the executor's Confirmer
  // queries: task(id), tasks(), queue(), running(), slots(), awaiting(), doneToday(), isBusy(), feed(), transcript(id), metrics(id), newestRunning()
  signals: taskAdded/taskChanged/taskRemoved(TaskId), queueChanged(), countsChanged(),
           approvalRequested(TaskId, actionId, agentId, tool, question, risk), approvalSettled(actionId, allowed),
           activity(const ActivityEvent &), taskReplyStarted/Route/Via/Delta/Notice/Action/Finished/Failed(TaskId, …), anyRunningChanged(bool);
  ```
  Rules: a slot is held while Running or AwaitingYou (Paused, Delegating, Queued hold none); `maxConcurrent`
  1…8, default 3, FIFO; resume and a parent returning from Delegating go to the queue's head. **Provider per
  task**: Auto on an HTTP first choice → the shared chain + current model; Auto on a program first choice, or any
  explicit choice → `createProvider(id)` = its own instance (one warm agy/Codex process per running task — bounded
  by the slots; opencode instances share Nexa's `opencode serve`), `warmUp()` at start, released at the end; no
  chain fallback for an explicit choice (the card says so). **Approvals**: `confirm()` finds the task, sets
  AwaitingYou + "Approve run_command?", emits `approvalRequested`; `approve/deny` answer once; `stop/pause` of a
  task with a pending question answers false; a request with task 0 goes to the old modal. **Delegation**: child
  at depth 1 with the parent's source, parent → Delegating (slot released), the child's last assistant message
  (or error) becomes the parent's tool result, parent re-takes a slot at the head. **Fast path inside a task**:
  `env->fastPath(text)` first; a match runs through `startFastPath`, `task.fastPath = true`, role from the tool.
  `tick()` once a second: elapsed and sparkline shift for active tasks.

### B.3 Changes to existing core

- `core/tools/ITool.h`: `struct ToolContext { QString source; quint64 task{0}; }`; a context-aware
  `invoke(args, const ToolContext &, Done)` with a forwarding default — no existing tool changes.
- `ToolExecutor`: `execute(tool, args, const ToolContext &, Done)` (the `source` overload = task 0);
  `struct ConfirmRequest { id, tool, arguments, question, risk, source, task }` and
  `Confirmer = function<void(const ConfirmRequest &, function<void(bool)>)>`; signals gain a trailing `quint64 task`
  (`actionStarted(id, tool, args, source, task)`, `actionFinished(id, ok, summary, ms, task)` — existing connects
  stay valid); **`cancelTask(task)`**: pending confirmations of the task answer Cancelled (a late `answer()` is
  ignored); running invocations call `tool->cancel()` only when no *other* task runs the same `ITool*`, else the
  result is dropped on arrival (per-tool cancel is per tool, not per invocation — documented). `cancelAll()` stays
  for panic. `AuditLog::Entry` gains `quint64 task{0}` (written only when non-zero).
- `Agent`: `setTask(id)`, `setHiddenTools({"delegate_task"})` (left out of the specs; a call answered like a
  withheld tool), `cancel(closingNote)` (pause writes "Paused by the user."), `Agent.cc:49` → `cancelTask` when
  a task is set; `stepStarted(int)` for the runner's latency clock.
- `AppController`: owns `unique_ptr<MissionControl> mission_` (built in its constructor from `AppServices`;
  `AppServices::agent` goes); `provider_` becomes `shared_ptr` (Settings swaps the chain; running tasks keep the
  old one); `submit()` keeps the pre-task branches (panic, share screen, the AppContext redirect) then
  `mission_->add({prompt, source})` — the `busy_` guard goes; `submitFrom(text, source) → TaskId`;
  `selectTask(TaskId)` (default: the newest task from gui/voice/phone); `isBusy()` = `mission_->isBusy()` so the
  `--ask` queue in `main.cc` and `--screenshot`'s idle detection work unchanged; voice: `taskReply*` forwarded to
  `VoicePipeline` only for the one voice task being spoken (`speakingTask_`); core state: `thinking` = any runner
  streaming, `acting` from the executor's counter, `fastPath` while a fast-path task runs; `cancel()` → stop the
  selected task; `panic()` → `mission_->stopAll()` + `executor->cancelAll()` + `PanicController`; `updateConfig()`
  no longer cancels; `newConversation()` removes finished tasks; `lastHeard(QString)` re-emitted for the core card.
- `RemoteBridge`: remembers its own `TaskId`s, forwards only their `taskReply*` to the server's slots through its
  own signals (`main.cc:412-434` connects from the bridge); no more "Nexa is busy" refusal (requests queue).
- `PanicController`: `main.cc` registers the board's `stopAll`.

### B.4 Config (v5)

`struct MissionConfig { int maxConcurrent{3}; QMap<QString, ModelChoice> roleModels; int activityCapacity{500}; }`
as `config.mission`; `kVersion = 5`; `UiConfig::design` default → `"mission"`; **upgrade**: a v4 config whose
`ui.design` is absent or `"studio"` (the old default) moves to `"mission"`; a chosen design (orb, console, …) stays.
Settings → new tab **Mission** (`app/MissionSettingsPage`): "Run up to N tasks at once" (1…8) and five rows role →
provider (Auto + the configured providers) + model. Tests in `ConfigStoreTest`.

### B.5 UI

`ui/`:
- `NexaDesigns`: `WindowLayout::MissionControl` (appended; `kWindowLayoutCount = 8`), design `mission` ("Mission
  Control", Ring core, Comet motion, Cards bubbles, sans 10, palette `mission-dark`) appended after Studio so
  `resolve(unknown)` still gives Studio.
- `NexaThemes`: family "Mission" (hue 268) with hand-tuned dark tokens from the mock-up — ground `#0B0D12`, panel
  `#12141B`, raised `#191C25`, line `#232736`, text `#E7E9F2`, muted `#8A90A6`, accent/idle `#A855F7`, wake/online
  `#22C55E`, listen `#C084FC`, stt `#38BDF8`, think `#818CF8`, act/approval `#F59E0B`, speak `#E879F9`, fast
  `#F5F3FF`, error `#F87171` — contrast enforced as for the other packs; light sibling generated (54 packs).
- `NexaIcons::Kind` += Pause, Play, Check, Cross, Globe, Code, Screen, Queue, Plus.
- `ui/Sparkline.{h,cc}`: `Sparkline::paint(painter, rect, samples, line, fill, bars)` + `SparklineWidget`
  (Q_PROPERTY colours) — the agent cards' activity bars and the core card's waveform.
- `theme.qss`: `#onlineChip`, `#metaPill`, `#sectionTitle`, `#sectionNote`, the four list views, `#approvalCard`
  (`@act@` border), `#approve` (`@wake@`), `#deny` (`@errorSoft@`), `#tileLabel`/`#tileValue`, `#ghost` buttons,
  and `nexa--MissionBoard / AgentsRail / ActivityStream { qproperty-mutedColor … monoFamily }` for the delegates.

`app/` (each new class also listed in `tests/CMakeLists.txt` with its test):

| Class | What it is |
|---|---|
| `TaskListModel` (+ `ActiveTasksFilter`, `QueuedTasksFilter`, `AgentCardsFilter`) | `QAbstractListModel` over `MissionControl`; roles for id, agent id, title, state, sub-status, progress, elapsed, role, provider·model, sparkline, depth, queue position |
| `ActivityModel` | over `ActivityFeed` + `activity`; `setLane()` |
| `AgentsRail` + `AgentCardDelegate` | "AGENTS · n active · r/m slots", a fixed Voice card (24 px core + LISTENING / WAKE WORD ON), one card per slot-holding task, "+ Spawn agent" |
| `MissionBoard` + `TaskRowDelegate`, `QueueRowDelegate` | the counts line, Pause all, + Add tasks, task rows with pause/play and × hit-rects, the QUEUE section "starts when a slot frees · limit N concurrent" |
| `ActivityStream` + `ApprovalCard`, `ActivityLineDelegate` | "LIVE ACTIVITY · STREAMING", chips ALL/WEB/CODE/SCREEN/PHONE/VOICE (`#galleryChip`), one card per pending approval with Approve / Deny, the timestamped lines |
| `CoreCard` | the 120 px presence re-parented here, "Wake word on · MIC OK · −21 dB · 0 % clipped", the waveform, "last heard › …", then `MetricsTiles` |
| `MetricsTiles` | LATENCY · TOKENS/S · CPU · MEMORY tiles ("—" when idle) |
| `AddTasksDialog` | one task per line, provider (Auto + list) and model, button "Add N tasks" (also "Spawn agent") |
| `TaskTranscriptDrawer` | "BROWSER-01 · RUNNING · title" + "‹ Board"; hosts the existing `ChatView`, replays the `TaskTranscript`, follows the selected task live, a follow-up input |
| `ActionLogDock` (existing) | gains a `TaskStrip` (≤ 5 one-line rows over `ActiveTasksFilter`) — all the other designs show of tasks |
| `MissionSettingsPage` | §B.4 |

`MainWindow`: the toolbar is the top bar — `#onlineChip` "● CORE ONLINE / OFFLINE", the Provider and Model pills,
`#metaPill`s UPTIME and SLOTS r/m (Mission layout only), the policy pill, `#stopAll`, theme, Settings; New chat /
Actions / Phone buttons hidden in this layout. `rebuildCentral()` gets `case MissionControl`: rail (260–320) |
centre {`CoreCard`, `QStackedWidget` {board, drawer}} | stream (300–380); ActionLogDock hidden here (the stream
carries the actions); leaving the layout re-parents the presence back as today. `eventFilter` drops the busy
guard; Send always adds a task in this layout (Stop follows the selected task elsewhere); the model combo is
never disabled. Approvals: cards in the stream; in other layouts the `QMessageBox` becomes `Qt::NonModal` with
the agent id in its title. Keys: `Ctrl+T` Add tasks, `Ctrl+Shift+Y` / `Ctrl+Shift+N` approve / deny the oldest,
`Esc` stop the selected task, `Ctrl+Shift+Space` pause all, `Ctrl+Alt+Esc` stop all (exists). `--design mission`.

### B.6 Metrics

`system/info/ProcessStats.{h,cc}`: `parseStat(line)` (handles "(comm with spaces)"), `sample()` = this process and
every descendant by `/proc/*/stat` ppid links, CPU = Δ(utime+stime)/Δwall over all cores, RSS summed;
`ProcessStatsSampler` (QTimer 2 s) handed to `MainWindow` through `SystemStatus::processStats`. Latency and
tokens/s from `TaskRunner::Metrics` of `newestRunning()`. Mic dB from `levelChanged`, trouble text from
`VoicePipeline::microphoneTrouble()`.

### B.7 Tests

`tests/ScriptedProvider.h` extracted from `AgentTest.cc`. New: `ToolExecutorTest` (task on the signals and in the
audit line; `cancelTask` cancels only its own `slow`; shared tool → result dropped; a pending confirmation cancelled
ignores a late answer), `RoleClassifierTest` (≥ 4 positives per role, the negatives, precedence, `forTool`),
`ModelFitTest` (provider sets {agy}, {opencode+agy}, {Groq vision first + Codex}, {Groq blind + Gemini vision}; a
removed provider falls back), `ActivityFeedTest`, `TaskTest`, `TaskRunnerTest` (latency and tokens/s from scripted
chunks; fast path; NotFound → agent), `MissionControlTest` (two tasks run at once; slots=1 queues; pause/resume keeps
the history; approval routed to the right task, approve runs, deny declines; delegate: depth 1, parent Delegating,
child's text as the tool result, a child's delegate refused; stopAll; addMany; doneToday; lanes; explicit choice →
`createProvider(id)`), `ProcessStatsTest`, widget tests `MissionBoardTest`, `AgentsRailTest`, `ActivityStreamTest`,
`AddTasksDialogTest`, `TaskTranscriptDrawerTest` (offscreen, `findChild` by objectName). Updated: `AgentTest`
(setTask, hidden tools, the pause note), `ConfigStoreTest` (v5, upgrade cases), `NexaDesignsTest` (9 designs, 8
layouts), `NexaThemesTest` (54 packs, `mission-dark` contrast). Pictures: `--design mission` with two `--ask`s, in
`mission-light` too, `--design studio` (TaskStrip), `--screenshot-settings` (the Mission tab).

