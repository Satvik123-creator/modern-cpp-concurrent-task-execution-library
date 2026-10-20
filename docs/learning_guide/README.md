# CTEE Learning Guide — Modular Phases

This directory contains the complete, phase-by-phase learning guide for the **Concurrent Task Execution Engine (CTEE)**.

Each file corresponds to a focused phase of the study plan, designed for modular reading, editor split-screens, and interview preparation.

### Master Document
If you prefer reading the entire guide as a single contiguous document, see:
- [CTEE_COMPLETE_LEARNING_GUIDE.md](../../CTEE_COMPLETE_LEARNING_GUIDE.md) in the project root.

---

### Phase Index

| Phase | Document | Description |
|---|---|---|
| **Phase 1** | [01_phase1_project_overview.md](01_phase1_project_overview.md) | Directory structure, 6 core components, architecture diagram, file dependencies, end-to-end task lifecycle |
| **Phase 2A** | [02_phase2_core_library_files.md](02_phase2_core_library_files.md) | Line-by-line breakdown of all 10 core library files (Task, Queue, Worker, ThreadPool, Scheduler, Metrics, History) |
| **Phase 2B** | [03_phase2b_infrastructure_server.md](03_phase2b_infrastructure_server.md) | Infrastructure & server files (Logger, Config, ThreadNaming, HTTPServer, RouteHandler, TaskRegistry, main.cpp) |
| **Phase 3** | [04_phase3_runtime_flows.md](04_phase3_runtime_flows.md) | Step-by-step runtime flows (Init, Submit, Worker Run Loop, Completion, Auto-scale, Cancel, Shutdown) |
| **Phase 4** | [05_phase4_concurrency_analysis.md](05_phase4_concurrency_analysis.md) | Shared data map, mutex & CV invariants, deadlock analysis, atomics & memory ordering, race condition audit |
| **Phase 5** | [06_phase5_call_graph.md](06_phase5_call_graph.md) | Complete caller/callee Mermaid call graphs, data flow pipelines, object lifetime & ownership map |
| **Phase 6** | [07_phase6_tests_examples.md](07_phase6_tests_examples.md) | In-depth analysis of 326 tests across 19 test files, example programs, coverage gaps, and edge cases |
| **Phase 7** | [08_phase7_interview_guide.md](08_phase7_interview_guide.md) | 30s/1m/3m pitch scripts, 35+ file-specific interview Q&As, architecture deep dives, concurrency questions |

---
