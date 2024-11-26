#pragma once

#include "raft.grpc.pb.h"
#include "rafty/raft.hpp"
#include <grpcpp/grpcpp.h>

using grpc::ServerContext;
using grpc::Status;

namespace rafty {
class Raft;
class RaftServiceHandler final : public raftpb::RaftService::Service {
    public:
        explicit RaftServiceHandler(Raft* raft_instance) : raft(raft_instance) {}
    private:
        Status RequestVote(ServerContext* context,
                            const raftpb::RequestVoteRequest* request,
                            raftpb::RequestVoteResponse* response) override;
        Status AppendEntries(ServerContext* context,
                            const raftpb::AppendEntriesRequest* request,
                            raftpb::AppendEntriesResponse* response) override;
    
        Raft* raft;
};
}
