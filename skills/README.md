# Halos Outside-In Safety Skills

Skills for deploying and operating **Halos Outside-In Safety**. Each subdirectory under `skills/` is a self-contained skill following the [agentskills.io](https://agentskills.io/specification) specification, with `name`, `description`, and `version` declared in its `SKILL.md` frontmatter.

These are a **developer-side tool**: a coding agent (Claude Code, Codex, or any agentskills.io-compatible host) loads them to deploy and operate the system from natural language.

## Catalog

| Skill | Description |
|---|---|
| [hoisa-deploy-profile](hoisa-deploy-profile/SKILL.md) | Select, configure, deploy, verify, debug, or tear down a Halos profile (`base` / `sil` / `hil`) on top of the VSS Blueprint perception backend. Chains the VSS `vss-deploy-profile` skill to bring up perception first (it does not bundle it). |

## Install (recommended: ask your coding agent)

Open this repository in your coding agent (Claude Code, Codex, Cursor, or any other agentskills.io-compatible host) and paste the following prompt:

> Read `skills/README.md` and every `SKILL.md` under `skills/`, then install each skill for this host by symlinking its folder into the host's skills directory:
>
> - Claude Code: `~/.claude/skills/<name>/`
> - Codex: `~/.codex/skills/<name>/`
> - agentskills.io hosts: `~/.agents/skills/<name>/`
>
> Symlink each folder rather than copying it so a `git pull` here keeps installs current. Also install the `vss-deploy-profile` skill from the VSS Blueprint repository; the perception backend is deployed first. When done, list the skills you registered and the directory you used.

Verify with `/skills` in your agent: `hoisa-deploy-profile` (and `vss-deploy-profile`) should be listed.

## Usage

The perception backend (VSS Blueprint) is Stack 1 and must be running before Halos (Stack 2); the skill brings up both. Ask your agent in natural language, for example:

> Deploy the Halos Outside-In Safety `base` profile on this machine: bring up the VSS Blueprint perception backend, then the `base` profile, and confirm the cameras are streaming and the safety overlay reacts.

> Deploy the Halos Outside-In Safety `sil` profile: set up the VSS Blueprint perception backend, deploy the closed-loop stack, run the test scenario, and show me the safety command output.

Deployment is long-running (NGC image pulls, perception engine build, Isaac Sim shader compile), so use a high-context model with maximum effort, and expect the agent to pause for host-level input it cannot do for you: `sudo` / system installs, the NGC API key, and the GPU / profile choice. "Complete" means sim-driven MUTE / UNMUTE transitions appear in the logs, not merely that all containers are up. The host prerequisites are yours to provide (NGC access to `nvidia/halos-outside-in`, Docker, and the Isaac Sim GPU); see [`hoisa-deploy-profile/references/prerequisites.md`](hoisa-deploy-profile/references/prerequisites.md).
