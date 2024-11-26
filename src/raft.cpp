#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <random>

#include "common/utils/rand_gen.hpp"
#include "rafty/raft.hpp"

#include "raft.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace rafty
{
  Raft::Raft(const Config &config, MessageQueue<ApplyResult> &ready)
      : id(config.id),
        listening_addr(config.addr),
        peer_addrs(config.peer_addrs),
        dead(false),
        ready_queue(ready),
        logger(utils::logger::get_logger(id)),
        // TODO: add more field if desired
        currentTerm(0),
        votedFor(-1),
        lastLogIndex(-1),
        lastLogTerm(-1),
        currentRole(Follower),
        commitIndex(0),
        lastApplied(0),
        nextIndex(std::vector<int>(peer_addrs.size(), 1)),
        matchIndex(std::vector<int>(peer_addrs.size(), 0))
  {
    // TODO: finish it
    srand(time(NULL) + id);
    reset_election_timeout();

    next_heartbeat_time = std::chrono::steady_clock::now();
    log.push_back(LogEntry{0, ""});
  }

  Raft::~Raft() { this->stop_server(); }

  void Raft::run()
  {
    // TODO: kick off the raft instance
    // Note: this function should be non-blocking

    // lab 1
    logger->info("Start Raft node with ID {}", id);

    // raft main loop thread
    std::thread([this]()
                {
      while (!is_dead()) {
          std::unique_lock<std::mutex> lock(mtx);          

          auto now = std::chrono::steady_clock::now();


          // Check for election timeout
          if (currentRole != Leader && now >= election_timeout) {
              logger->info("Election timeout, start new election. ");             
              become_candidate();
          }

          // Send heartbeats periodically
          if (currentRole == Leader && now >= next_heartbeat_time) {
              logger->info("Send heartbeats to peers.");
              send_heartbeats();
              next_heartbeat_time = now + std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS);              
          }

          // Wait for next iteration
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));

  } })
        .detach();
  }

  State Raft::get_state() const
  {
    // TODO: finish it
    // lab 1
    std::lock_guard<std::mutex> lock(mtx);
    State state;
    state.term = currentTerm;
    state.is_leader = (currentRole == Leader);
    return state;
  }

  ProposalResult Raft::propose(const std::string &data)
  {
    // TODO: finish it
    // lab 2

    std::lock_guard<std::mutex> lock(mtx);
    if (currentRole != Leader)
    {
      return ProposalResult{1, currentTerm, false};
    }

    appendEntries(data);

    // Start the replication process asynchronously
    // std::thread replication_thread(&Raft::replicateEntries, this);
    // replication_thread.detach();
    replicateEntries();

    logger->info("Exiting from propose function");

    return ProposalResult{log.size() - 1, currentTerm, true};
  }

  void Raft::become_follower(int term)
  {
    logger->info("Node id {} becoming follower.", id);
    currentTerm = term;
    currentRole = Follower;
    votedFor = -1;

    // reset election timeout here, after becoming follower, to prevent new election from starting
    // reset_election_timeout();
  }

  void Raft::become_candidate()
  {
    logger->info("Node id {} becoming candidate.", id);
    currentTerm++;
    currentRole = Candidate;
    votedFor = id;
    start_election();
  }

  void Raft::become_leader()
  {
    logger->info("Node id {} becoming leader.", id);
    currentRole = Leader;
    send_heartbeats();
  }

  void Raft::start_election()
  {
    logger->info("Node {} is starting an election for term {}", id, currentTerm);

    // Reset election timeout
    reset_election_timeout();

    // It votes for itself
    int votesGranted = 1;

    // Get actual last log info
    int64_t last_log_index = log.size() - 1;
    int64_t last_log_term = (last_log_index >= 0) ? log[last_log_index].term : -1;
    logger->info("Node {} trying to be candidate; its last log index: {} and last log term: {} ", id, last_log_index, last_log_term);

    raftpb::RequestVoteRequest request;
    request.set_term(currentTerm);
    request.set_candidateid(id);
    // request.set_lastlogindex(lastLogIndex);
    // request.set_lastlogterm(lastLogTerm);
    request.set_lastlogindex(last_log_index);
    request.set_lastlogterm(last_log_term);

    // Send RequestVote RPCs to all peers
    for (const auto &[peer_id, stub] : peers_)
    {
      std::thread([this, peer_id, stub = stub.get(), &votesGranted, request]() mutable
                  {
              
      auto context = create_context(peer_id);

      raftpb::RequestVoteResponse response;
      grpc::Status status = stub->RequestVote(&*context, request, &response);
      logger->info("Request Vote RPC for term {} called.", getCurrentTerm());

      if (status.ok() && response.votegranted()) {
        logger->info("RequestVote RPC to peer {} success", peer_id);
          std::lock_guard<std::mutex> lock(mtx);
          votesGranted++;
          
          // Check if we have a majority 
          if (votesGranted > (peers_.size() + 1) / 2) {
              logger->info("Node {} became leader for term {}", id, currentTerm);
              become_leader();              
          }
      } else if (status.ok() && response.term() > currentTerm) {
          
          std::lock_guard<std::mutex> lock(mtx);
          become_follower(response.term());          
      } else if (!status.ok()) {          
          logger->error("RequestVote RPC to peer {} failed: {}", peer_id, status.error_message());
      } })
          .detach();
    }
  }

  void Raft::send_heartbeats()
  {
    // Leader sends AppendEntries RPCs (heartbeat) to peers, potentially with log entries
    logger->info("Node {} is sending heartbeats for term {}", id, currentTerm);

    for (const auto &[peer_id, stub] : peers_)
    {
      std::thread([this, peer_id, stub = stub.get()]() mutable
                  {        
            raftpb::AppendEntriesRequest request;
            request.set_term(currentTerm);
            request.set_leaderid(id);

            std::lock_guard<std::mutex> lock(mtx);

            // Consistency check according to paper
            if (nextIndex[peer_id] <= log.size())
            {
                uint64_t prev_log_index = nextIndex[peer_id] - 1;
                request.set_prevlogindex(prev_log_index);
                // request.set_prevlogterm(log[prev_log_index].term);
                request.set_prevlogterm((prev_log_index == -1) ? -1 : log[prev_log_index].term);

                for (size_t i = nextIndex[peer_id]; i < log.size(); ++i)
                {
                    auto *entry = request.add_entries();
                    entry->set_term(log[i].term);
                    entry->set_command(log[i].command);
                }
            }
            else
            {
                // normal heartbeat?
                if (log.size() == 0)
                {
                  request.set_prevlogindex(-1);
                  request.set_prevlogterm(-1);
                }                
                else
                {                  
                  request.set_prevlogindex(log.size()-1);
                  request.set_prevlogterm(log.back().term);
                }
            }

            request.set_leadercommit(commitIndex);

            auto context = create_context(peer_id);

            raftpb::AppendEntriesResponse response;
            grpc::Status status = stub->AppendEntries(&*context, request, &response);

            if (status.ok() && response.term() <= currentTerm)
            {
              logger->info("AppendEntries RPC to peer {} success", peer_id);

              if (!request.entries().empty() && response.success()) {
                  nextIndex[peer_id] = log.size() + 1;  
                  matchIndex[peer_id] = log.size();     
              }
            }
            else if (status.ok() && response.term() > currentTerm)
            {
                logger->error("Response term:{} > Current Term:{}", response.term(), currentTerm);
                become_follower(response.term()); 
            }
            else
            {
              logger->error("AppendEntries RPC to peer {} failed: {}", peer_id, status.error_message());

              nextIndex[peer_id] = std::max(1, nextIndex[peer_id] - 1);
            } })
          .detach();
    }
  }

  void Raft::reset_election_timeout()
  {
    int random_timeout_ms = ELECTION_TIMEOUT_MIN_MS + (rand() % (ELECTION_TIMEOUT_MAX_MS - ELECTION_TIMEOUT_MIN_MS));
    election_timeout = std::chrono::steady_clock::now() + std::chrono::milliseconds(random_timeout_ms);
    logger->info("Node {} election timeout set to {}ms", id, random_timeout_ms);
  }

  int64_t Raft::getCurrentTerm() const
  {
    return currentTerm;
  }

  void Raft::setCurrentTerm(int64_t term)
  {
    currentTerm = term;
  }

  int Raft::getVotedFor() const
  {
    return votedFor;
  }

  void Raft::setVotedFor(int candidateId)
  {
    votedFor = candidateId;
  }

  int64_t Raft::getLastLogIndex() const
  {
    return lastLogIndex;
  }

  int64_t Raft::getLastLogTerm() const
  {
    return lastLogTerm;
  }

  void Raft::appendEntries(const std::string &data)
  {
    logger->info("Appending {} to logs", data);
    LogEntry entry;
    entry.term = currentTerm;
    entry.command = data;
    log.push_back(entry);
  }

  void Raft::replicateEntries()
  {
    logger->info("Node {} is replicating enteries for term {}", id, currentTerm);
    // send log entry to all other followers

    for (const auto &[peer_id, stub] : peers_)
    {
      if (peer_id == id)
        continue;

      if (nextIndex[peer_id] < 1)
      {
        nextIndex[peer_id] = 1;
      }
      if (nextIndex[peer_id] > log.size())
      {
        nextIndex[peer_id] = log.size();
      }

      raftpb::AppendEntriesRequest request;
      request.set_term(currentTerm);
      request.set_leaderid(id);
      request.set_prevlogindex(nextIndex[peer_id] - 1);
      request.set_prevlogterm(log[nextIndex[peer_id] - 1].term);
      request.set_leadercommit(commitIndex);

      for (int i = nextIndex[peer_id]; i < log.size(); ++i)
      {
        // *request.add_entries() = log[i];
        raftpb::Entry *entry = request.add_entries();
        entry->set_term(log[i].term);
        entry->set_command(log[i].command);
      }

      auto context = create_context(peer_id);
      raftpb::AppendEntriesResponse response;
      grpc::Status status = stub->AppendEntries(&*context, request, &response);

      if (status.ok())
      {
        logger->info("AppendEntries RPC for log replicate to peer {} success", peer_id);
        if (response.success())
        {
          matchIndex[peer_id] = log.size() - 1;
          nextIndex[peer_id] = log.size();
        }
        else
        {
          if (nextIndex[peer_id] > 0)
          {
            nextIndex[peer_id]--;
          }
        }
      }
    }

    updateCommitIndex();
    applyLogEntries();
  }

  void Raft::updateCommitIndex()
  {
    // update commit index
    logger->info("Entered to update commit index");
    for (int N = commitIndex + 1; N < log.size(); ++N)
    {
      int replicationCount = 1; // Count self
      for (const auto &[peer_id, _] : peers_)
      {
        if (matchIndex[peer_id] >= N)
        {
          replicationCount++;
        }
      }

      if (replicationCount > (peers_.size()) / 2 && log[N].term == currentTerm)
      {
        commitIndex = N;
      }
      else
      {
        break;
      }
    }
  }

  void Raft::applyLogEntries()
  {
    while (commitIndex > lastApplied)
    {
      lastApplied++;
      apply(ApplyResult{true, log[lastApplied].command, lastApplied});
      logger->info("Applied command {} to state machine at index {}", log[lastApplied].command, lastApplied);
    }
  }

  // TODO: add more functions if desired.
  using grpc::ServerContext;
  using grpc::Status;

  Status
  RaftServiceHandler::RequestVote(ServerContext *context,
                                  const raftpb::RequestVoteRequest *request,
                                  raftpb::RequestVoteResponse *response)
  {
    std::lock_guard<std::mutex> lock(raft->mtx);

    response->set_term(raft->getCurrentTerm());
    response->set_votegranted(false);

    if (request->term() < raft->getCurrentTerm())
    {
      return Status::OK;
    }

    if (request->term() > raft->getCurrentTerm())
    {
      raft->become_follower(request->term());
    }

    int64_t curr_last_term = -1;
    if (!raft->log.empty())
    {
      curr_last_term = raft->log.back().term;
    }
    int64_t curr_last_index = raft->log.size() - 1;

    bool log_is_up_to_date = false;

    if (request->lastlogterm() > curr_last_term)
    {
      log_is_up_to_date = true;
    }
    else if (request->lastlogterm() == curr_last_term)
    {
      log_is_up_to_date = (request->lastlogindex() >= curr_last_index);
    }

    // Grant vote if:
    // 1. Haven't voted for anyone else in this term
    // 2. Candidate's log is at least as up-to-date as ours
    if ((raft->getVotedFor() == -1 || raft->getVotedFor() == request->candidateid()) &&
        log_is_up_to_date)
    {

      raft->logger->info("Granting vote to candidate {} in term {}. -- log - lastTerm: {}, lastIndex: {}. Candidate log - lastTerm: {}, lastIndex: {}",
                         request->candidateid(),
                         request->term(),
                         curr_last_term,
                         curr_last_index,
                         request->lastlogterm(),
                         request->lastlogindex());

      raft->setVotedFor(request->candidateid());
      response->set_votegranted(true);

      raft->reset_election_timeout();
    }
    else
    {
      raft->logger->info("Rejecting vote for candidate {} in term {}. My log - lastTerm: {}, lastIndex: {}. Candidate log - lastTerm: {}, lastIndex: {}",
                         request->candidateid(),
                         request->term(),
                         curr_last_term,
                         curr_last_index,
                         request->lastlogterm(),
                         request->lastlogindex());
    }

    return Status::OK;
  }

  Status
  RaftServiceHandler::AppendEntries(ServerContext *context,
                                    const raftpb::AppendEntriesRequest *request,
                                    raftpb::AppendEntriesResponse *response)
  {
    // Implement the AppendEntries handler logic here
    // std::lock_guard<std::mutex> lock(raft->mtx);

    response->set_term(raft->getCurrentTerm());
    response->set_success(false);
    if (request->term() < raft->getCurrentTerm())
    {
      return Status::OK;
    }
    else
    {
      raft->become_follower(request->term());
    }

    if (request->term() == raft->getCurrentTerm() &&
        raft->currentRole == Leader)
    {
      raft->logger->info("Stepping down - saw AppendEntries from another leader in same term");
      raft->become_follower(request->term());
    }

    raft->reset_election_timeout();

    raft->logger->info("prevLogIndex {}: prevlogterm {}, term inside log at prev log index {}",
                       request->prevlogindex(), request->prevlogterm(),
                       raft->log[request->prevlogindex()].term);

    raft->logger->info("log size {}",
                       raft->log.size());

    if (request->prevlogindex() > raft->log.size() - 1 ||
        (request->prevlogindex() >= 0 &&
         raft->log[request->prevlogindex()].term != request->prevlogterm()))
    {
      return Status::OK;
    }

    for (int i = 0; i < request->entries_size(); ++i)
    {
      if (request->prevlogindex() + 1 + i >= raft->log.size())
      {
        raft->log.push_back(LogEntry{request->entries(i).term(), request->entries(i).command()});
      }
      else if (raft->log[request->prevlogindex() + 1 + i].term != request->entries(i).term())
      {
        raft->log.erase(raft->log.begin() + request->prevlogindex() + 1 + i, raft->log.end());
        raft->log.push_back(LogEntry{request->entries(i).term(), request->entries(i).command()});
      }
    }

    if (request->leadercommit() > raft->commitIndex)
    {
      raft->commitIndex = std::min(request->leadercommit(), (int64_t)raft->log.size() - 1);
    }

    raft->applyLogEntries();

    response->set_success(true);
    return Status::OK;
  }

} // namespace rafty
