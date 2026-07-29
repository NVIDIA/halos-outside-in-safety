# NVIDIA Halos Outside-In Safety

> **Early access.** This document describes a developer-preview reference
> architecture. It does not define a certified functional-safety product and does
> not make safety claims.

## Overview

Halos Outside-In Safety is a reference architecture for observing a workcell
from the outside, interpreting what is happening in that workcell, and driving a
bounded safety decision to equipment operating inside it.

The initial target scenario is a warehouse workcell such as an autonomous trailer
loading dock. Fixed cameras observe the dock area. An AI perception backend
detects relevant actors, objects, and spatial events. Safety Core consumes those
events, integrates them over time, and emits a compact decision such as whether
the equipment should remain muted or return to its onboard safety behavior.

The repository is intended for research, evaluation, and integration
development. It is useful for understanding the software interfaces, event flow,
and closed-loop behavior of an outside-in safety concept. It is not a substitute
for an independent certified safety system in a real installation.

## Architecture

Halos Outside-In Safety is designed around NVIDIA IGX as the target platform
foundation.
On top of that platform, the runtime path has two main components: vision-based
AI Perception and Safety Core. Closed-Loop Testing sits alongside the runtime
path as an offline development and evaluation loop.

### Platform: NVIDIA IGX

NVIDIA IGX Thor and Halos Core provide the platform foundation for the
architecture. IGX Thor combines Thor SoC compute with dedicated safety hardware
and platform safety extensions. Halos Core provides the operating-system and
runtime environment for safety applications, including safety services,
security services, accelerated compute runtime support, and platform mechanisms
for separation, fault reporting, heartbeat, and supervision.

Halos Outside-In Safety fits this context as an outside-in robotics application
blueprint. This repository provides the application-level interfaces, deployment
profiles, and examples; it does not define the certified IGX safety integration.
The source reflects that split: Safety Core can be built for x86_64 and
aarch64/Tegra targets and packaged as desktop or Tegra artifacts. The deployment
profiles cover local base/SIL workflows and include a HIL
hook where Safety Core runs on a Thor device.

For platform architecture, platform safety assumptions, and integration
responsibilities, refer to the Halos Core documentation and the NVIDIA IGX Thor
in Safety-Related Systems Application Note.

### Runtime: Vision-Based AI Perception

Vision-based AI Perception provides the outside-in view of the workcell. Fixed
cameras observe the area around the equipment, and the perception backend
detects relevant actors, objects, and spatial events.

The reference backend is NVIDIA VSS Blueprint, but the architecture treats
perception as a swappable source of structured events. The integration point is
the event stream consumed by Safety Core, not a particular model or camera
pipeline.

The runtime boundary is: vision-based AI perception detects scene
state and events; Safety Core decides how those events affect the configured
safety behavior.

### Runtime: Safety Core

Safety Core provides monitoring, event integration, decision-making, and command
transport. It ingests perception events through adapters, supports sensor and
event-health monitoring, integrates events over time, runs the configured
decision-maker, and passes the resulting command toward the downstream
interface.

The repository includes reference decision-maker applications, including
Automated Trailer Loading and Proximity. These examples show how perception
events can be mapped into domain-specific decisions. They are provided for
software-in-the-loop and hardware-in-the-loop evaluation, not as production
safety functions.

### Offline: Closed-Loop Testing

Closed-Loop Testing is not a runtime safety component. It is an offline harness
for exercising the full loop before connecting the software to physical
equipment. The harness uses simulation and supporting services to feed
perception, run Safety Core, publish the safety decision, and observe the effect
on simulated equipment behavior.

This loop is useful for integration development because it makes timing,
configuration, and interface failures visible early. It also gives teams a
repeatable environment for comparing profiles such as base, SIL, and future HIL
flows.

## What This Repository Provides

- A reference outside-in event flow from perception to decision output.
- Safety Core source and reference decision-maker applications.
- Deployment profiles for base and SIL evaluation workflows.
- A closed-loop test harness using simulation and supporting services.
- Developer-oriented skills and runbooks for bringing up the stack.

## What This Repository Does Not Provide

- A certified safety function.
- A production safety case.
- A qualified perception model, dataset, or deployment configuration.
- A replacement for equipment-level protective measures.
- Certification evidence for a specific site, machine, or operating envelope.

