#pragma once

#include <QtGlobal>

class QProcess;

namespace nexa::process {

// Configures a QProcess for clean subprocess containment before start():
// - On Unix: creates a new session, resets signal handlers, closes extra FDs,
//   and sets PR_SET_PDEATHSIG so orphans cannot outlive Nexa.
// - On Windows: suppresses console window flashes for piped CLI helpers.
void configureChild(QProcess &process);

// Terminates a spawned process and its descendant/group processes.
// When includeRoot is true, the root pid itself is also killed; when false,
// only its process group / descendants are targeted. When force is false,
// Unix sends SIGTERM instead of SIGKILL (Windows still terminates the tree).
void killTree(qint64 pid, bool force = true, bool includeRoot = true);

// Non-blocking liveness probe for a single PID (test / shutdown helper).
bool isAlive(qint64 pid);

} // namespace nexa::process
