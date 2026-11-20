# M18 — Mission Control: implementation plan

Design: `docs/superpowers/specs/2026-11-09-mission-control-design.md`. Test-first, one wave at a time; after each: `cmake --build build`, `cmake --build build-debug` (-Werror), `ctest --test-dir build`.

## Waves

0. Spec + plan docs; branch; empty compiling stubs and every CMake entry.
1. Executor plumbing test-first: `ToolContext`, `ConfirmRequest`, task on the signals, `cancelTask`, audit `task`;
   `Agent::setTask/setHiddenTools/cancel(note)`; the `AppController.cc:110` fix; `MainWindow::confirm` on the new
   signature. Behaviour unchanged — smoke `--ask "run uname -sr"`.
2. Pure mission core: `AgentRole`, `RoleClassifier`, `ModelFit`, `Task`, `TaskTranscript`, `ActivityFeed`,
   `Config` v5 + upgrade.
3. `TaskRunner`, `MissionControl`, `DelegateTool`; ASan on `nexa_tests "Mission*"`.
4. The app on the board, old look: `AppController` as environment, `RemoteBridge`, `main.cc`, `MainWindow` without
   the busy guard, `ActionLogDock` TaskStrip, `TaskListModel`. Verify: two typed requests in a row both run; the
   remote page via `scripts/preview-web-remote.py`; one `--hear` run (voice → task → spoken).
5. The look in `ui/`: design, palette, icons, sparkline, qss.
6. The Mission Control widgets, the top bar, `rebuildCentral`, shortcuts, the Mission settings tab; the pictures.
7. Metrics: `ProcessStats`, tiles, mic line.
8. Debug `-Werror`, ASan/UBSan, a review pass (cancel paths, pending approvals at teardown, `shared_ptr`
   lifetimes, no key logged, every file in CMake); `ARCHITECTURE.md` M18 row + deviations (single-flight programs,
   per-tool cancel limit, one voice task spoken at a time), `CLAUDE.md` layout, README; release build; restart Nexa.

---

