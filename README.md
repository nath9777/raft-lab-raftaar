# Raft Implementation in C++ with gRPC

### Overview
This repository contains an implementation of the **Raft Consensus Algorithm**, a fault-tolerant protocol designed for managing replicated logs in distributed systems. Raft ensures consistency and correctness of replicated state machines in the presence of failures such as server crashes or network partitions. This implementation is written in C++ and uses gRPC for communication between Raft nodes.

---


### The project is divided into two major phases:

1. Leader Election: Ensures a single leader is elected in a distributed cluster.
2. Log Replication: Manages consistent log replication across all nodes, ensuring fault-tolerant operation.

This implementation adheres closely to the Raft algorithm as described in the Raft Extended Paper.

---


### Key Features

#### Phase 1: Leader Election
> * Implements the RequestVote RPC to facilitate leader elections.
> * Periodically sends heartbeat messages using the AppendEntries RPC to maintain leadership.
> * Ensures network fault tolerance, where the cluster remains operational if **a majority of nodes** are connected.

#### Phase 2: Log Replication
> * Supports client proposals for state updates, handled exclusively by the leader.
> * Replicates log entries to followers through AppendEntries RPCs.
> * Commits log entries only after receiving majority acknowledgments.
> * Handles network failures and stale requests gracefully, ensuring eventual consistency.

#### Additional Highlights
> * Fault Tolerant: Handles network failures and ensures that Raft will resume normal operation once a majority of servers are reachable
> * Minimizes unnecessary RPCs and ensures consensus is achieved with minimal data transfer and within a reasonable time frame.
> * Supports concurrent log proposals and ensures data consistency even in a multi-node setup.

---

### Codebase Structure
```
.
├── app                 # Bootstrap application to start a Raft node
├── build               # Build output folder
├── cmake               # CMake scripts for gRPC setup
├── generated           # Auto-generated protobuf and gRPC files
├── inc
│   ├── common          # Common utilities shared across modules
│   └── rafty           # Raft implementation header files
├── impl                # Inline function definitions
├── toolings            # Grader tools for testing
├── integration_tests   # Integration test cases
├── libs                # 3rd-party libraries
├── proto               # Protobuf definitions for gRPC
└── src                 # Source code for Raft implementation
```
**NOTE: The main implementation changes are present in these files: ***inc/rafty***, ***proto***, and ***src*****


---

### Architecture

#### Leader Election
Leader election ensures that the cluster has a single, operational leader:
* Each node starts as a follower.
* If a follower does not receive heartbeats within a specified election timeout, it transitions to the candidate state and initiates a leader election.
* The node receiving a majority of votes transitions to the leader state and starts sending periodic heartbeats to maintain authority.

#### Log Replication
Once a leader is elected:
* Client proposals are submitted to the leader via the propose method.
* The leader appends the proposal to its log and sends AppendEntries RPCs to replicate the log on followers.
* The leader commits the log entry once a majority of followers acknowledge replication.


---

### References

* **[Raft Extended Paper](https://raft.github.io/raft.pdf)**
* **[gRPC Documentation](https://grpc.io/docs/)**

---

Contributors
* **[Sashank Agarwal](https://github.com/sasagarw)**
* **[Atul Nath](https://github.com/nath9777)**

---
